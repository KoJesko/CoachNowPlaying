#include "common.h"

#include "cover_lookup.h"
#include "d3d11_hooks.h"
#include "engine_hooks.h"
#include "extra_sources.h"
#include "log.h"
#include "media.h"
#include "net_hooks.h"
#include "radio_tracker.h"
#include "renderer.h"
#include "station_logo.h"
#include "safe_mode.h"
#include "screen_assets.h"
#include "screen_fit.h"
#include "settings.h"
#include "text_util.h"
#include "version.h"

// The SDK header declares the entry points without extern "C"; the game looks them up by plain name.
extern "C" {
#include <scssdk_telemetry.h>
#include <common/scssdk_telemetry_common_channels.h>
#include <common/scssdk_telemetry_common_configs.h>
#include <common/scssdk_telemetry_truck_common_channels.h>
}
#include "eta_screen.h"
#include "passenger.h"

#include <MinHook.h>
#include <thread>
#include <shlobj.h>

namespace {

scs_log_t g_scsLog = nullptr;

void GameLog(scs_log_type_t type, const char* msg) {
    if (g_scsLog) g_scsLog(type, msg);
}

std::wstring KnownFolder(REFKNOWNFOLDERID id) {
    PWSTR p = nullptr;
    std::wstring out;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &p)) && p) out = p;
    CoTaskMemFree(p);
    return out;
}

// ---- game updates ----------------------------------------------------------------------------------------------------
// A game update can move the places the plugin hooks into. Two safety nets keep the player from being left with a dead
// GPS screen:
//  * if a hook cannot be found at all, the game's own GPS stays untouched and the game console says so;
//  * if the hooks exist but the GPS texture is never redirected within a minute after the truck was loaded, the game's GPS
//    is switched back on, the game console says so, and the game version is remembered (together with the plugin version).
//    A remembered version starts in "observe only" mode: nothing is changed in the game; as soon as a lookup of the GPS
//    texture shows that redirecting would work, the mark is removed and the player is asked to restart the game.
const char kGithub[] = "https://github.com/jxhnch/CoachNowPlaying";

std::wstring g_dir;
std::string g_gameName;
std::atomic<ULONGLONG> g_truckTick{0};  // when the player's truck was configured for the first time
bool g_decided = false;
bool g_observe = false;
ULONGLONG g_lastCheck = 0;

// What the player is told. Nobody reads the game console, so it is a small pop-up window (shown from its own thread, the
// game keeps running). The "not supported" one comes once per game + plugin version, not at every start.
enum class Notice { None, Unsupported, WorksAgain };
Notice g_notice = Notice::None;
bool g_noticeShown = false;

std::wstring StatePath() { return g_dir + L"\\state.ini"; }

bool CompatFlagged() {
    wchar_t game[256] = {}, plugin[64] = {};
    GetPrivateProfileStringW(L"compat", L"game", L"", game, 256, StatePath().c_str());
    GetPrivateProfileStringW(L"compat", L"plugin", L"", plugin, 64, StatePath().c_str());
    const bool failed = GetPrivateProfileIntW(L"compat", L"failed", 0, StatePath().c_str()) != 0;
    return failed && ep::ToWide(g_gameName) == game && ep::ToWide(ETSNP_VERSION) == plugin;
}

void CompatSet(bool failed) {
    if (!failed) {
        WritePrivateProfileStringW(L"compat", nullptr, nullptr, StatePath().c_str());
        return;
    }
    WritePrivateProfileStringW(L"compat", L"game", ep::ToWide(g_gameName).c_str(), StatePath().c_str());
    WritePrivateProfileStringW(L"compat", L"plugin", ep::ToWide(ETSNP_VERSION).c_str(), StatePath().c_str());
    WritePrivateProfileStringW(L"compat", L"failed", L"1", StatePath().c_str());
}

