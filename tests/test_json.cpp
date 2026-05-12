#include "src/Json.hpp"
#include "src/SecureFile.hpp"
#include "src/Snapshot.hpp"

#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <unistd.h>

namespace {

namespace fs = std::filesystem;

void require(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

void testRoundTripSnapshotShape() {
    hsrjson::Array command;
    command.emplace_back("/usr/bin/alacritty");
    command.emplace_back("--title");
    command.emplace_back("dev shell");

    hsrjson::Object window;
    window["cmdline"] = std::move(command);
    window["cwd"] = "/home/wirehead";
    window["workspace_id"] = 2;
    window["floating"] = false;

    hsrjson::Array windows;
    windows.emplace_back(std::move(window));

    hsrjson::Object root;
    root["schema_version"] = 1;
    root["windows"] = std::move(windows);

    auto serialized = hsrjson::serialize(hsrjson::Value(std::move(root)));
    auto parsed = hsrjson::parse(serialized);

    require(parsed.has_value(), "serialized snapshot parses");
    require(parsed->isObject(), "root is object");
    require(parsed->find("windows") != nullptr, "windows field exists");
    require(parsed->find("windows")->arr().size() == 1, "one window restored");
}

void testRejectsTrailingGarbage() {
    auto parsed = hsrjson::parse("{\"windows\":[]} trailing");
    require(!parsed.has_value(), "parser rejects trailing garbage");
}

void testRejectsBadEscapes() {
    auto parsed = hsrjson::parse("{\"x\":\"\\q\"}");
    require(!parsed.has_value(), "parser rejects invalid string escape");
}

void testRejectsTooDeepJson() {
    std::string nested;
    for (int i = 0; i < HSR_MAX_WORKSPACE_ID / 10; ++i) {
        nested += '[';
    }
    nested += "null";
    for (int i = 0; i < HSR_MAX_WORKSPACE_ID / 10; ++i) {
        nested += ']';
    }

    auto parsed = hsrjson::parse(nested);
    require(!parsed.has_value(), "parser rejects excessive nesting");
}

void testUnicodeEscape() {
    auto parsed = hsrjson::parse("{\"x\":\"\\u0041\"}");
    require(parsed.has_value(), "unicode escape parses");
    const auto* value = parsed->find("x");
    require(value != nullptr && value->strOr("") == "A", "unicode escape decodes BMP ascii");
}

void testBuildDispatchQuotesShellMetacharacters() {
    SWindowSnapshot window;
    window.cmdline = {"/usr/bin/app", "semi;colon", "it's fine", "two words"};
    window.workspaceId = 3;
    auto command = buildDispatchExecCommand(window);

    require(command.has_value(), "command builder accepts valid snapshot");
    const auto& commandText = *command;
    require(contains(commandText, "workspace 3 silent"), "workspace flag present");
    require(contains(commandText, "sh -c"), "shell wrapper present");
    require(shellQuote("semi;colon") == "'semi;colon'", "semicolon argument quoted");
    require(shellQuote("it's fine") == "'it'\\''s fine'", "apostrophe escaped");
    require(shellQuote("two words") == "'two words'", "space argument quoted");
}

void testBuildDispatchRejectsEmptyArgument() {
    SWindowSnapshot window;
    window.cmdline = {"/usr/bin/app", ""};

    require(!buildDispatchExecCommand(window).has_value(), "empty argument rejected");
}

void testBuildDispatchClampsRanges() {
    SWindowSnapshot window;
    window.cmdline = {"/usr/bin/app"};
    window.workspaceId = 50000;
    window.floating = true;
    window.atX = 1e9;
    window.atY = -1e9;
    window.sizeW = -1.0;
    window.sizeH = 10.0;

    auto command = buildDispatchExecCommand(window);
    require(command.has_value(), "out-of-range geometry is clamped");
    const auto& commandText = *command;
    require(contains(commandText, "workspace 1 silent"), "workspace id clamped");
    require(!contains(commandText, ";move"), "invalid position omitted");
    require(!contains(commandText, ";size"), "invalid size omitted");
}

fs::path makeTempDir() {
    auto base = fs::temp_directory_path() / "hsr-tests-XXXXXX";
    std::string pattern = base.string();
    char* created = ::mkdtemp(pattern.data());
    require(created != nullptr, "mkdtemp creates temp dir");
    return fs::path(created);
}

void writePlainFile(const fs::path& path, const std::string& content) {
    std::ofstream file(path, std::ios::binary);
    require(static_cast<bool>(file), "test file opens");
    file << content;
    file.close();
    require(static_cast<bool>(file), "test file writes");
}

void testSecureFileRejectsSymlink() {
    auto dir = makeTempDir();
    auto real = dir / "real";
    auto link = dir / "link";
    writePlainFile(real, "{}");
    require(::chmod(real.c_str(), HSR_SECURE_FILE_MODE) == 0, "chmod real");
    require(::symlink(real.c_str(), link.c_str()) == 0, "symlink created");

    require(!readSecureFile(link).has_value(), "symlink rejected");
    fs::remove_all(dir);
}

void testSecureFileRejectsGroupWritable() {
    auto dir = makeTempDir();
    auto file = dir / "session.json";
    writePlainFile(file, "{}");
    require(::chmod(file.c_str(), 0660) == 0, "chmod group writable");

    require(!readSecureFile(file).has_value(), "group writable file rejected");
    fs::remove_all(dir);
}

void testSecureFileRejectsOversized() {
    auto dir = makeTempDir();
    auto file = dir / "session.json";
    writePlainFile(file, std::string(HSR_MAX_SNAPSHOT_BYTES + 1, 'x'));
    require(::chmod(file.c_str(), HSR_SECURE_FILE_MODE) == 0, "chmod oversized");

    require(!readSecureFile(file).has_value(), "oversized file rejected");
    fs::remove_all(dir);
}

int runTests() {
    testRoundTripSnapshotShape();
    testRejectsTrailingGarbage();
    testRejectsBadEscapes();
    testRejectsTooDeepJson();
    testUnicodeEscape();
    testBuildDispatchQuotesShellMetacharacters();
    testBuildDispatchRejectsEmptyArgument();
    testBuildDispatchClampsRanges();
    testSecureFileRejectsSymlink();
    testSecureFileRejectsGroupWritable();
    testSecureFileRejectsOversized();
    return 0;
}

}

int main() {
    return runTests();
}
