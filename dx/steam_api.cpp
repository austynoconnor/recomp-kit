// steam_api.cpp - the Steam API as GOG's GalaxyWrp.dll exports it, offline.
//
// GOG builds of Steam-era games (Fallout: New Vegas first) keep their Steam
// calls and link them to GalaxyWrp.dll, which answers them through the GOG
// Galaxy client. There is no client here: SteamAPI_Init reports failure,
// IsSteamRunning says no, every interface getter returns null and the
// callback calls do nothing, which is the path a game takes when it was
// started without Steam (or Galaxy) and plays on without achievements or
// overlays.
#include "dx.h"
#include "../runtime/imports.h"

#include <iterator>

namespace {

void zero(X86 *c) {
    set_eax(c, 0);
}
void nothing(X86 *) {}

#define STEAM(dll, name, argc, fn) {dll, name, argc, fn}
const ImportShim shims[] = {
    STEAM("GalaxyWrp.dll", "SteamAPI_Init", ARGC_CDECL, zero),
    STEAM("GalaxyWrp.dll", "SteamAPI_IsSteamRunning", ARGC_CDECL, zero),
    STEAM("GalaxyWrp.dll", "SteamAPI_Shutdown", ARGC_CDECL, nothing),
    STEAM("GalaxyWrp.dll", "SteamAPI_RunCallbacks", ARGC_CDECL, nothing),
    STEAM("GalaxyWrp.dll", "SteamAPI_RegisterCallback", ARGC_CDECL, nothing),
    STEAM("GalaxyWrp.dll", "SteamAPI_UnregisterCallback", ARGC_CDECL, nothing),
    STEAM("GalaxyWrp.dll", "SteamUser", ARGC_CDECL, zero),
    STEAM("GalaxyWrp.dll", "SteamFriends", ARGC_CDECL, zero),
    STEAM("GalaxyWrp.dll", "SteamUserStats", ARGC_CDECL, zero),
    STEAM("GalaxyWrp.dll", "SteamApps", ARGC_CDECL, zero),
    STEAM("GalaxyWrp.dll", "SteamUtils", ARGC_CDECL, zero),
    // The same exports under Valve's own DLL name.
    STEAM("steam_api.dll", "SteamAPI_Init", ARGC_CDECL, zero),
    STEAM("steam_api.dll", "SteamAPI_IsSteamRunning", ARGC_CDECL, zero),
    STEAM("steam_api.dll", "SteamAPI_Shutdown", ARGC_CDECL, nothing),
    STEAM("steam_api.dll", "SteamAPI_RunCallbacks", ARGC_CDECL, nothing),
    STEAM("steam_api.dll", "SteamAPI_RegisterCallback", ARGC_CDECL, nothing),
    STEAM("steam_api.dll", "SteamAPI_UnregisterCallback", ARGC_CDECL, nothing),
    STEAM("steam_api.dll", "SteamUser", ARGC_CDECL, zero),
    STEAM("steam_api.dll", "SteamFriends", ARGC_CDECL, zero),
    STEAM("steam_api.dll", "SteamUserStats", ARGC_CDECL, zero),
    STEAM("steam_api.dll", "SteamApps", ARGC_CDECL, zero),
    STEAM("steam_api.dll", "SteamUtils", ARGC_CDECL, zero),
};
#undef STEAM

} // namespace

void steam_api_register() {
    imports_register(shims, std::size(shims));
}