bool NoticeAlreadyGiven() {
    wchar_t game[256] = {}, plugin[64] = {};
    GetPrivateProfileStringW(L"compat", L"game", L"", game, 256, StatePath().c_str());
    GetPrivateProfileStringW(L"compat", L"plugin", L"", plugin, 64, StatePath().c_str());
    return GetPrivateProfileIntW(L"compat", L"noticed", 0, StatePath().c_str()) != 0 && ep::ToWide(g_gameName) == game &&
           ep::ToWide(ETSNP_VERSION) == plugin;
}

void MarkNoticed() {
    WritePrivateProfileStringW(L"compat", L"game", ep::ToWide(g_gameName).c_str(), StatePath().c_str());
    WritePrivateProfileStringW(L"compat", L"plugin", ep::ToWide(ETSNP_VERSION).c_str(), StatePath().c_str());
    WritePrivateProfileStringW(L"compat", L"noticed", L"1", StatePath().c_str());
}

void WarnUnsupported() {
    const std::string msg = std::string("[CoachNowPlaying] Plugin ") + ETSNP_VERSION + " does not support this game version (" + g_gameName +
                            ") yet, the normal GPS is used. Please look for an update: " + kGithub;
    GameLog(SCS_LOG_TYPE_warning, msg.c_str());
    if (g_notice == Notice::None) g_notice = Notice::Unsupported;
}

void ShowNotice(Notice kind) {
    const bool german = PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_GERMAN;
    std::wstring text;
    if (kind == Notice::Unsupported) {
        const std::wstring game = ep::ToWide(g_gameName), url = ep::ToWide(std::string(kGithub) + "/releases");
        text = german ? L"CoachNowPlaying unterstützt diese Spielversion noch nicht (" + game +
                            L").\n\nDein normales GPS bleibt in Benutzung, sonst ist nichts beeinträchtigt.\n"
                            L"Ein Plugin-Update gibt es hier:\n" + url
                      : L"CoachNowPlaying does not support this version of the game yet (" + game +
                            L").\n\nYour normal GPS stays in use, nothing else is affected.\n"
                            L"A plugin update will appear here:\n" + url;
    } else {
        text = german ? L"CoachNowPlaying funktioniert mit dieser Spielversion wieder.\n\nBitte starte das Spiel einmal neu."
                      : L"CoachNowPlaying works with this game version again.\n\nPlease restart the game once.";
    }
    // Own thread: MessageBox only blocks that thread, the game keeps running.
    std::thread([text] {
        MessageBoxW(nullptr, text.c_str(), L"CoachNowPlaying", MB_OK | MB_ICONINFORMATION | MB_SETFOREGROUND | MB_TOPMOST);
    }).detach();
}

// Shows a pending notice once the player is really in the game (the truck was loaded a few seconds ago).
void ProcessNotice(ULONGLONG now) {
    if (g_notice == Notice::None || g_noticeShown) return;
    if (g_notice == Notice::Unsupported) {
        const ULONGLONG truck = g_truckTick.load();
        if (!truck || now - truck < 8000) return;
        g_noticeShown = true;
        if (NoticeAlreadyGiven()) return;
        MarkNoticed();
    }
    g_noticeShown = true;
    ShowNotice(g_notice);
}

// Called once per frame (game thread), cheap once the decision is made.
void WatchTick() {
    const ULONGLONG now = GetTickCount64();
    if (now - g_lastCheck < 1000) return;
    g_lastCheck = now;
    ProcessNotice(now);
    if (g_decided) return;
    if (g_observe) {
        if (ep::EngineGpsSightings() > 0) {
            g_decided = true;
            CompatSet(false);
            ep::Logf("COMPAT the GPS texture lookup was seen again: this game version works with this plugin, mark removed");
            GameLog(SCS_LOG_TYPE_message, "[CoachNowPlaying] This game version works with the plugin again. Please restart the game once.");
            g_notice = Notice::WorksAgain;
            g_noticeShown = false;
        }
        return;
    }
    const ULONGLONG truck = g_truckTick.load();
    if (!truck || now - truck < 60000) return;
    g_decided = true;
    if (ep::EngineRedirects() > 0) {
        ep::Logf("COMPAT the GPS texture was redirected %d time(s): fine", ep::EngineRedirects());
        return;
    }
    ep::Logf("COMPAT no GPS texture redirect within a minute of the truck being loaded: game version '%s' is probably not supported yet",
             g_gameName.c_str());
    ep::EngineRestoreGameGps();
    CompatSet(true);
    WarnUnsupported();
}

