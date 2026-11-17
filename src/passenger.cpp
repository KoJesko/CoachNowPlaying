#include "passenger.h"

#include "announce_text.h"
#include "tv_text.h"

#include "log.h"
#include "settings.h"
#include "text_util.h"

#include <mmsystem.h>
#include <sapi.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <cwctype>
#include <mutex>
#include <thread>

#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "sapi.lib")

namespace ep {
namespace {

std::mutex g_mx;
std::wstring g_source, g_dest;
float g_plannedKm = 0.0f;
std::string g_vehicle;
std::atomic<bool> g_isCoach{false};
std::atomic<float> g_scale{19.0f};

// announcement state
std::wstring g_announcedFor;      // job/route the announcement was made for
bool g_armed = false;             // the remaining time was above the window once (no announcement right after loading)
std::atomic<bool> g_speaking{false};

float RealSecondsLeft(float navSeconds) {
    const float s = g_scale.load();
    return navSeconds / (s > 0.1f ? s : 19.0f);
}

bool InWindow(float navSeconds, float navMeters) {
    const Settings& st = GetSettings();
    if (!st.announce || navSeconds <= 0.0f || navMeters <= 0.0f) return false;
    return RealSecondsLeft(navSeconds) <= st.announceMinutes * 60.0f;
}

// ---- sound helpers ----------------------------------------------------------------------------------------------------
std::vector<uint8_t> MakeWav(const std::vector<double>& s, int rate, double gain) {
    const size_t n = s.size();
    std::vector<uint8_t> w(44 + n * 2);
    auto put32 = [&](size_t at, uint32_t v) { memcpy(&w[at], &v, 4); };
    auto put16 = [&](size_t at, uint16_t v) { memcpy(&w[at], &v, 2); };
    memcpy(&w[0], "RIFF", 4); put32(4, static_cast<uint32_t>(36 + n * 2)); memcpy(&w[8], "WAVE", 4);
    memcpy(&w[12], "fmt ", 4); put32(16, 16); put16(20, 1); put16(22, 1); put32(24, rate); put32(28, rate * 2);
    put16(32, 2); put16(34, 16); memcpy(&w[36], "data", 4); put32(40, static_cast<uint32_t>(n * 2));
    for (size_t i = 0; i < n; ++i) {
        const int16_t v = static_cast<int16_t>(std::clamp(s[i] * gain * 32767.0, -32000.0, 32000.0));
        memcpy(&w[44 + i * 2], &v, 2);
    }
    return w;
}

// short soft "ding" before the announcement
std::vector<double> Ding(int rate, double amp) {
    std::vector<double> s(static_cast<size_t>(rate));
    for (size_t i = 0; i < s.size(); ++i) {
        const double t = i / double(rate), env = std::exp(-t * 4.5) * std::min(1.0, t * 300.0);
        s[i] = amp * env * (std::sin(2 * 3.14159265 * 1046.5 * t) + 0.3 * std::sin(2 * 3.14159265 * 2093 * t) +
                            0.2 * std::sin(2 * 3.14159265 * 1568 * t));
    }
    return s;
}

struct Biquad {
    double b0, b1, b2, a1, a2, x1 = 0, x2 = 0, y1 = 0, y2 = 0;
    Biquad(char type, double f, double q, double gainDb, int rate) {
        const double A = std::pow(10.0, gainDb / 40.0), w0 = 2 * 3.14159265 * f / rate, al = std::sin(w0) / (2 * q),
                     c = std::cos(w0);
        double B0, B1, B2, A0, A1, A2;
        if (type == 'h') { B0 = (1 + c) / 2; B1 = -(1 + c); B2 = (1 + c) / 2; A0 = 1 + al; A1 = -2 * c; A2 = 1 - al; }
        else if (type == 'l') { B0 = (1 - c) / 2; B1 = 1 - c; B2 = (1 - c) / 2; A0 = 1 + al; A1 = -2 * c; A2 = 1 - al; }
        else { B0 = 1 + al * A; B1 = -2 * c; B2 = 1 - al * A; A0 = 1 + al / A; A1 = -2 * c; A2 = 1 - al / A; }
        b0 = B0 / A0; b1 = B1 / A0; b2 = B2 / A0; a1 = A1 / A0; a2 = A2 / A0;
    }
    double operator()(double x) {
        const double y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
        x2 = x1; x1 = x; y2 = y1; y1 = y;
        return y;
    }
};

// "driver at the microphone" in a coach: band limit, a little presence, light compression, the ceiling speakers along
// the aisle (same signal a few ms later) and a few short reflections of the small upholstered room - no hall.
std::vector<double> BusSound(const std::vector<double>& x, int rate) {
    Biquad h1('h', 170, 0.7, 0, rate), h2('h', 170, 0.7, 0, rate), l1('l', 7000, 0.7, 0, rate), pr('p', 2500, 0.9, 3, rate),
        mud('p', 250, 1.0, -3, rate);
    std::vector<double> y(x.size());
    double env = 0;
    for (size_t i = 0; i < x.size(); ++i) {
        double v = mud(pr(l1(h2(h1(x[i])))));
        env = std::max(std::abs(v), env * 0.9995);
        if (env > 0.25) v *= 0.25 / env + 0.5 * (1 - 0.25 / env);
        y[i] = v;
    }
    const double tapMs[] = {0, 4.5, 9.0, 14.0, 21.0, 29.0, 38.0}, tapG[] = {1.0, 0.55, 0.40, 0.17, 0.13, 0.09, 0.06};
    std::vector<double> o(y.size() + rate / 12, 0.0);
    for (int t = 0; t < 7; ++t) {
        const size_t d = static_cast<size_t>(tapMs[t] * rate / 1000.0);
        for (size_t i = 0; i < y.size(); ++i) o[i + d] += y[i] * tapG[t];
    }
    Biquad l2('l', 6300, 0.7, 0, rate);
    double m = 1e-9;
    for (auto& v : o) { v = l2(v); m = std::max(m, std::abs(v)); }
    for (auto& v : o) v *= 0.75 / m;
    return o;
}

std::wstring PluginDir() {
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&PluginDir), &self);
    wchar_t buf[MAX_PATH] = {};
    GetModuleFileNameW(self, buf, MAX_PATH);
    std::wstring p = buf;
    const size_t s = p.find_last_of(L"\\/");
    return s == std::wstring::npos ? L"." : p.substr(0, s);
}


