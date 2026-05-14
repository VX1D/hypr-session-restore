#include <hyprland/src/debug/log/Logger.hpp>
#include <hyprland/src/helpers/Color.hpp>
#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/version.h>

#include "SessionManager.hpp"

#include <memory>

inline HANDLE PHANDLE = nullptr;
inline std::unique_ptr<CSessionManager> g_pSession;

APICALL EXPORT std::string PLUGIN_API_VERSION() {
    return HYPRLAND_API_VERSION;
}

APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle) {
    PHANDLE = handle;

    const std::string SERVER_HASH = __hyprland_api_get_hash();
    const std::string CLIENT_HASH = __hyprland_api_get_client_hash();
    if (SERVER_HASH != CLIENT_HASH) {
        HyprlandAPI::addNotification(
            PHANDLE, "[hypr-session-restore] built against a different Hyprland version. Rebuild.",
            CHyprColor{1.0f, 0.2f, 0.2f, 1.0f}, 8000);
        throw std::runtime_error("hyprland version mismatch");
    }

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
    HyprlandAPI::addConfigValue(PHANDLE, "plugin:hypr-session:enabled", Hyprlang::INT{1});
    HyprlandAPI::addConfigValue(PHANDLE, "plugin:hypr-session:debounce_secs", Hyprlang::INT{3});
    HyprlandAPI::addConfigValue(PHANDLE, "plugin:hypr-session:periodic_secs", Hyprlang::INT{30});
    HyprlandAPI::addConfigValue(PHANDLE, "plugin:hypr-session:startup_delay_secs",
                                Hyprlang::INT{5});
    HyprlandAPI::addConfigValue(PHANDLE, "plugin:hypr-session:crash_loop_window_secs",
                                Hyprlang::INT{180});
    HyprlandAPI::addConfigValue(PHANDLE, "plugin:hypr-session:crash_loop_limit", Hyprlang::INT{3});
    HyprlandAPI::addConfigValue(PHANDLE, "plugin:hypr-session:launch_gap_ms", Hyprlang::INT{400});
    HyprlandAPI::addConfigValue(PHANDLE, "plugin:hypr-session:extra_skip", Hyprlang::STRING{""});
    HyprlandAPI::addConfigValue(PHANDLE, "plugin:hypr-session:extra_sensitive",
                                Hyprlang::STRING{""});
#pragma GCC diagnostic pop

    g_pSession = std::make_unique<CSessionManager>(handle);
    try {
        g_pSession->init();
    } catch (const std::exception& e) {
        g_pSession.reset();
        HyprlandAPI::addNotification(PHANDLE,
                                     std::string("[hypr-session-restore] init failed: ") + e.what(),
                                     CHyprColor{1.0f, 0.4f, 0.2f, 1.0f}, 8000);
        return {"hypr-session-restore", "init failed — plugin disabled", "wirehead", "1.0.0"};
    } catch (...) {
        g_pSession.reset();
        HyprlandAPI::addNotification(PHANDLE, "[hypr-session-restore] init failed (unknown)",
                                     CHyprColor{1.0f, 0.4f, 0.2f, 1.0f}, 8000);
        return {"hypr-session-restore", "init failed — plugin disabled", "wirehead", "1.0.0"};
    }

    HyprlandAPI::addNotification(PHANDLE, "[hypr-session-restore] loaded",
                                 CHyprColor{0.2f, 1.0f, 0.2f, 1.0f}, 4000);

    return {"hypr-session-restore",
            "Persist window layouts across crashes; relaunches apps on Hyprland startup.",
            "wirehead", "1.0.0"};
}

APICALL EXPORT void PLUGIN_EXIT() {
    g_pSession.reset();
}