SCSAPI_VOID OnFrameEnd(const scs_event_t, const void* const, const scs_context_t) { WatchTick(); }

// Telemetry for the ANKUNFT field (eta_screen.cpp). A null value means the channel has no value right now.
SCSAPI_VOID OnGameTime(const scs_string_t, const scs_u32_t, const scs_value_t* const v, const scs_context_t) {
    if (v && v->type == SCS_VALUE_TYPE_u32) ep::EtaSetGameTime(v->value_u32.value);
}
SCSAPI_VOID OnNavTime(const scs_string_t, const scs_u32_t, const scs_value_t* const v, const scs_context_t) {
    ep::EtaSetNavigationTime(v && v->type == SCS_VALUE_TYPE_float ? v->value_float.value : 0.0f);
}
SCSAPI_VOID OnSpeedLimit(const scs_string_t, const scs_u32_t, const scs_value_t* const v, const scs_context_t) {
    ep::EtaSetSpeedLimit(v && v->type == SCS_VALUE_TYPE_float ? v->value_float.value : 0.0f);
}
SCSAPI_VOID OnNavDistance(const scs_string_t, const scs_u32_t, const scs_value_t* const v, const scs_context_t) {
    ep::EtaSetNavigationDistance(v && v->type == SCS_VALUE_TYPE_float ? v->value_float.value : 0.0f);
}

// Electrics on/off: the CarPlay screen plays its start-up animation when the bus is switched on.
SCSAPI_VOID OnElectric(const scs_string_t, const scs_u32_t, const scs_value_t* const v, const scs_context_t) {
    if (v && v->type == SCS_VALUE_TYPE_bool) ep::EtaSetElectric(v->value_bool.value != 0);
}

SCSAPI_VOID OnTimeScale(const scs_string_t, const scs_u32_t, const scs_value_t* const v, const scs_context_t) {
    if (v && v->type == SCS_VALUE_TYPE_float) ep::PaxSetTimeScale(v->value_float.value);
}

double NumberOf(const scs_value_t& v) {
    switch (v.type) {
        case SCS_VALUE_TYPE_u32: return v.value_u32.value;
        case SCS_VALUE_TYPE_s32: return v.value_s32.value;
        case SCS_VALUE_TYPE_u64: return static_cast<double>(v.value_u64.value);
        case SCS_VALUE_TYPE_s64: return static_cast<double>(v.value_s64.value);
        case SCS_VALUE_TYPE_float: return v.value_float.value;
        case SCS_VALUE_TYPE_double: return v.value_double.value;
        default: return 0.0;
    }
}