// Piper (neural text-to-speech, offline): <plugins>\CoachNowPlaying_piper\piper.exe + voice\*.onnx
bool PiperSay(const std::wstring& text, const AnnounceLang& lang, std::vector<double>& out, int& rate) {
    const std::wstring dir = PluginDir() + L"\\CoachNowPlaying_piper";
    const std::wstring exe = dir + L"\\piper.exe";
    if (GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES) return false;
    const std::wstring model = PiperVoiceFile(dir, lang);
    if (model.empty()) { Logf("ANNOUNCE no Piper voice for '%ls' in CoachNowPlaying_piper\\voice", lang.Code().c_str()); return false; }
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    const std::wstring wav = std::wstring(tmp) + L"CoachNowPlaying_announce.wav";
    DeleteFileW(wav.c_str());
    std::wstring cmd = L"\"" + exe + L"\" --model \"" + model + L"\" --output_file \"" + wav + L"\" --sentence_silence 0.35";
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return false;
    SetHandleInformation(wr, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = rd;
    si.hStdOutput = INVALID_HANDLE_VALUE;
    si.hStdError = INVALID_HANDLE_VALUE;
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> cl(cmd.begin(), cmd.end());
    cl.push_back(0);
    const BOOL ok = CreateProcessW(nullptr, cl.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, dir.c_str(), &si, &pi);
    CloseHandle(rd);
    if (!ok) { CloseHandle(wr); Logf("ANNOUNCE could not start piper (%lu)", GetLastError()); return false; }
    const std::string utf8 = ToUtf8(text) + "\n";
    DWORD written = 0;
    WriteFile(wr, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
    CloseHandle(wr);
    const DWORD w = WaitForSingleObject(pi.hProcess, 30000);
    if (w != WAIT_OBJECT_0) TerminateProcess(pi.hProcess, 1);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    FILE* fp = _wfopen(wav.c_str(), L"rb");
    if (!fp) { Logf("ANNOUNCE piper made no sound file"); return false; }
    std::vector<uint8_t> b;
    uint8_t chunk[65536];
    size_t n;
    while ((n = fread(chunk, 1, sizeof(chunk), fp)) > 0) b.insert(b.end(), chunk, chunk + n);
    fclose(fp);
    DeleteFileW(wav.c_str());
    size_t pos = 12;
    int ch = 1;
    rate = 22050;
    while (pos + 8 <= b.size()) {
        uint32_t len;
        memcpy(&len, &b[pos + 4], 4);
        if (memcmp(&b[pos], "fmt ", 4) == 0 && pos + 16 <= b.size()) {
            uint16_t c; memcpy(&c, &b[pos + 10], 2); ch = c;
            uint32_t r; memcpy(&r, &b[pos + 12], 4); rate = static_cast<int>(r);
        }
        if (memcmp(&b[pos], "data", 4) == 0) {
            const size_t cnt = std::min<size_t>(len, b.size() - pos - 8) / 2 / std::max(1, ch);
            out.resize(cnt);
            for (size_t i = 0; i < cnt; ++i) {
                int16_t v; memcpy(&v, &b[pos + 8 + i * 2 * ch], 2);
                out[i] = v / 32768.0;
            }
            return !out.empty();
        }
        pos += 8 + len + (len & 1);
    }
    return false;
}
// Picks a voice for the language (LCID hex, e.g. L"407" = German); true if one was found.
bool PickVoice(ISpVoice* voice, const wchar_t* lcid) {
    ISpObjectTokenCategory* cat = nullptr;
    if (FAILED(CoCreateInstance(CLSID_SpObjectTokenCategory, nullptr, CLSCTX_ALL, IID_ISpObjectTokenCategory,
                                reinterpret_cast<void**>(&cat))))
        return false;
    bool ok = false;
    if (SUCCEEDED(cat->SetId(SPCAT_VOICES, FALSE))) {
        IEnumSpObjectTokens* en = nullptr;
        std::wstring req = std::wstring(L"Language=") + lcid;
        if (SUCCEEDED(cat->EnumTokens(req.c_str(), nullptr, &en)) && en) {
            ISpObjectToken* tok = nullptr;
            if (en->Next(1, &tok, nullptr) == S_OK && tok) {
                ok = SUCCEEDED(voice->SetVoice(tok));
                tok->Release();
            }
            en->Release();
        }
    }
    cat->Release();
    return ok;
}

// Windows (SAPI) voice for the language, e.g. de_de -> "Language=407". true if one was found.
bool PickWindowsVoice(ISpVoice* voice, const AnnounceLang& lang) {
    std::wstring name = lang.region.empty() ? lang.base : lang.base + L"-" + lang.region;
    if (lang.base == L"es" && lang.region == L"la") name = L"es-MX";
    wchar_t full[LOCALE_NAME_MAX_LENGTH] = {};
    if (ResolveLocaleName(name.c_str(), full, LOCALE_NAME_MAX_LENGTH) <= 0) return false;
    for (const wchar_t* n : {name.c_str(), static_cast<const wchar_t*>(full)}) {
        const LCID id = LocaleNameToLCID(n, 0);
        if (!id || PRIMARYLANGID(LANGIDFROMLCID(id)) == LANG_NEUTRAL) continue;
        wchar_t hex[16];
        swprintf(hex, 16, L"%x", LANGIDFROMLCID(id));
        if (PickVoice(voice, hex)) return true;
    }
    return false;
}

std::vector<uint8_t> WithDing(const std::vector<double>& voice, int rate, double gain, bool ding) {
    std::vector<double> all;
    if (ding) {
        all = Ding(rate, 0.10);
        all.resize(all.size() + rate / 10, 0.0);
    }
    std::vector<double> bus = BusSound(voice, rate);
    all.insert(all.end(), bus.begin(), bus.end());
    return MakeWav(all, rate, gain);
}

// One announcement per configured language (auto = the game's language), the chime before the first.
// Per language: Piper voice of that language (coach sound) -> Windows voice of that language -> English.
void Speak(std::wstring dest, int hour) {
    const Settings& st = GetSettings();
    const double gain = std::clamp(st.announceVolume, 0, 100) / 100.0;
    bool first = true;
    bool comTried = false, com = false;
    for (AnnounceLang lang : AnnounceLanguages(st.announceLanguage)) {
        for (int attempt = 0; attempt < 2; ++attempt) {
            const std::wstring text = AnnounceText(lang, dest, hour);
            std::vector<double> voice;
            int rate = 22050;
            if (PiperSay(text, lang, voice, rate)) {
                std::vector<uint8_t> wav = WithDing(voice, rate, gain, first);
                Logf("ANNOUNCE (Piper %ls): %ls", lang.Code().c_str(), text.c_str());
                PlaySoundW(reinterpret_cast<LPCWSTR>(wav.data()), nullptr, SND_MEMORY | SND_SYNC | SND_NODEFAULT);
                first = false;
                break;
            }
            if (!comTried) { comTried = true; com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)); }
            ISpVoice* sp = nullptr;
            bool spoke = false;
            if (SUCCEEDED(CoCreateInstance(CLSID_SpVoice, nullptr, CLSCTX_ALL, IID_ISpVoice, reinterpret_cast<void**>(&sp)))) {
                if (PickWindowsVoice(sp, lang)) {
                    if (first) {
                        std::vector<uint8_t> ding = MakeWav(Ding(22050, 0.10), 22050, gain);
                        PlaySoundW(reinterpret_cast<LPCWSTR>(ding.data()), nullptr, SND_MEMORY | SND_SYNC | SND_NODEFAULT);
                        first = false;
                    }
                    sp->SetVolume(static_cast<USHORT>(std::clamp(st.announceVolume, 0, 100)));
                    sp->SetRate(-1);
                    Logf("ANNOUNCE (Windows %ls voice): %ls", lang.Code().c_str(), text.c_str());
                    sp->Speak(text.c_str(), SPF_DEFAULT, nullptr);
                    spoke = true;
                }
                sp->Release();
            }
            if (spoke) break;
            if (lang.base == L"en") { Logf("ANNOUNCE no voice for '%ls' and none for English", lang.Code().c_str()); break; }
            Logf("ANNOUNCE no voice for '%ls' (Piper or Windows), English instead", lang.Code().c_str());
            lang.base = L"en";
            lang.region = L"gb";
        }
    }
    if (com) CoUninitialize();
    g_speaking = false;
}
}  // namespace

