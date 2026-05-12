#include "SessionManager.hpp"
#include "Json.hpp"
#include "SecureFile.hpp"

#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/debug/log/Logger.hpp>
#include <hyprland/src/desktop/Workspace.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/plugins/PluginAPI.hpp>

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <unistd.h>

namespace fs = std::filesystem;
using namespace std::chrono;

namespace {
static std::string readFile(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return {};
    }
    std::ostringstream content;
    content << file.rdbuf();
    return content.str();
}

static std::string basename(const std::string& p) {
    auto pos = p.find_last_of('/');
    return pos == std::string::npos ? p : p.substr(pos + 1);
}

}

CSessionManager::CSessionManager(HANDLE handle) : m_handle(handle) {
    const char* home = std::getenv("HOME");
    fs::path base = home ? fs::path(home) / ".cache" / "hypr" : fs::path("/tmp/hypr");
    m_cacheDir = base;
    m_sessionFile = base / "session.json";
    m_backupFile = base / "session.json.1";
    m_restoreLock = base / "restoring.lock";
    m_restoreLog = base / "restore-history";
}

CSessionManager::~CSessionManager() {
    shutdown();
}

void CSessionManager::init() {
    m_pluginStart = steady_clock::now();
    m_lastEvent = m_pluginStart;

    auto& B = Event::bus();
    std::function<void()> evt = [this] { onWindowEvent(); };

    m_listeners.emplace_back(B->m_events.window.open.listen(evt));
    m_listeners.emplace_back(B->m_events.window.close.listen(evt));
    m_listeners.emplace_back(B->m_events.window.destroy.listen(evt));
    m_listeners.emplace_back(B->m_events.window.title.listen(evt));
    m_listeners.emplace_back(B->m_events.window.fullscreen.listen(evt));
    m_listeners.emplace_back(B->m_events.window.moveToWorkspace.listen(evt));
    m_listeners.emplace_back(B->m_events.tick.listen(std::function<void()>([this] { onTick(); })));

    Log::logger->log(Log::INFO, "[hypr-session] plugin loaded; cache at {}", m_cacheDir.string());
}

void CSessionManager::shutdown() {
    m_listeners.clear();
}

void CSessionManager::onWindowEvent() {
    m_dirty = true;
    m_lastEvent = steady_clock::now();
}

void CSessionManager::onTick() {
    auto now = steady_clock::now();

    if (!m_restoreAttempted && now - m_pluginStart >= seconds(m_startupDelaySecs)) {
        m_restoreAttempted = true;
        try {
            restoreNow();
        } catch (const std::exception& e) {
            Log::logger->log(Log::ERR, "[hypr-session] restore threw: {}", e.what());
        }
        m_dirty = false;
        m_lastEvent = steady_clock::now();
        return;
    }

    if (m_dirty && now - m_lastEvent >= seconds(m_debounceSecs) && !restoreLockExists()) {
        m_dirty = false;
        try {
            snapshotNow();
        } catch (const std::exception& e) {
            Log::logger->log(Log::ERR, "[hypr-session] snapshot threw: {}", e.what());
        }
    }
}

bool CSessionManager::readProc(pid_t pid, std::vector<std::string>& argv, std::string& cwd) {
    argv.clear();
    cwd.clear();
    fs::path procDir = fs::path("/proc") / std::to_string(pid);
    std::string raw = readFile(procDir / "cmdline");
    if (raw.empty())
        return false;
    std::string cur;
    for (char c : raw) {
        if (c == '\0') {
            if (!cur.empty())
                argv.push_back(std::move(cur));
            cur.clear();
        } else
            cur.push_back(c);
    }
    if (!cur.empty())
        argv.push_back(std::move(cur));
    if (argv.empty())
        return false;

    char buf[PATH_MAX];
    ssize_t n = readlink((procDir / "cwd").c_str(), buf, sizeof(buf) - 1);
    if (n > 0) {
        buf[n] = '\0';
        cwd = buf;
    }
    return true;
}