SCSAPI_VOID OnConfiguration(const scs_event_t, const void* const info, const scs_context_t) {
    const auto* c = static_cast<const scs_telemetry_configuration_t*>(info);
    if (!c || !c->id) return;
    if (strcmp(c->id, "truck") == 0) {
        if (g_truckTick.load() == 0) g_truckTick = GetTickCount64();
        std::string ident;
        for (const scs_named_value_t* a = c->attributes; a && a->name; ++a) {
            if (a->value.type == SCS_VALUE_TYPE_string && a->value.value_string.value &&
                (strcmp(a->name, "brand_id") == 0 || strcmp(a->name, "id") == 0 || strcmp(a->name, "name") == 0))
                ident += std::string(a->value.value_string.value) + " ";
        }
        if (!ident.empty()) ep::PaxSetVehicle(ident);
        // number plate of the bus: drawn by the plugin on the rear luggage box (the game only uses the cabin's plate)
        std::string plate, country;
        for (const scs_named_value_t* a = c->attributes; a && a->name; ++a) {
            if (a->value.type != SCS_VALUE_TYPE_string || !a->value.value_string.value) continue;
            if (strcmp(a->name, SCS_TELEMETRY_CONFIG_ATTRIBUTE_license_plate) == 0) plate = a->value.value_string.value;
            else if (strcmp(a->name, SCS_TELEMETRY_CONFIG_ATTRIBUTE_license_plate_country_id) == 0) country = a->value.value_string.value;
        }
        if (!plate.empty()) ep::EtaSetPlate(plate, country);
    } else if (strcmp(c->id, "job") == 0) {
        std::string src, dst;
        float km = 0.0f;
        for (const scs_named_value_t* a = c->attributes; a && a->name; ++a) {
            if (strcmp(a->name, "source.city") == 0 && a->value.type == SCS_VALUE_TYPE_string && a->value.value_string.value)
                src = a->value.value_string.value;
            else if (strcmp(a->name, "destination.city") == 0 && a->value.type == SCS_VALUE_TYPE_string && a->value.value_string.value)
                dst = a->value.value_string.value;
            else if (strcmp(a->name, "planned_distance.km") == 0)
                km = static_cast<float>(NumberOf(a->value));
        }
        ep::PaxSetJob(src, dst, km);  // empty attribute set = no job
    }
}

}  // namespace

