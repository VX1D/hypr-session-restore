// Edge case suite for the public header API: validateSnapshot, shellQuote,
// hsrjson::parse, readSecureFile/writeFileAtomic, buildDispatchExecCommand.

#include "src/Json.hpp"
#include "src/SecureFile.hpp"
#include "src/Snapshot.hpp"

#include <climits>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace {

namespace fs = std::filesystem;

int g_passes = 0;
int g_fails = 0;
const char* g_section = "?";

void require(bool cond, const char* msg) {
    if (cond) {
        ++g_passes;
    } else {
        ++g_fails;
        std::cerr << "FAIL [" << g_section << "]: " << msg << '\n';
    }
}

// SECTION C: validateSnapshot boundaries.

void sectionC_ValidateSnapshot() {
    g_section = "C/validate";

    SWindowSnapshot ok;
    ok.cmdline = {"/usr/bin/app"};
    require(validateSnapshot(ok), "minimal valid");

    SWindowSnapshot empty;
    require(!validateSnapshot(empty), "empty cmdline rejected");

    SWindowSnapshot withEmptyArg;
    withEmptyArg.cmdline = {"/usr/bin/app", ""};
    require(!validateSnapshot(withEmptyArg), "empty arg rejected");

    SWindowSnapshot tooManyArgs;
    for (size_t i = 0; i < HSR_MAX_CMDLINE_ARGS + 1; ++i)
        tooManyArgs.cmdline.push_back("x");
    require(!validateSnapshot(tooManyArgs), "> 128 args rejected");

    SWindowSnapshot exactlyMaxArgs;
    for (size_t i = 0; i < HSR_MAX_CMDLINE_ARGS; ++i)
        exactlyMaxArgs.cmdline.push_back("x");
    require(validateSnapshot(exactlyMaxArgs), "exactly 128 args ok");

    SWindowSnapshot withNul;
    withNul.cmdline = {std::string("a\0b", 3)};
    require(!validateSnapshot(withNul), "embedded NUL rejected");

    SWindowSnapshot hugeArg;
    hugeArg.cmdline = {std::string(HSR_MAX_ARG_BYTES + 1, 'x')};
    require(!validateSnapshot(hugeArg), "oversized arg rejected");

    SWindowSnapshot hugeArgExact;
    hugeArgExact.cmdline = {std::string(HSR_MAX_ARG_BYTES, 'x')};
    require(validateSnapshot(hugeArgExact), "exactly max arg bytes ok");

    SWindowSnapshot badWs;
    badWs.cmdline = {"app"};
    badWs.workspaceId = 99999;
    validateSnapshot(badWs);
    require(badWs.workspaceId == 1, "out-of-range workspace clamped to 1");

    SWindowSnapshot negWs;
    negWs.cmdline = {"app"};
    negWs.workspaceId = -5;
    validateSnapshot(negWs);
    require(negWs.workspaceId == 1, "negative workspace clamped to 1");

    SWindowSnapshot nanGeom;
    nanGeom.cmdline = {"app"};
    nanGeom.atX = std::numeric_limits<double>::quiet_NaN();
    nanGeom.atY = 0;
    validateSnapshot(nanGeom);
    require(nanGeom.atX == 0.0 && nanGeom.atY == 0.0, "NaN position zeroed");

    SWindowSnapshot infGeom;
    infGeom.cmdline = {"app"};
    infGeom.sizeW = std::numeric_limits<double>::infinity();
    infGeom.sizeH = 100;
    validateSnapshot(infGeom);
    require(infGeom.sizeW == 0.0 && infGeom.sizeH == 0.0, "Inf size zeroed");

    SWindowSnapshot negSize;
    negSize.cmdline = {"app"};
    negSize.sizeW = -1;
    negSize.sizeH = 100;
    validateSnapshot(negSize);
    require(negSize.sizeW == 0.0 && negSize.sizeH == 0.0, "negative size zeroed");

    SWindowSnapshot extremePos;
    extremePos.cmdline = {"app"};
    extremePos.atX = 1e10;
    extremePos.atY = 0;
    validateSnapshot(extremePos);
    require(extremePos.atX == 0.0, "off-screen position clamped");
}

// SECTION D: shellQuote corners.

void sectionD_ShellQuote() {
    g_section = "D/shellquote";

    require(shellQuote("") == "''", "empty → ''");
    require(shellQuote("abc") == "abc", "alnum unquoted");
    require(shellQuote("/usr/bin/foo") == "/usr/bin/foo", "path unquoted");
    require(shellQuote("a-b_c.d,e:f@g+h=i") == "a-b_c.d,e:f@g+h=i", "safe chars unquoted");
    require(shellQuote(" ") == "' '", "space quoted");
    require(shellQuote("$") == "'$'", "dollar quoted");
    require(shellQuote("`") == "'`'", "backtick quoted");
    require(shellQuote("foo;rm -rf /") == "'foo;rm -rf /'", "semicolon quoted");
    require(shellQuote("'") == "''\\'''", "single quote escaped");
    require(shellQuote("a'b'c") == "'a'\\''b'\\''c'", "multiple quotes");
    require(shellQuote("\n") == "'\n'", "newline quoted (literal)");
    require(shellQuote("\\") == "'\\'", "backslash quoted");
    require(shellQuote("$(rm)") == "'$(rm)'", "command sub quoted");

    // High-bit / UTF-8 — must be quoted (not alnum in C locale).
    require(shellQuote("ó").front() == '\'', "non-ASCII quoted");
}

