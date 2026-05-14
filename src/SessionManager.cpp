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
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <ctime>
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

static void scrub(std::string& s) {
    if (!s.empty())
        ::explicit_bzero(s.data(), s.size());
    s.clear();
}

static void scrub(std::vector<std::string>& v) {
    for (auto& s : v)
        scrub(s);
    v.clear();
}

struct STerminalSpec {
    std::vector<std::string> execFlags;
};

// Meyers singletons — defer construction until first call so a hypothetical
// OOM during the initializer is catchable by the plugin loader instead of
// triggering std::terminate during dynamic-load static init.
static const std::unordered_set<std::string>& shellBasenames() {
    static const std::unordered_set<std::string> S = {
        "bash", "zsh", "fish", "sh", "dash", "ash", "ksh", "tcsh", "csh", "nu", "elvish", "xonsh",
    };
    return S;
}

static const std::unordered_set<std::string>& sensitiveBasenames() {
    static const std::unordered_set<std::string> S = {
        "ssh",     "scp",       "sftp",      "rsync",      "gpg",       "gpg2",
        "age",     "sudo",      "doas",      "su",         "pkexec",    "aws",
        "gh",      "glab",      "op",        "pass",       "bw",        "secret-tool",
        "keyring", "curl",      "wget",      "httpie",     "http",      "psql",
        "mysql",   "mysqldump", "pg_dump",   "mongosh",    "redis-cli", "vault",
        "kubectl", "helm",      "terraform", "tofu",       "ansible",   "ansible-vault",
        "openssl", "keytool",   "ssh-add",   "ssh-keygen",
    };
    return S;
}

static const std::unordered_map<std::string, STerminalSpec>& terminalClasses() {
    static const std::unordered_map<std::string, STerminalSpec> M = {
        {"kitty", {{"--"}}},
        {"foot", {{"--"}}},
        {"alacritty", {{"-e"}}},
        {"Alacritty", {{"-e"}}},
        {"org.wezfurlong.wezterm", {{"start", "--"}}},
        {"wezterm", {{"start", "--"}}},
        {"com.mitchellh.ghostty", {{"-e"}}},
        {"ghostty", {{"-e"}}},
        {"xterm", {{"-e"}}},
        {"XTerm", {{"-e"}}},
        {"st-256color", {{"-e"}}},
        {"st", {{"-e"}}},
        {"URxvt", {{"-e"}}},
    };
    return M;
}

static std::vector<pid_t> readChildPids(pid_t pid) {
    std::vector<pid_t> out;
    fs::path file =
        fs::path("/proc") / std::to_string(pid) / "task" / std::to_string(pid) / "children";
    std::string content = readFile(file);
    std::istringstream ss(content);
    pid_t child = 0;
    while (ss >> child)
        if (child > 0)
            out.push_back(child);
    return out;
}

struct SProcStat {
    pid_t ppid = 0;
    unsigned long long starttime = 0;
};

// /proc/PID/stat parsing. `comm` (field 2) is paren-wrapped and may contain
// arbitrary bytes including spaces and close-parens, so we anchor on the LAST
// ')' and parse the rest space-delimited. ppid = field 4, starttime = field 22.
static std::optional<SProcStat> readStat(pid_t pid) {
    std::string s = readFile(fs::path("/proc") / std::to_string(pid) / "stat");
    if (s.empty())
        return std::nullopt;
    auto close = s.rfind(')');
    if (close == std::string::npos || close + 2 >= s.size())
        return std::nullopt;
    std::istringstream rest(s.substr(close + 2));
    char state = 0;
    pid_t ppid = 0;
    if (!(rest >> state >> ppid))
        return std::nullopt;
    std::string skip;
    for (int i = 0; i < 17; ++i)
        if (!(rest >> skip))
            return std::nullopt;
    unsigned long long starttime = 0;
    if (!(rest >> starttime))
        return std::nullopt;
    return SProcStat{ppid, starttime};
}

