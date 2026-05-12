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

    g_pSession = std::make_unique<CSessionManager>(handle);
    g_pSession->init();

    HyprlandAPI::addNotification(PHANDLE, "[hypr-session-restore] loaded",
                                 CHyprColor{0.2f, 1.0f, 0.2f, 1.0f}, 4000);

    return {"hypr-session-restore",
            "Persist window layouts across crashes; relaunches apps on Hyprland startup.",
            "wirehead", "1.0.0"};
}

APICALL EXPORT void PLUGIN_EXIT() {
    g_pSession.reset();
}
