// Dry-run the restore pipeline on a real session.json and print the
// hyprctl dispatch exec commands that would be issued.

#include "src/Json.hpp"
#include "src/SecureFile.hpp"
#include "src/Snapshot.hpp"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>

namespace fs = std::filesystem;

int main(int argc, char** argv) {
    fs::path path;
    if (argc > 1) {
        path = argv[1];
    } else if (const char* home = std::getenv("HOME")) {
        path = fs::path(home) / ".cache" / "hypr" / "session.json";
    } else {
        std::cerr << "usage: " << argv[0] << " <session.json>\n";
        return 2;
    }

    auto raw = readSecureFile(path);
    if (!raw) {
        std::cerr << "[FAIL] readSecureFile(" << path << ") returned null "
                  << "(missing, oversized, world-readable, or symlinked)\n";
        return 1;
    }
    std::cout << "[OK]   readSecureFile: " << raw->size() << " bytes\n";

    auto parsed = hsrjson::parse(*raw);
    if (!parsed || !parsed->isObject()) {
        std::cerr << "[FAIL] hsrjson::parse — malformed JSON\n";
        return 1;
    }
    std::cout << "[OK]   hsrjson::parse: root is object\n";

    if (const auto* schema = parsed->find("schema_version")) {
        int v = static_cast<int>(schema->numOr(-1));
        if (v != HSR_SNAPSHOT_SCHEMA_VERSION) {
            std::cerr << "[FAIL] schema_version=" << v << " (expected "
                      << HSR_SNAPSHOT_SCHEMA_VERSION << ")\n";
            return 1;
        }
        std::cout << "[OK]   schema_version=" << v << '\n';
    }

    const auto* wins = parsed->find("windows");
    if (!wins || !wins->isArray()) {
        std::cerr << "[FAIL] windows is not array\n";
        return 1;
    }
    std::cout << "[OK]   windows array size=" << wins->arr().size() << '\n';
    if (wins->arr().empty()) {
        std::cerr << "[WARN] empty windows array (nothing to restore)\n";
        return 0;
    }
    if (wins->arr().size() > HSR_MAX_WINDOWS) {
        std::cerr << "[FAIL] too many windows (" << wins->arr().size() << " > " << HSR_MAX_WINDOWS
                  << ")\n";
        return 1;
    }

    int validCount = 0;
    int rejectedCount = 0;
    int dispatchOkCount = 0;

    for (size_t i = 0; i < wins->arr().size(); ++i) {
        const auto& wv = wins->arr()[i];
        if (!wv.isObject()) {
            std::cerr << "[skip #" << i << "] not object\n";
            ++rejectedCount;
            continue;
        }
        SWindowSnapshot s;
        if (auto* v = wv.find("cmdline"); v && v->isArray())
            for (const auto& a : v->arr())
                if (a.isString())
                    s.cmdline.push_back(a.str());
        if (s.cmdline.empty()) {
            std::cerr << "[skip #" << i << "] empty cmdline\n";
            ++rejectedCount;
            continue;
        }
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

        if (!validateSnapshot(s)) {
            std::cerr << "[skip #" << i << "] validateSnapshot rejected\n";
            ++rejectedCount;
            continue;
        }
        ++validCount;

        auto cmd = buildDispatchExecCommand(s);
        if (!cmd) {
            std::cerr << "[skip #" << i << "] buildDispatchExecCommand rejected\n";
            ++rejectedCount;
            continue;
        }
        ++dispatchOkCount;
        std::cout << "  [#" << i << "] ws=" << s.workspaceId
                  << " class=" << (s.klass.empty() ? "?" : s.klass) << "  → hyprctl dispatch exec "
                  << *cmd << '\n';
    }

    std::cout << "\n=== summary ===\n";
    std::cout << "windows in snapshot     : " << wins->arr().size() << '\n';
    std::cout << "passed validateSnapshot : " << validCount << '\n';
    std::cout << "dispatch cmd built      : " << dispatchOkCount << '\n';
    std::cout << "rejected                : " << rejectedCount << '\n';

    return (dispatchOkCount > 0) ? 0 : 1;
}
