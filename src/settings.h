#pragma once

// Windows: Spotify by default (the Windows media controls list every app). Linux/Proton build: any player, i.e.
// whatever the system plays (Local Radio in the browser, Spotify, ...), see media_mpris.cpp.
#ifdef _MSC_VER
#define CNP_DEFAULT_APP_FILTER "spotify"
#define CNP_DEFAULT_APP_FILTER_W L"spotify"
#else
#define CNP_DEFAULT_APP_FILTER ""
#define CNP_DEFAULT_APP_FILTER_W L""
#endif
#include "common.h"

namespace ep {

// User settings from <plugin folder>\CoachNowPlaying.ini. Missing keys are added with their defaults.
struct Settings {
    bool flipVertical = false;      // [screen] flip_vertical: the game samples the screen texture upside down
    std::string appFilter = CNP_DEFAULT_APP_FILTER;  // [media] app_filter: only show media from apps containing this text ("" = any)
    bool coverLookup = true;        // [radio] cover_lookup: look up song covers for radio titles on iTunes
    int radioStaleMs = 2500;        // [radio] pause_after_ms: no stream data for this long = radio paused
    int radioHoldMs = 600;          // [radio] switch_hold_ms: keep the radio screen this long after a stream ends
    bool touch = true;              // [touch] enabled: click the buttons on the screen with the mouse
    int autoFit = 1;                // [screen] auto_fit: 0 off, 1 fit the picture to each truck's display, 2 measure only
    bool extraSources = true;       // [radio] extra_sources: use the optional CoachNowPlaying.sources.ini
    bool announce = true;           // [announce] enabled: chime + voice a few minutes before arrival (coaches only)
    int announceMinutes = 5;        // [announce] minutes: real minutes before arrival
    int announceVolume = 80;        // [announce] volume: 0..100
    std::wstring announceLanguage = L"auto";  // [announce] language: auto (game language) or codes, e.g. "auto,en"
    bool textureFinder = false;     // [debug] texture_finder: developer tool (Ctrl+Shift+F6..F9), not in the default ini
    // [screen] gps_takeover: 1 = without the mod (e.g. TruckersMP), show the CarPlay picture on the game's own cab
    // screen (its 1024x1024 render target). Ctrl+Shift+F10 cycles: candidate 0, 1, ..., off (stock GPS).
    // auto = like 1, but it switches itself off as soon as the mod's own CarPlay screen loads (single player with the mod).
    // gpsTakeover: prepared at start (1 or auto); whether it is still in use: TakeoverActive() (d3d11_hooks.h).
    bool gpsTakeover = false;
    bool gpsTakeoverAuto = false;
    int gpsTakeoverPick = 0;        // [screen] gps_takeover_pick: which matching texture to use first (-1 = start off)
    // [screen] gps_takeover_hide: displays (meshes "indices:start:base", comma separated) that share the cab screen
    // texture but keep the game's own picture, e.g. the middle of a Volvo 9700's instrument cluster. Ctrl+Shift+F11.
    std::wstring gpsTakeoverHide;
    std::wstring ini;               // full path of CoachNowPlaying.ini
};

void LoadSettings(const std::wstring& dir);
const Settings& GetSettings();

}  // namespace ep