// SECTION E: JSON parser corner cases not in test_json.cpp.

void sectionE_JsonCorners() {
    g_section = "E/json";

    require(!hsrjson::parse("").has_value(), "empty rejected");
    require(!hsrjson::parse("   ").has_value(), "whitespace only rejected");
    require(hsrjson::parse("null").has_value(), "bare null ok");
    require(hsrjson::parse("true").has_value(), "bare true ok");
    require(hsrjson::parse("false").has_value(), "bare false ok");
    require(hsrjson::parse("42").has_value(), "bare number ok");
    require(hsrjson::parse("\"hi\"").has_value(), "bare string ok");
    require(hsrjson::parse("[]").has_value(), "empty array");
    require(hsrjson::parse("{}").has_value(), "empty object");

    require(!hsrjson::parse("{").has_value(), "unterminated object");
    require(!hsrjson::parse("[").has_value(), "unterminated array");
    require(!hsrjson::parse("\"unterminated").has_value(), "unterminated string");
    require(!hsrjson::parse("{\"x\":}").has_value(), "missing value");
    require(!hsrjson::parse("{\"x\":1,}").has_value(), "trailing comma in obj");
    require(!hsrjson::parse("[1,]").has_value(), "trailing comma in arr");
    require(!hsrjson::parse("{x:1}").has_value(), "unquoted key");
    require(!hsrjson::parse("'single'").has_value(), "single-quoted string");
    require(hsrjson::parse("0").has_value(), "zero");
    require(hsrjson::parse("-0").has_value(), "negative zero");
    require(hsrjson::parse("1e308").has_value(), "near double max");
    require(hsrjson::parse("1.7976931348623157e+308").has_value(), "exact double max");
    require(!hsrjson::parse("NaN").has_value(), "NaN rejected");
    require(!hsrjson::parse("Infinity").has_value(), "Infinity rejected");
    auto p = hsrjson::parse("{\"x\":\"\\u0000\"}");
    if (p.has_value()) {
        const auto* v = p->find("x");
        require(v && v->isString(), "unicode NUL parses");
        // Confirm the NUL is preserved; downstream validation must reject.
        require(v->str().size() == 1 && v->str()[0] == '\0', "NUL byte preserved");
    }
    auto sp = hsrjson::parse("{\"x\":\"\\uD834\\uDD1E\"}");
    require(sp.has_value(), "surrogate pair parses");
    (void)hsrjson::parse("{\"x\":\"\\uD834\"}");
    std::string deep;
    for (int i = 0; i < 30; ++i)
        deep += '[';
    deep += "1";
    for (int i = 0; i < 30; ++i)
        deep += ']';
    require(hsrjson::parse(deep).has_value(), "30-deep array ok");
}

// SECTION G: SecureFile edge cases — extends test_json.cpp coverage.

fs::path makeTempDir() {
    auto base = fs::temp_directory_path() / "hsr-edge-XXXXXX";
    std::string pattern = base.string();
    char* created = ::mkdtemp(pattern.data());
    require(created != nullptr, "mkdtemp");
    return fs::path(created);
}

void writePlain(const fs::path& path, const std::string& content, mode_t mode = 0600) {
    std::ofstream f(path, std::ios::binary);
    f << content;
    f.close();
    ::chmod(path.c_str(), mode);
}