void PaxSetJob(const std::string& sourceCity, const std::string& destinationCity, float plannedKm) {
    std::lock_guard<std::mutex> lock(g_mx);
    g_source = ToWide(sourceCity);
    g_dest = ToWide(destinationCity);
    g_plannedKm = plannedKm;
    Logf("PAX job: '%s' -> '%s', %.0f km", sourceCity.c_str(), destinationCity.c_str(), plannedKm);
}

void PaxSetVehicle(const std::string& idText) {
    std::string low = idText;
    for (auto& c : low) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    const char* words[] = {"bus", "coach", "lion", "9700", "touring", "tourismo", "setra", "travego", "intercity"};
    bool coach = false;
    for (const char* w : words) coach = coach || low.find(w) != std::string::npos;
    {
        std::lock_guard<std::mutex> lock(g_mx);
        g_vehicle = idText;
    }
    g_isCoach = coach;
    Logf("PAX vehicle '%s' -> %s", idText.c_str(), coach ? "coach (announcements on)" : "no coach");
}

void PaxSetTimeScale(float scale) { g_scale = scale; }

PaxView PaxGetView(uint32_t gameMinutes, bool haveTime, float navSeconds, float navMeters) {
    PaxView v;
    float planned;
    {
        std::lock_guard<std::mutex> lock(g_mx);
        v.source = g_source;
        v.destination = g_dest;
        planned = g_plannedKm;
    }
    if (planned > 0.5f && navMeters > 0.0f) v.progress = std::clamp(1.0f - navMeters / (planned * 1000.0f), 0.0f, 1.0f);
    v.arriving = InWindow(navSeconds, navMeters);
    wchar_t buf[96];
    swprintf(buf, 96, L"|%u|%d|%d|%d|%d", haveTime ? gameMinutes : 0u, static_cast<int>(navMeters / 1000.0f),
             static_cast<int>(navSeconds / 60.0f), v.progress < 0 ? -1 : static_cast<int>(v.progress * 200), v.arriving ? 1 : 0);
    v.key = v.source + L"|" + v.destination + buf;
    return v;
}

