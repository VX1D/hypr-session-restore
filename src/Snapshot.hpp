#pragma once

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <linux/limits.h>
#include <optional>
#include <string>
#include <vector>

inline constexpr int HSR_SNAPSHOT_SCHEMA_VERSION = 1;
inline constexpr size_t HSR_MAX_SNAPSHOT_BYTES = 1024UL * 1024UL;
inline constexpr size_t HSR_MAX_WINDOWS = 256;
inline constexpr size_t HSR_MAX_CMDLINE_ARGS = 128;
inline constexpr size_t HSR_MAX_ARG_BYTES = 4096;
inline constexpr size_t HSR_MAX_FIELD_BYTES = 4096;
inline constexpr int HSR_MIN_WORKSPACE_ID = 0;
inline constexpr int HSR_MAX_WORKSPACE_ID = 1000;
inline constexpr double HSR_MIN_GEOMETRY = -100000.0;
inline constexpr double HSR_MAX_GEOMETRY = 100000.0;

struct SWindowSnapshot {
    std::vector<std::string> cmdline;
    std::string cwd;
    std::string klass;
    std::string title;
    int workspaceId = 1;
    std::string workspaceName;
    bool floating = false;
    bool fullscreen = false;
    double atX = 0.0;
    double atY = 0.0;
    double sizeW = 0.0;
    double sizeH = 0.0;
};

inline bool validSnapshotString(const std::string& value, size_t maxBytes) {
    return value.size() <= maxBytes &&
           std::none_of(value.begin(), value.end(),
                        [](unsigned char character) { return character == '\0'; });
}

inline bool validSnapshotNumber(double value, double minValue, double maxValue) {
    return std::isfinite(value) && value >= minValue && value <= maxValue;
}

inline bool validateSnapshot(SWindowSnapshot& window) {
    if (window.cmdline.empty() || window.cmdline.size() > HSR_MAX_CMDLINE_ARGS) {
        return false;
    }
    for (const auto& arg : window.cmdline) {
        if (arg.empty() || !validSnapshotString(arg, HSR_MAX_ARG_BYTES)) {
            return false;
        }
    }
    if (!validSnapshotString(window.cwd, PATH_MAX) ||
        !validSnapshotString(window.klass, HSR_MAX_FIELD_BYTES) ||
        !validSnapshotString(window.title, HSR_MAX_FIELD_BYTES) ||
        !validSnapshotString(window.workspaceName, HSR_MAX_FIELD_BYTES)) {
        return false;
    }
    if (window.workspaceId < HSR_MIN_WORKSPACE_ID || window.workspaceId > HSR_MAX_WORKSPACE_ID) {
        window.workspaceId = 1;
    }
    if (!validSnapshotNumber(window.atX, HSR_MIN_GEOMETRY, HSR_MAX_GEOMETRY) ||
        !validSnapshotNumber(window.atY, HSR_MIN_GEOMETRY, HSR_MAX_GEOMETRY)) {
        window.atX = 0.0;
        window.atY = 0.0;
    }
    if (!validSnapshotNumber(window.sizeW, 0.0, HSR_MAX_GEOMETRY) ||
        !validSnapshotNumber(window.sizeH, 0.0, HSR_MAX_GEOMETRY)) {
        window.sizeW = 0.0;
        window.sizeH = 0.0;
    }
    return true;
}

inline std::string shellQuote(const std::string& value) {
    if (value.empty()) {
        return "''";
    }
    if (std::all_of(value.begin(), value.end(), [](char character) {
            return std::isalnum(static_cast<unsigned char>(character)) || character == '_' ||
                   character == '/' || character == '-' || character == '.' || character == ',' ||
                   character == ':' || character == '@' || character == '+' || character == '=';
        })) {
        return value;
    }
    std::string out = "'";
    for (char character : value) {
        if (character == '\'') {
            out += "'\\''";
        } else {
            out += character;
        }
    }
    out += '\'';
    return out;
}

inline std::optional<std::string> buildDispatchExecCommand(const SWindowSnapshot& window) {
    SWindowSnapshot checked = window;
    if (!validateSnapshot(checked)) {
        return std::nullopt;
    }

    std::string flags = "workspace " + std::to_string(checked.workspaceId) + " silent";
    if (checked.floating) {
        flags += ";float";
        if (checked.sizeW > 0 && checked.sizeH > 0) {
            flags += ";size " + std::to_string(static_cast<int>(checked.sizeW)) + ' ' +
                     std::to_string(static_cast<int>(checked.sizeH));
        }
        if (checked.atX != 0 || checked.atY != 0) {
            flags += ";move " + std::to_string(static_cast<int>(checked.atX)) + ' ' +
                     std::to_string(static_cast<int>(checked.atY));
        }
    }
    if (checked.fullscreen) {
        flags += ";fullscreen";
    }

    std::string cmd;
    for (size_t index = 0; index < checked.cmdline.size(); ++index) {
        if (index != 0) {
            cmd += ' ';
        }
        cmd += shellQuote(checked.cmdline[index]);
    }
    std::error_code ec;
    if (!checked.cwd.empty() && std::filesystem::is_directory(checked.cwd, ec)) {
        cmd = "cd " + shellQuote(checked.cwd) + " && exec " + cmd;
    }

    return "[" + flags + "] sh -c " + shellQuote(cmd);
}