void sectionG_SecureFile() {
    g_section = "G/securefile";

    // Missing file
    require(!readSecureFile("/nonexistent/path/xyz").has_value(), "missing file");

    auto dir = makeTempDir();

    // Empty file
    auto empty = dir / "empty";
    writePlain(empty, "", HSR_SECURE_FILE_MODE);
    auto r = readSecureFile(empty);
    require(r.has_value() && r->empty(), "empty file ok");

    // Exactly at size limit
    auto atLimit = dir / "at_limit";
    writePlain(atLimit, std::string(HSR_MAX_SNAPSHOT_BYTES, 'x'), HSR_SECURE_FILE_MODE);
    r = readSecureFile(atLimit);
    require(r.has_value() && r->size() == HSR_MAX_SNAPSHOT_BYTES, "exact-limit file ok");
    auto worldR = dir / "world_r";
    writePlain(worldR, "{}", 0604);
    require(!readSecureFile(worldR).has_value(), "other-readable rejected");
    auto ownerOnly = dir / "owner_only";
    writePlain(ownerOnly, "{}", 0400);
    require(readSecureFile(ownerOnly).has_value(), "0400 accepted");
    auto subdir = dir / "subdir";
    fs::create_directory(subdir);
    ::chmod(subdir.c_str(), 0700);
    require(!readSecureFile(subdir).has_value(), "directory rejected (not regular)");
    auto round = dir / "round";
    require(writeFileAtomic(round, "hello world"), "atomic write");
    auto back = readSecureFile(round);
    require(back.has_value() && *back == "hello world", "atomic round-trip");
    require(!writeFileAtomic(round, std::string(HSR_MAX_SNAPSHOT_BYTES + 1, 'x')),
            "atomic write refuses oversized");
    struct stat st{};
    ::stat(round.c_str(), &st);
    require((st.st_mode & 0777) == HSR_SECURE_FILE_MODE, "atomic write 0600 mode");
    require(ensureSecureCacheDir(dir), "secure dir ok");
    auto realDir = dir / "realsub";
    fs::create_directory(realDir);
    ::chmod(realDir.c_str(), 0700);
    auto linkDir = dir / "linksub";
    ::symlink(realDir.c_str(), linkDir.c_str());
    require(!ensureSecureCacheDir(linkDir), "symlink dir rejected");

    fs::remove_all(dir);
}

// SECTION H: buildDispatchExecCommand corners.

void sectionH_DispatchExec() {
    g_section = "H/dispatch";

    SWindowSnapshot w;
    w.cmdline = {"app"};
    w.workspaceId = 5;
    auto c = buildDispatchExecCommand(w);
    require(c && c->find("workspace 5 silent") != std::string::npos, "basic");
    SWindowSnapshot w2;
    w2.cmdline = {"app"};
    w2.cwd = "/nonexistent/path/xyz";
    auto c2 = buildDispatchExecCommand(w2);
    require(c2 && c2->find("cd ") == std::string::npos, "missing cwd dropped");
    SWindowSnapshot w3;
    w3.cmdline = {"app"};
    w3.cwd = "/tmp";
    auto c3 = buildDispatchExecCommand(w3);
    require(c3 && c3->find("cd /tmp && exec") != std::string::npos, "valid cwd included");
    SWindowSnapshot w4;
    w4.cmdline = {"app"};
    w4.floating = true;
    w4.sizeW = 800;
    w4.sizeH = 600;
    w4.atX = 100;
    w4.atY = 50;
    auto c4 = buildDispatchExecCommand(w4);
    require(c4 && c4->find(";float") != std::string::npos, "float flag");
    require(c4 && c4->find(";size 800 600") != std::string::npos, "size flag");
    require(c4 && c4->find(";move 100 50") != std::string::npos, "move flag");
    SWindowSnapshot w5;
    w5.cmdline = {"app"};
    w5.floating = true;
    w5.fullscreen = true;
    auto c5 = buildDispatchExecCommand(w5);
    require(c5 && c5->find(";fullscreen") != std::string::npos, "fullscreen flag");
    SWindowSnapshot w6;
    w6.cmdline = {"app", "$(rm -rf /)", "`whoami`", ";echo pwned"};
    auto c6 = buildDispatchExecCommand(w6);
    require(c6 && c6->find("rm -rf /") != std::string::npos, "metachars survive in literal");
    require(c6 && c6->find("'$(rm -rf /)'") != std::string::npos, "command-sub quoted");
    require(c6 && c6->find("'`whoami`'") != std::string::npos, "backtick quoted");
    SWindowSnapshot w7;
    w7.cmdline = {"app", ""};
    require(!buildDispatchExecCommand(w7), "empty arg rejected");
    SWindowSnapshot w8;
    w8.cmdline = {"app"};
    w8.workspaceId = 99999;
    auto c8 = buildDispatchExecCommand(w8);
    require(c8 && c8->find("workspace 1 silent") != std::string::npos, "huge ws clamped");
}

// SECTION J: integer cast / narrowing patterns used elsewhere in the plugin.

void sectionJ_CastingMath() {
    g_section = "J/casts";
    double bigD = 4.2e9; // > INT_MAX
    int castDown = static_cast<int>(bigD);
    (void)castDown; // result is implementation-defined for overflow
    SWindowSnapshot s;
    s.cmdline = {"app"};
    s.workspaceId = INT_MAX;
    validateSnapshot(s);
    require(s.workspaceId == 1, "INT_MAX workspace clamped");
    s.workspaceId = INT_MIN;
    validateSnapshot(s);
    require(s.workspaceId == 1, "INT_MIN workspace clamped");
    s.atX = 1e308;
    validateSnapshot(s);
    require(s.atX == 0.0, "huge geometry zeroed");
}

} // namespace

int main() {
    sectionC_ValidateSnapshot();
    sectionD_ShellQuote();
    sectionE_JsonCorners();
    sectionG_SecureFile();
    sectionH_DispatchExec();
    sectionJ_CastingMath();

    std::cerr << "passed: " << g_passes << "  failed: " << g_fails << '\n';
    return g_fails ? 1 : 0;
}