// Walk /proc descendant tree, picking at each step the child with the most
// recent start-time (true "most recently spawned"; kernel does NOT order
// /proc/PID/task/PID/children by spawn time). Children whose ppid no longer
// matches the parent are dropped — guards against PID-reuse where /proc reads
// races a dead-and-recycled descendant. Bounded depth to avoid loops.
static pid_t findDeepestChild(pid_t root) {
    pid_t cur = root;
    for (int depth = 0; depth < 16; ++depth) {
        auto kids = readChildPids(cur);
        pid_t pick = 0;
        unsigned long long bestStart = 0;
        for (pid_t k : kids) {
            auto st = readStat(k);
            if (!st || st->ppid != cur)
                continue;
            if (pick == 0 || st->starttime > bestStart) {
                pick = k;
                bestStart = st->starttime;
            }
        }
        if (pick == 0)
            return cur;
        cur = pick;
    }
    return cur;
}

} // namespace

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
    m_lastSnapshot = m_pluginStart;

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
    // Hyprlang::CConfigValue::getDataStaticPtr() returns void*const* and the
    // public API instructs callers to reinterpret to the concrete type matching
    // the registered Hyprlang::INT / Hyprlang::STRING. There is no safer cast
    // available at this boundary.
    auto getInt = [this](const char* name) {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
        return reinterpret_cast<Hyprlang::INT* const*>(
            HyprlandAPI::getConfigValue(m_handle, name)->getDataStaticPtr());
    };
    auto getStr = [this](const char* name) {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
        return reinterpret_cast<Hyprlang::STRING* const*>(
            HyprlandAPI::getConfigValue(m_handle, name)->getDataStaticPtr());
    };
    m_pcEnabled = getInt("plugin:hypr-session:enabled");
    m_pcDebounce = getInt("plugin:hypr-session:debounce_secs");
    m_pcPeriodic = getInt("plugin:hypr-session:periodic_secs");
    m_pcStartupDelay = getInt("plugin:hypr-session:startup_delay_secs");
    m_pcCrashWindow = getInt("plugin:hypr-session:crash_loop_window_secs");
    m_pcCrashLimit = getInt("plugin:hypr-session:crash_loop_limit");
    m_pcLaunchGapMs = getInt("plugin:hypr-session:launch_gap_ms");
    m_pcExtraSkip = getStr("plugin:hypr-session:extra_skip");
    m_pcExtraSensitive = getStr("plugin:hypr-session:extra_sensitive");
#pragma GCC diagnostic pop
    refreshConfig();

    if (m_crashLoopWindowSecs <= 0)
        m_crashLoopWindowSecs = 180;
    if (m_crashLoopLimit <= 0)
        m_crashLoopLimit = 3;
    if (m_debounceSecs <= 0)
        m_debounceSecs = 3;
    if (m_periodicSnapshotSecs <= 0)
        m_periodicSnapshotSecs = 30;

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

void CSessionManager::refreshConfig() {
    if (m_pcEnabled && *m_pcEnabled)
        m_enabled = **m_pcEnabled != 0;
    if (m_pcDebounce && *m_pcDebounce && **m_pcDebounce > 0)
        m_debounceSecs = static_cast<int>(**m_pcDebounce);
    if (m_pcPeriodic && *m_pcPeriodic && **m_pcPeriodic > 0)
        m_periodicSnapshotSecs = static_cast<int>(**m_pcPeriodic);
    if (m_pcStartupDelay && *m_pcStartupDelay && **m_pcStartupDelay >= 0)
        m_startupDelaySecs = static_cast<int>(**m_pcStartupDelay);
    if (m_pcCrashWindow && *m_pcCrashWindow && **m_pcCrashWindow > 0)
        m_crashLoopWindowSecs = static_cast<int>(**m_pcCrashWindow);
    if (m_pcCrashLimit && *m_pcCrashLimit && **m_pcCrashLimit > 0)
        m_crashLoopLimit = static_cast<int>(**m_pcCrashLimit);
    if (m_pcLaunchGapMs && *m_pcLaunchGapMs && **m_pcLaunchGapMs >= 0)
        m_launchGapSecs = static_cast<double>(**m_pcLaunchGapMs) / 1000.0;

    auto refreshSet = [](Hyprlang::STRING* const* p, std::string& strCache,
                         std::unordered_set<std::string>& out) {
        if (!p || !*p)
            return;
        Hyprlang::STRING cur = **p;
        std::string s = cur ? cur : "";
        if (s == strCache)
            return;
        strCache = std::move(s);
        out.clear();
        std::string tok;
        for (char c : strCache) {
            if (c == ',' || c == ' ' || c == '\t' || c == '\n') {
                if (!tok.empty()) {
                    out.insert(tok);
                    tok.clear();
                }
            } else
                tok.push_back(c);
        }
        if (!tok.empty())
            out.insert(std::move(tok));
    };
    refreshSet(m_pcExtraSkip, m_lastExtraSkip, m_extraSkipBasenames);
    refreshSet(m_pcExtraSensitive, m_lastExtraSensitive, m_extraSensitiveBasenames);
}