void PaxTick(uint32_t gameMinutes, float navSeconds, float navMeters) {
    const Settings& st = GetSettings();
    if (!st.announce || !g_isCoach.load()) return;
    std::wstring dest;
    {
        std::lock_guard<std::mutex> lock(g_mx);
        dest = g_dest;
    }
    const std::wstring routeKey = dest.empty() ? L"(route)" : dest;
    if (navSeconds <= 0.0f || navMeters <= 0.0f) {  // no route (arrived / cleared): next route may announce again
        g_announcedFor.clear();
        g_armed = false;
        return;
    }
    const float left = RealSecondsLeft(navSeconds);
    if (left > st.announceMinutes * 60.0f + 30.0f) {
        g_armed = true;
        if (g_announcedFor == routeKey) g_announcedFor.clear();  // route got longer again
        return;
    }
    if (!g_armed || g_announcedFor == routeKey || left < 15.0f || navMeters < 150.0f) return;
    if (g_speaking.exchange(true)) return;
    g_announcedFor = routeKey;
    const int hour = static_cast<int>((gameMinutes % 1440u) / 60u);
    Logf("ANNOUNCE %.0f s (real) / %.0f m before '%ls'", left, navMeters, routeKey.c_str());
    std::thread(Speak, dest, hour).detach();
}