std::vector<SWindowSnapshot> CSessionManager::collectWindows() const {
    std::vector<SWindowSnapshot> out;
    std::unordered_set<pid_t> seen;

    for (const auto& w : g_pCompositor->m_windows) {
        if (!w || w->m_fadingOut || w->isHidden())
            continue;

        pid_t pid = w->getPID();
        if (pid <= 0 || seen.count(pid))
            continue;

        std::vector<std::string> argv;
        std::string cwd;
        if (!readProc(pid, argv, cwd))
            continue;

        std::string base = basename(argv[0]);
        if (m_skipBasenames.count(base))
            continue;

        auto ws = w->m_workspace;
        if (!ws)
            continue;
        int wsId = static_cast<int>(ws->m_id);
        if (wsId < 0)
            continue;

        seen.insert(pid);

        SWindowSnapshot s;
        s.cmdline = std::move(argv);
        s.cwd = std::move(cwd);
        s.klass = w->m_class;
        s.title = w->m_title;
        s.workspaceId = wsId;
        s.workspaceName = ws->m_name.empty() ? std::to_string(wsId) : ws->m_name;
        s.floating = w->m_isFloating;
        s.fullscreen = w->isFullscreen();

        auto pos = w->m_realPosition->value();
        auto size = w->m_realSize->value();
        s.atX = pos.x;
        s.atY = pos.y;
        s.sizeW = size.x;
        s.sizeH = size.y;
        out.push_back(std::move(s));
    }
    return out;
}

void CSessionManager::writeSnapshot(const std::vector<SWindowSnapshot>& windows) {
    if (windows.empty())
        return;

    using namespace hsrjson;
    Array winArr;
    winArr.reserve(windows.size());
    for (const auto& w : windows) {
        Array cmd;
        for (const auto& a : w.cmdline)
            cmd.emplace_back(a);
        Array at;
        at.emplace_back(w.atX);
        at.emplace_back(w.atY);
        Array sz;
        sz.emplace_back(w.sizeW);
        sz.emplace_back(w.sizeH);
        Object obj;
        obj["cmdline"] = std::move(cmd);
        obj["cwd"] = w.cwd;
        obj["class"] = w.klass;
        obj["title"] = w.title;
        obj["workspace_id"] = w.workspaceId;
        obj["workspace_name"] = w.workspaceName;
        obj["floating"] = w.floating;
        obj["fullscreen"] = w.fullscreen;
        obj["at"] = std::move(at);
        obj["size"] = std::move(sz);
        winArr.emplace_back(std::move(obj));
    }
    Object root;
    root["schema_version"] = HSR_SNAPSHOT_SCHEMA_VERSION;
    root["ts"] =
        static_cast<double>(duration_cast<seconds>(system_clock::now().time_since_epoch()).count());
    root["windows"] = std::move(winArr);

    if (!ensureSecureCacheDir(m_cacheDir)) {
        Log::logger->log(Log::ERR, "[hypr-session] insecure cache dir {}; refusing snapshot",
                         m_cacheDir.string());
        return;
    }
    if (isSecureRegularFile(m_sessionFile)) {
        std::error_code ec;
        fs::rename(m_sessionFile, m_backupFile, ec);
        static_cast<void>(::chmod(m_backupFile.c_str(), HSR_SECURE_FILE_MODE));
    }
    if (!writeFileAtomic(m_sessionFile, serialize(Value(std::move(root)), 2)))
        Log::logger->log(Log::ERR, "[hypr-session] failed to write {}", m_sessionFile.string());
}

void CSessionManager::snapshotNow() {
    if (restoreLockExists())
        return;
    writeSnapshot(collectWindows());
}