void CSessionManager::onTick() {
    refreshConfig();
    if (!m_enabled)
        return;

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

    bool debouncedDirty = m_dirty && now - m_lastEvent >= seconds(m_debounceSecs);
    bool periodicDue = now - m_lastSnapshot >= seconds(m_periodicSnapshotSecs);

    if ((debouncedDirty || periodicDue) && !restoreLockExists()) {
        m_dirty = false;
        try {
            snapshotNow();
            m_lastSnapshot = now;
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

    if (!g_pCompositor)
        return out;

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
        if (m_skipBasenames.count(base) || m_extraSkipBasenames.count(base)) {
            scrub(argv);
            scrub(cwd);
            continue;
        }

        auto ws = w->m_workspace;
        if (!ws) {
            scrub(argv);
            scrub(cwd);
            continue;
        }
        int wsId = static_cast<int>(ws->m_id);
        if (wsId < 0) {
            scrub(argv);
            scrub(cwd);
            continue;
        }

        seen.insert(pid);

        // Terminal foreground capture: if the window is a known terminal,
        // walk the process tree to the deepest descendant and (only if it's
        // not a shell or a sensitive-arg program) record `<terminal> <flag> <argv0>`
        // with the child's cwd. We deliberately drop ALL args of the foreground
        // program — argv often carries secrets (tokens, passwords, hosts), and
        // this snapshot lands on disk where backups can scrape it.
        const auto& terms = terminalClasses();
        if (auto termIt = terms.find(w->m_class); termIt != terms.end()) {
            pid_t deepPid = findDeepestChild(pid);
            if (deepPid != pid) {
                std::vector<std::string> childArgv;
                std::string childCwd;
                if (readProc(deepPid, childArgv, childCwd) && !childArgv.empty()) {
                    std::string childBase = basename(childArgv[0]);
                    if (!shellBasenames().count(childBase) &&
                        !sensitiveBasenames().count(childBase) &&
                        !m_extraSensitiveBasenames.count(childBase) && !childBase.empty() &&
                        childBase[0] != '-') {
                        std::vector<std::string> wrapped;
                        wrapped.reserve(2 + termIt->second.execFlags.size());
                        wrapped.push_back(base);
                        for (const auto& f : termIt->second.execFlags)
                            wrapped.push_back(f);
                        wrapped.push_back(childBase);
                        scrub(argv);
                        argv = std::move(wrapped);
                        if (!childCwd.empty()) {
                            scrub(cwd);
                            cwd = std::move(childCwd);
                        }
                    }
                    scrub(childArgv);
                    scrub(childCwd);
                }
            }
        }

        SWindowSnapshot s;
        s.cmdline = std::move(argv);
        s.cwd = std::move(cwd);
        s.klass = w->m_class;
        s.title = w->m_title;
        s.workspaceId = wsId;
        s.workspaceName = ws->m_name.empty() ? std::to_string(wsId) : ws->m_name;
        s.floating = w->m_isFloating;
        s.fullscreen = w->isFullscreen();

        if (!w->m_realPosition || !w->m_realSize)
            continue;
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

    // Fingerprint the windows array (without `ts`) and skip the write if it
    // matches the last successful write. This stops the periodic snapshot
    // from rotating session.json -> .1 every 30s during idle.
    std::string winsJson = serialize(Value(winArr), 2);
    if (winsJson == m_lastWrittenJson) {
        return;
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
        if (ec) {
            Log::logger->log(Log::WARN, "[hypr-session] backup rename failed: {}", ec.message());
        } else if (::chmod(m_backupFile.c_str(), HSR_SECURE_FILE_MODE) != 0) {
            Log::logger->log(Log::WARN, "[hypr-session] chmod on backup failed: {}",
                             std::strerror(errno));
        }
    }
    if (!writeFileAtomic(m_sessionFile, serialize(Value(std::move(root)), 2))) {
        Log::logger->log(Log::ERR, "[hypr-session] failed to write {}", m_sessionFile.string());
        return;
    }
    m_lastWrittenJson = std::move(winsJson);
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
        if (ec)
            Log::logger->log(Log::WARN, "[hypr-session] stale lock remove failed: {}",
                             ec.message());
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

void CSessionManager::removeRestoreLock() const noexcept {
    std::error_code ec;
    fs::remove(m_restoreLock, ec);
    if (!ec)
        return;
    try {
        Log::logger->log(Log::WARN, "[hypr-session] lock remove failed: {}", ec.message());
    } catch (...) { // NOLINT(bugprone-empty-catch)
        // dtor path: logger formatting may throw bad_alloc; nothing we can do.
    }
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

    bool tripped = m_crashLoopLimit > 0 && history.size() >= static_cast<size_t>(m_crashLoopLimit);
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
    if (!g_pCompositor)
        return m;
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
        double gap = std::clamp(m_launchGapSecs, 0.0, 60.0);
        struct timespec ts{};
        ts.tv_sec = static_cast<time_t>(gap);
        ts.tv_nsec = static_cast<long>((gap - static_cast<double>(ts.tv_sec)) * 1e9);
        while (::nanosleep(&ts, &ts) == -1 && errno == EINTR) {
        }
    }
}