extern "C" SCSAPI_RESULT scs_telemetry_init(const scs_u32_t version,
                                            const scs_telemetry_init_params_t* const params) {
    if (version != SCS_TELEMETRY_VERSION_1_00 && version != SCS_TELEMETRY_VERSION_1_01) return SCS_RESULT_unsupported;
    const auto* p = static_cast<const scs_telemetry_init_params_v100_t*>(params);
    g_scsLog = p->common.log;

    // Never let the game unload us while hooks point into this module.
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                       reinterpret_cast<LPCWSTR>(&scs_telemetry_init), &self);

    std::string gameId = p->common.game_id ? p->common.game_id : "";
    const wchar_t* docsName = gameId == "ats" ? L"American Truck Simulator" : L"Euro Truck Simulator 2";
    std::wstring docs = KnownFolder(FOLDERID_Documents) + L"\\" + docsName;
    std::wstring dir = docs + L"\\CoachNowPlaying";
    g_dir = dir;
    g_gameName = p->common.game_name ? p->common.game_name : "";
    ep::SetGameDirs(docs, dir);
    ep::LogInit(dir);
    ep::Logf("CoachNowPlaying v" ETSNP_VERSION ", game_id='%s' game_name='%s' telemetry=%u.%u", gameId.c_str(),
             p->common.game_name ? p->common.game_name : "", SCS_GET_MAJOR_VERSION(version),
             SCS_GET_MINOR_VERSION(version));

    try {
        MH_STATUS st = MH_Initialize();
        if (st != MH_OK) {
            ep::Logf("MH_Initialize failed: %s", MH_StatusToString(st));
            GameLog(SCS_LOG_TYPE_error, "[CoachNowPlaying] MinHook init failed");
            return SCS_RESULT_ok;  // never block the game because of us
        }
        ep::LoadSettings(dir);
        ep::Features features = ep::SafeModeBegin(dir);
        ep::RadioTrackerInit();
        // Network hooks detect the in-game radio stream (station, song). ON by default. If joining a Convoy session
        // fails with "DLCs do not match", set [radio] stream_titles=0 in CoachNowPlaying.ini (no radio info then).
        const std::wstring iniPath = dir + L"\\CoachNowPlaying.ini";
        int streamTitles = static_cast<int>(GetPrivateProfileIntW(L"radio", L"stream_titles", -1, iniPath.c_str()));
        if (streamTitles < 0) {
            WritePrivateProfileStringW(L"radio", L"stream_titles", L"1", iniPath.c_str());
            streamTitles = 1;
        }
        bool net = streamTitles != 0 && ep::NetHooksInstall();
        ep::Logf("NET radio stream titles %s (CoachNowPlaying.ini [radio] stream_titles=%d)", net ? "on" : "off", streamTitles);
        // CoachNowPlaying: no game-code hooks at all. The "CarPlay-Style Infotainment for Coaches" mod ships its own
        // placeholder texture (material/ui/cpc_np/np.dds) on its MUSIK page; the D3D11 hook recognises that texture
        // and swaps it for our picture. The game's own GPS UI keeps running, so the version watchdog is not needed.
        g_observe = false;
        g_decided = true;
        const bool engine = false;
        features.touch = false;  // the picture is drawn inside the game's UI pass, not on the cabin mesh: no mouse mapping
        ep::FitInit(0);
        bool gfx = ep::D3D11HooksInstall(features.screen, features.upload, false);
        ep::CoverLookupStart();
        ep::ExtraSourcesStart();
        ep::StationLogoStart();
        ep::RendererStart();
        ep::Logf("init done: net=%d engine=%d d3d11=%d", net, engine, gfx);
        GameLog(SCS_LOG_TYPE_message, "[CoachNowPlaying] started, see Documents\\...\\CoachNowPlaying\\diag.log");
        if (p->register_for_event) {
            p->register_for_event(SCS_TELEMETRY_EVENT_configuration, OnConfiguration, nullptr);
            p->register_for_event(SCS_TELEMETRY_EVENT_frame_end, OnFrameEnd, nullptr);
        }
        if (p->register_for_channel) {
            p->register_for_channel(SCS_TELEMETRY_CHANNEL_game_time, SCS_U32_NIL, SCS_VALUE_TYPE_u32,
                                    SCS_TELEMETRY_CHANNEL_FLAG_none, OnGameTime, nullptr);
            p->register_for_channel(SCS_TELEMETRY_TRUCK_CHANNEL_navigation_time, SCS_U32_NIL, SCS_VALUE_TYPE_float,
                                    SCS_TELEMETRY_CHANNEL_FLAG_none, OnNavTime, nullptr);
            p->register_for_channel(SCS_TELEMETRY_TRUCK_CHANNEL_navigation_distance, SCS_U32_NIL, SCS_VALUE_TYPE_float,
                                    SCS_TELEMETRY_CHANNEL_FLAG_none, OnNavDistance, nullptr);
            p->register_for_channel(SCS_TELEMETRY_TRUCK_CHANNEL_navigation_speed_limit, SCS_U32_NIL, SCS_VALUE_TYPE_float,
                                    SCS_TELEMETRY_CHANNEL_FLAG_none, OnSpeedLimit, nullptr);
            p->register_for_channel(SCS_TELEMETRY_TRUCK_CHANNEL_electric_enabled, SCS_U32_NIL, SCS_VALUE_TYPE_bool,
                                    SCS_TELEMETRY_CHANNEL_FLAG_none, OnElectric, nullptr);
            p->register_for_channel(SCS_TELEMETRY_CHANNEL_local_scale, SCS_U32_NIL, SCS_VALUE_TYPE_float,
                                    SCS_TELEMETRY_CHANNEL_FLAG_none, OnTimeScale, nullptr);
        }
    } catch (...) {
        ep::Logf("exception during init");
    }
    return SCS_RESULT_ok;
}

extern "C" SCSAPI_VOID scs_telemetry_shutdown(void) {
    ep::Logf("shutdown");
    ep::NetHooksDisable();
    ep::D3D11HooksDisable();
    ep::RendererStop();
    ep::ExtraSourcesStop();
    ep::StationLogoStop();
    ep::CoverLookupStop();
    ep::RadioTrackerShutdown();
    ep::SafeModeEndClean();
    // Hooks stay installed (they pass through) because the module is pinned and the game is exiting.
}

BOOL APIENTRY DllMain(HMODULE, DWORD, LPVOID) { return TRUE; }