// ---- TV picture ----------------------------------------------------------------------------------------------------
std::vector<uint8_t> PaxDraw(UINT w, UINT h, const PaxView& v, uint32_t gameMinutes, bool haveTime, float navSeconds,
                             float navMeters) {
    return PaxDrawText(w, h, v, gameMinutes, haveTime, navSeconds, navMeters, GetTvText(CurrentGameLanguage()));
}

std::vector<uint8_t> PaxDrawText(UINT w, UINT h, const PaxView& v, uint32_t gameMinutes, bool haveTime, float navSeconds,
                                 float navMeters, const TvText& tx) {
    std::vector<uint8_t> out(static_cast<size_t>(w) * h * 4, 255);
    HDC dc = CreateCompatibleDC(nullptr);
    if (!dc) return out;
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = static_cast<LONG>(w);
    bi.bmiHeader.biHeight = -static_cast<LONG>(h);
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bmp || !bits) {
        DeleteDC(dc);
        return out;
    }
    HGDIOBJ oldBmp = SelectObject(dc, bmp);
    SetBkMode(dc, TRANSPARENT);
    auto box = [&](int x, int y, int bw, int bh, COLORREF c) {
        RECT r{x, y, x + bw, y + bh};
        HBRUSH b = CreateSolidBrush(c);
        FillRect(dc, &r, b);
        DeleteObject(b);
    };
    auto text = [&](const std::wstring& s, int px, bool bold, COLORREF c, int x, int y, int x2, int y2, UINT fmt) {
        HFONT f = CreateFontW(-px, 0, 0, 0, bold ? FW_SEMIBOLD : FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                              CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, bold ? L"Segoe UI Semibold" : L"Segoe UI");
        HGDIOBJ of = SelectObject(dc, f);
        // longer translations: smaller font until the line fits (down to 60 %)
        for (int p = px; p > px * 6 / 10; p = p * 92 / 100) {
            RECT m{0, 0, 0, 0};
            DrawTextW(dc, s.c_str(), -1, &m, DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
            if (m.right <= x2 - x) break;
            SelectObject(dc, of);
            DeleteObject(f);
            f = CreateFontW(-(p * 92 / 100), 0, 0, 0, bold ? FW_SEMIBOLD : FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                            CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, bold ? L"Segoe UI Semibold" : L"Segoe UI");
            of = SelectObject(dc, f);
        }
        SetTextColor(dc, c);
        RECT r{x, y, x2, y2};
        DrawTextW(dc, s.c_str(), -1, &r, fmt | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
        SelectObject(dc, of);
        DeleteObject(f);
    };
    const COLORREF dark = RGB(0x0E, 0x11, 0x17), navy = RGB(0x1B, 0x2A, 0x44), white = RGB(0xF4, 0xF5, 0xF7),
                   grey = RGB(0x5B, 0x6B, 0x85), red = RGB(0xD3, 0x2F, 0x2F), blue = RGB(0x1F, 0x5F, 0xBF),
                   yellow = RGB(0xF2, 0xC9, 0x4C), brown = RGB(0x3B, 0x2A, 0x05), brown2 = RGB(0x6B, 0x53, 0x10),
                   wine = RGB(0xB3, 0x26, 0x1E), light = RGB(0xAA, 0xB4, 0xC8);
    const int W = static_cast<int>(w), H = static_cast<int>(h);
    box(0, 0, W, H, dark);

    // header: destination
    box(0, 0, W, 228, white);
    box(0, 0, 30, 228, red);
    box(W - 220, 0, 220, 228, navy);
    const bool haveDest = !v.destination.empty();
    const TvText en = GetTvText(AnnounceLang{L"en", L"gb"});
    auto both = [&](const std::wstring& a, const std::wstring& b) { return tx.english || a == b ? a : a + L"  ·  " + b; };
    auto fill = [](std::wstring s, const wchar_t* key, const std::wstring& val) {
        const size_t p = s.find(key);
        if (p != std::wstring::npos) s.replace(p, wcslen(key), val);
        return s;
    };
    text(haveDest ? both(tx.destination, en.destination) : L"MAN Lion's Coach", 28, false, grey, 62, 24, W - 240, 62, DT_LEFT | DT_VCENTER);
    text(haveDest ? v.destination : tx.welcome, 70, true, navy, 56, 62, W - 236, 150, DT_LEFT | DT_VCENTER);
    std::wstring sub = tx.english ? std::wstring() : en.welcome;
    if (navSeconds > 0.0f && navMeters > 0.0f && haveTime) {
        const uint64_t eta = gameMinutes + static_cast<uint64_t>(std::llround(navSeconds / 60.0f));
        wchar_t hm[16], km[16];
        swprintf(hm, 16, L"%02u:%02u", static_cast<unsigned>((eta % 1440u) / 60u), static_cast<unsigned>(eta % 60u));
        swprintf(km, 16, L"%d", static_cast<int>(std::lround(navMeters / 1000.0f)));
        sub = fill(fill(tx.arrival, L"{T}", hm), L"{K}", km);
    }
    text(sub, 32, false, grey, 62, 156, W - 240, 206, DT_LEFT | DT_VCENTER);
    // clock + weekday
    if (haveTime) {
        wchar_t b[16];
        swprintf(b, 16, L"%02u:%02u", (gameMinutes % 1440u) / 60u, gameMinutes % 60u);
        text(b, 72, true, RGB(0xFF, 0xFF, 0xFF), W - 220, 40, W, 140, DT_CENTER | DT_VCENTER);
        text(tx.days[(gameMinutes / 1440u) % 7u], 30, false, light, W - 220, 142, W, 190, DT_CENTER | DT_VCENTER);
    }

    // progress bar source -> destination
    int y = 248;
    if (v.progress >= 0.0f) {
        text(v.source, 30, false, light, 40, y, W / 2, y + 40, DT_LEFT | DT_VCENTER);
        text(v.destination, 30, false, light, W / 2, y, W - 40, y + 40, DT_RIGHT | DT_VCENTER);
        const int bx = 40, bw = W - 80, by = y + 50;
        box(bx, by, bw, 14, RGB(0x2A, 0x2F, 0x3A));
        const int px = bx + static_cast<int>(bw * v.progress);
        box(bx, by, std::max(0, px - bx), 14, blue);
        HBRUSH wb = CreateSolidBrush(RGB(255, 255, 255));
        HPEN pen = CreatePen(PS_SOLID, 6, blue);
        HGDIOBJ ob = SelectObject(dc, wb), op = SelectObject(dc, pen);
        Ellipse(dc, px - 17, by - 10, px + 17, by + 24);
        SelectObject(dc, ob); SelectObject(dc, op);
        DeleteObject(wb); DeleteObject(pen);
        y += 96;
    } else {
        y += 20;
    }

    // yellow info panel
    const int py = y, ph = H - 36 - py;
    box(34, py, W - 68, ph, yellow);
    if (v.arriving) {
        text(both(tx.nextStop, en.nextStop), 34, false, brown2, 70, py + 24, W - 70, py + 70, DT_LEFT | DT_VCENTER);
        text(haveDest ? v.destination : (tx.english || tx.target == en.target ? tx.target : tx.target + L" / " + en.target), 84, true, wine, 64, py + 76, W - 70, py + 186, DT_LEFT | DT_VCENTER);
        text(tx.belongings, 36, false, brown, 70, py + ph - 128, W - 70, py + ph - 78, DT_LEFT | DT_VCENTER);
        if (!tx.english) text(en.belongings, 30, false, brown2, 70, py + ph - 76, W - 70, py + ph - 30, DT_LEFT | DT_VCENTER);
    } else {
        text(tx.goodTrip, 64, true, wine, 66, py + 18, W - 70, py + 110, DT_LEFT | DT_VCENTER);
        const int ly = py + ph - 250 > py + 110 ? py + ph - 250 : py + 110;
        text(tx.seatBelt, 36, false, brown, 70, ly, W - 70, ly + 50, DT_LEFT | DT_VCENTER);
        if (!tx.english) text(en.seatBelt, 30, false, brown2, 70, ly + 50, W - 70, ly + 94, DT_LEFT | DT_VCENTER);
        text(tx.wifi, 36, false, brown, 70, ly + 130, W - 70, ly + 180, DT_LEFT | DT_VCENTER);
        if (!tx.english) text(en.wifi, 30, false, brown2, 70, ly + 180, W - 70, ly + 224, DT_LEFT | DT_VCENTER);
    }
    GdiFlush();
    memcpy(out.data(), bits, out.size());
    for (size_t i = 3; i < out.size(); i += 4) out[i] = 255;
    SelectObject(dc, oldBmp);
    DeleteObject(bmp);
    DeleteDC(dc);
    return out;
}

}  // namespace ep
