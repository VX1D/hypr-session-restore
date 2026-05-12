#pragma once

#include "Snapshot.hpp"

#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprutils/signal/Listener.hpp>

#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class CSessionManager {
  public:
    explicit CSessionManager(HANDLE handle);
    ~CSessionManager();
    CSessionManager(const CSessionManager&) = delete;
    CSessionManager& operator=(const CSessionManager&) = delete;
    CSessionManager(CSessionManager&&) = delete;
    CSessionManager& operator=(CSessionManager&&) = delete;

    void init();
    void shutdown();

    void snapshotNow();
    void restoreNow();

    static std::optional<std::string> buildDispatchExec(const SWindowSnapshot& window);

  private:
    void onWindowEvent();
    void onTick();

    std::vector<SWindowSnapshot> collectWindows() const;
    void writeSnapshot(const std::vector<SWindowSnapshot>& windows);
    std::optional<std::vector<SWindowSnapshot>> readSnapshot() const;
    static bool readProc(pid_t pid, std::vector<std::string>& argv, std::string& cwd);

    bool restoreLockExists() const;
    void touchRestoreLock() const;
    void removeRestoreLock() const;
    bool crashLoopTripped();
    std::unordered_map<std::string, int> currentClassCounts() const;
    std::vector<SWindowSnapshot>
    filterAlreadyRunning(std::vector<SWindowSnapshot> want,
                         const std::unordered_map<std::string, int>& running) const;
    void dispatchExec(const SWindowSnapshot& window) const;

    HANDLE m_handle = nullptr;

    std::filesystem::path m_cacheDir;
    std::filesystem::path m_sessionFile;
    std::filesystem::path m_backupFile;
    std::filesystem::path m_restoreLock;
    std::filesystem::path m_restoreLog;

    std::vector<Hyprutils::Signal::CHyprSignalListener> m_listeners;

    bool m_dirty = false;
    std::chrono::steady_clock::time_point m_lastEvent;
    std::chrono::steady_clock::time_point m_pluginStart;
    bool m_restoreAttempted = false;

    int m_debounceSecs = 3;
    int m_crashLoopWindowSecs = 180;
    int m_crashLoopLimit = 3;
    int m_restoreLockMaxAgeSecs = 90;
    int m_startupDelaySecs = 5;
    double m_launchGapSecs = 0.4;

    std::unordered_set<std::string> m_skipBasenames = {
        "waybar",
        "hyprpaper",
        "hypridle",
        "hyprlock",
        "mako",
        "dunst",
        "swaync",
        "wl-paste",
        "cliphist",
        "awww-daemon",
        "awww",
        "xdg-desktop-portal",
        "xdg-desktop-portal-hyprland",
        "xdg-desktop-portal-gtk",
        "polkit-gnome-authentication-agent-1",
        "Hyprland",
        "hyprland",
    };
};