std::optional<std::vector<SWindowSnapshot>> CSessionManager::readSnapshot() const {
    for (const auto& path : {m_sessionFile, m_backupFile}) {
        auto secureContent = readSecureFile(path);
        if (!secureContent) {
            std::error_code ec;
            if (fs::exists(path, ec)) {
                Log::logger->log(Log::WARN, "[hypr-session] refusing insecure or oversized {}",
                                 path.string());
            }
            continue;
        }
        std::string raw = std::move(*secureContent);
        if (raw.empty())
            continue;
        auto parsed = hsrjson::parse(raw);
        if (!parsed || !parsed->isObject()) {
            Log::logger->log(Log::WARN, "[hypr-session] bad {} (parse failed)", path.string());
            continue;
        }
        if (auto* schema = parsed->find("schema_version");
            schema && schema->numOr(HSR_SNAPSHOT_SCHEMA_VERSION) != HSR_SNAPSHOT_SCHEMA_VERSION) {
            Log::logger->log(Log::WARN, "[hypr-session] unsupported snapshot schema in {}",
                             path.string());
            continue;
        }
        const auto* wins = parsed->find("windows");
        if (!wins || !wins->isArray() || wins->arr().empty() ||
            wins->arr().size() > HSR_MAX_WINDOWS) {
            continue;
        }

        if (path == m_backupFile)
            Log::logger->log(Log::INFO, "[hypr-session] primary missing, using {}",
                             path.filename().string());

        std::vector<SWindowSnapshot> out;
        for (const auto& wv : wins->arr()) {
            if (!wv.isObject())
                continue;
            SWindowSnapshot s;
            if (auto* v = wv.find("cmdline"); v && v->isArray())
                for (const auto& a : v->arr())
                    if (a.isString())
                        s.cmdline.push_back(a.str());
            if (s.cmdline.empty())
                continue;
            if (auto* v = wv.find("cwd"))
                s.cwd = v->strOr("");
            if (auto* v = wv.find("class"))
                s.klass = v->strOr("");
            if (auto* v = wv.find("title"))
                s.title = v->strOr("");
            if (auto* v = wv.find("workspace_id"))
                s.workspaceId = static_cast<int>(v->numOr(1));
            if (auto* v = wv.find("workspace_name"))
                s.workspaceName = v->strOr("");
            if (auto* v = wv.find("floating"))
                s.floating = v->boolOr(false);
            if (auto* v = wv.find("fullscreen"))
                s.fullscreen = v->boolOr(false);
            if (auto* v = wv.find("at"); v && v->isArray() && v->arr().size() == 2) {
                s.atX = v->arr()[0].numOr(0);
                s.atY = v->arr()[1].numOr(0);
            }
            if (auto* v = wv.find("size"); v && v->isArray() && v->arr().size() == 2) {
                s.sizeW = v->arr()[0].numOr(0);
                s.sizeH = v->arr()[1].numOr(0);
            }
            if (validateSnapshot(s)) {
                out.push_back(std::move(s));
            }
        }
        if (out.empty())
            continue;
        return out;
    }
    return std::nullopt;
}

bool CSessionManager::restoreLockExists() const {
    std::error_code ec;
    auto st = fs::last_write_time(m_restoreLock, ec);
    if (ec)
        return false;
    auto age = fs::file_time_type::clock::now() - st;
    if (age > seconds(m_restoreLockMaxAgeSecs)) {
        fs::remove(m_restoreLock, ec);
        return false;
    }
    return true;
}

void CSessionManager::touchRestoreLock() const {
    if (!ensureSecureCacheDir(m_cacheDir)) {
        return;
    }
    static_cast<void>(writeFileAtomic(m_restoreLock, ""));
}

void CSessionManager::removeRestoreLock() const {
    std::error_code ec;
    fs::remove(m_restoreLock, ec);
}

bool CSessionManager::crashLoopTripped() {
    auto nowSec = duration_cast<seconds>(system_clock::now().time_since_epoch()).count();

    std::vector<int64_t> history;
    if (auto rawHistory = readSecureFile(m_restoreLog)) {
        std::istringstream historyStream(*rawHistory);
        std::string line;
        while (std::getline(historyStream, line)) {
            try {
                history.push_back(std::stoll(line));
            } catch (const std::invalid_argument&) {
                continue;
            } catch (const std::out_of_range&) {
                continue;
            }
        }
    }
    history.erase(std::remove_if(history.begin(), history.end(),
                                 [&](int64_t t) { return nowSec - t >= m_crashLoopWindowSecs; }),
                  history.end());

    bool tripped = static_cast<int>(history.size()) >= m_crashLoopLimit;
    history.push_back(nowSec);
    if (history.size() > 20)
        history.erase(history.begin(), history.end() - 20);

    if (!ensureSecureCacheDir(m_cacheDir)) {
        return true;
    }

    std::ostringstream out;
    for (auto t : history)
        out << t << '\n';
    if (!writeFileAtomic(m_restoreLog, out.str()))
        Log::logger->log(Log::WARN, "[hypr-session] failed to write {}", m_restoreLog.string());
    return tripped;
}

std::unordered_map<std::string, int> CSessionManager::currentClassCounts() const {
    std::unordered_map<std::string, int> m;
    for (const auto& w : g_pCompositor->m_windows) {
        if (!w || w->m_fadingOut || w->isHidden())
            continue;
        ++m[w->m_class.empty() ? std::string("?") : w->m_class];
    }
    return m;
}

std::vector<SWindowSnapshot>
CSessionManager::filterAlreadyRunning(std::vector<SWindowSnapshot> want,
                                      const std::unordered_map<std::string, int>& running) const {

    std::unordered_map<std::string, int> needed;
    for (const auto& w : want)
        ++needed[w.klass.empty() ? std::string("?") : w.klass];
    for (auto& [cls, n] : needed) {
        auto it = running.find(cls);
        if (it != running.end())
            n = std::max(0, n - it->second);
    }
    std::vector<SWindowSnapshot> out;
    for (auto& w : want) {
        std::string cls = w.klass.empty() ? std::string("?") : w.klass;
        if (needed[cls] > 0) {
            out.push_back(std::move(w));
            --needed[cls];
        }
    }
    return out;
}

std::optional<std::string> CSessionManager::buildDispatchExec(const SWindowSnapshot& w) {
    return buildDispatchExecCommand(w);
}

void CSessionManager::dispatchExec(const SWindowSnapshot& window) const {
    auto full = buildDispatchExec(window);
    if (!full) {
        Log::logger->log(Log::WARN, "[hypr-session] refusing invalid snapshot entry");
        return;
    }
    HyprlandAPI::invokeHyprctlCommand("dispatch", "exec " + *full);
}

void CSessionManager::restoreNow() {
    if (!ensureSecureCacheDir(m_cacheDir)) {
        Log::logger->log(Log::ERR, "[hypr-session] insecure cache dir {}; refusing restore",
                         m_cacheDir.string());
        return;
    }
    touchRestoreLock();
    struct LockGuard {
        const CSessionManager* mgr;
        explicit LockGuard(const CSessionManager* manager) : mgr(manager) {}
        LockGuard(const LockGuard&) = delete;
        LockGuard& operator=(const LockGuard&) = delete;
        ~LockGuard() {
            mgr->removeRestoreLock();
        }
    } guard{this};

    auto snap = readSnapshot();
    if (!snap) {
        Log::logger->log(Log::INFO, "[hypr-session] no snapshot to restore");
        return;
    }
    auto windows = std::move(*snap);
    Log::logger->log(Log::INFO, "[hypr-session] snapshot has {} apps", windows.size());

    if (crashLoopTripped()) {
        Log::logger->log(Log::WARN,
                         "[hypr-session] crash loop detected ({}+ restores in {}s); "
                         "refusing to relaunch. rm {} to reset",
                         m_crashLoopLimit, m_crashLoopWindowSecs, m_restoreLog.string());
        return;
    }

    auto running = currentClassCounts();
    auto before = windows.size();
    windows = filterAlreadyRunning(std::move(windows), running);
    auto skipped = before - windows.size();
    if (skipped)
        Log::logger->log(Log::INFO, "[hypr-session] skipping {} entries already running", skipped);
    if (windows.empty()) {
        Log::logger->log(Log::INFO, "[hypr-session] nothing left to launch");
        return;
    }

    for (const auto& w : windows) {
        dispatchExec(w);
        usleep(static_cast<useconds_t>(m_launchGapSecs * 1'000'000.0));
    }
}
