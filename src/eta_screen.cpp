#include "eta_screen.h"

#include "boot_anim.h"
#include "plate.h"

#include "cover_lookup.h"
#include "log.h"
#include "media.h"
#include "passenger.h"
#include "radio_tracker.h"
#include "settings.h"
#include "station_logo.h"
#include "text_util.h"

#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <thread>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

namespace ep {
namespace {

std::atomic<uint32_t> g_gameMinutes{0};
std::atomic<bool> g_haveGameTime{false};
std::atomic<float> g_navSeconds{0.0f};
std::atomic<float> g_navMeters{0.0f};
std::atomic<float> g_speedLimit{0.0f};

constexpr int kMaxViews = 8;

struct Field {
    const char* name;
    UINT phW, phH;     // placeholder size in the mod (BC3, unique)
    UINT w, h;         // our texture
    bool opaque;       // opaque: background colour bg; otherwise transparent background
    COLORREF bg, fg;
    int fontPx;        // start size, shrinks until the text fits
    bool center;
    const wchar_t* face;
    int weight;
    int kind;          // 0 = ETA, 1 = LIMIT, 2 = NOW PLAYING bar, 3 = passenger TV, 4 = boot animation, 5 = off marker, 6 = number plate

    ID3D11Texture2D* placeholder = nullptr;
    ID3D11Texture2D* tex = nullptr;
    ID3D11ShaderResourceView* srv = nullptr;
    std::atomic<ID3D11ShaderResourceView*> srvAtomic{nullptr};
    bool srgb = false;
    std::atomic<ID3D11ShaderResourceView*> views[kMaxViews];
    std::wstring shown;
    ID3D11Texture2D* shownIn = nullptr;
    std::atomic<ULONGLONG> lastBound{0};  // last time the game drew this placeholder (= its page is on screen)
};

// Colours match the mod: card 0xFF1A1E26, text 0xFFE4E7EC, sign digits 0xFF101215.
Field g_fields[7] = {
    {"ETA", 52, 1996, 256, 96, true, RGB(0x1A, 0x1E, 0x26), RGB(0xE4, 0xE7, 0xEC), 84, false, L"Segoe UI Semibold", FW_SEMIBOLD, 0},
    {"LIMIT", 44, 1996, 128, 128, false, RGB(0, 0, 0), RGB(0x10, 0x12, 0x15), 54, true, L"Arial", FW_BOLD, 1},
    {"NOWBAR", 36, 1996, 768, 96, true, RGB(0x1A, 0x1E, 0x26), RGB(0xE4, 0xE7, 0xEC), 36, false, L"Segoe UI Semibold", FW_SEMIBOLD, 2},
    {"PAXTV", 1024, 760, 1024, 760, true, RGB(0x0E, 0x11, 0x17), RGB(0xFF, 0xFF, 0xFF), 0, false, L"Segoe UI", FW_NORMAL, 3},
    {"BOOT", 28, 1996, 640, 360, false, RGB(0, 0, 0), RGB(0, 0, 0), 0, false, L"Segoe UI", FW_NORMAL, 4},
    {"OFF", 20, 1996, 4, 4, true, RGB(0, 0, 0), RGB(0, 0, 0), 0, false, L"Segoe UI", FW_NORMAL, 5},
    {"PLATE", 12, 1996, 1040, 220, true, RGB(0, 0, 0), RGB(0, 0, 0), 0, false, L"Bahnschrift", FW_NORMAL, 6},
};
constexpr int kBootField = 4;

// Start-up animation: armed (overlay opaque black) while the screen is off - black off page or electrics off - and
// started when a page with the boot placeholder is drawn after switching on.
bool g_bootArmed = false;
ULONGLONG g_bootStart = 0;
ULONGLONG g_electricOnAt = 0;  // electrics switched on at this time; the animation waits until the screen is drawn
std::atomic<int> g_electric{-1};             // -1 unknown, 0 off, 1 on
std::atomic<bool> g_electricOnEvent{false};  // set by the telemetry thread, handled on the Present thread

std::mutex g_plateMx;                        // number plate from the telemetry (telemetry thread -> Present thread)
std::string g_plateRaw, g_plateCountry;

std::mutex g_mx;  // guards placeholder/tex/srv of all fields
ULONGLONG g_lastLog = 0;

// Drawing thread: the pictures (GDI text, passenger TV 1024x760, cover decoding, boot animation) used to be drawn on
// the game's render thread inside Present, which held up the game's frame every time a text changed (small stutters,
// e.g. every in-game minute for the passenger TV). Now the Present thread only queues a job and uploads finished
// pictures; one job per field at a time, a newer key is queued once the previous picture is back.
struct DrawDone {
    bool has = false;
    std::wstring key;
    ID3D11Texture2D* tex = nullptr;  // only compared, never dereferenced
    std::vector<uint8_t> px;
};
struct DrawJob {
    size_t field;
    std::wstring key;
    ID3D11Texture2D* tex;
    std::function<std::vector<uint8_t>()> draw;
};
std::mutex g_jobMx;
std::condition_variable g_jobCv;
std::deque<DrawJob> g_jobs;
DrawDone g_done[7];
bool g_busy[7] = {};
bool g_drawStop = false;
std::thread g_drawThread;

void DrawThread() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);  // WIC (cover pictures)
    for (;;) {
        DrawJob job;
        {
            std::unique_lock<std::mutex> lock(g_jobMx);
            g_jobCv.wait(lock, [] { return g_drawStop || !g_jobs.empty(); });
            if (g_drawStop) break;
            job = std::move(g_jobs.front());
            g_jobs.pop_front();
        }
        std::vector<uint8_t> px = job.draw();
        std::lock_guard<std::mutex> lock(g_jobMx);
        g_done[job.field] = DrawDone{true, std::move(job.key), job.tex, std::move(px)};
        g_busy[job.field] = false;
    }
    CoUninitialize();
}

void DrawQueue(size_t field, const std::wstring& key, ID3D11Texture2D* tex, std::function<std::vector<uint8_t>()> draw) {
    std::lock_guard<std::mutex> lock(g_jobMx);
    if (g_drawStop || g_busy[field] || g_done[field].has) return;  // previous picture not uploaded yet: next frame
    if (!g_drawThread.joinable()) g_drawThread = std::thread(DrawThread);
    g_busy[field] = true;
    g_jobs.push_back(DrawJob{field, key, tex, std::move(draw)});
    g_jobCv.notify_one();
}

void EtaText(std::wstring& main, std::wstring& suffix) {
    const float nav = g_navSeconds.load();
    const float dist = g_navMeters.load();
    if (!g_haveGameTime.load() || nav <= 0.0f || dist <= 0.0f) {
        main = L"--:--";
        return;
    }
    // truck.navigation.time is the advisor's remaining time in in-game seconds.
    const uint64_t now = g_gameMinutes.load();
    const uint64_t eta = now + static_cast<uint64_t>(std::llround(nav / 60.0f));
    const unsigned minuteOfDay = static_cast<unsigned>(eta % 1440u);
    const unsigned days = static_cast<unsigned>(eta / 1440u - now / 1440u);
    wchar_t buf[16];
    swprintf(buf, 16, L"%02u:%02u", minuteOfDay / 60u, minuteOfDay % 60u);
    main = buf;
    if (days > 0) {
        swprintf(buf, 16, L"+%u", days);
        suffix = buf;
    }
}

void LimitText(std::wstring& main) {
    const float v = g_speedLimit.load();
    if (v < 0.5f) return;  // no limit known: the whole sign is hidden
    wchar_t buf[16];
    swprintf(buf, 16, L"%d", static_cast<int>(std::lround(v * 3.6f)));
    main = buf;
}

HFONT MakeFont(const Field& f, int px) {
    return CreateFontW(-px, 0, 0, 0, f.weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                       ANTIALIASED_QUALITY, DEFAULT_PITCH, f.face);
}

// Draws white-on-black with GDI (grey anti-aliasing = coverage), then turns the coverage into the field's colours.
std::vector<uint8_t> Draw(const Field& f, const std::wstring& main, const std::wstring& suffix, UINT width = 0) {
    const UINT W = width ? width : f.w;  // narrower drawing area (e.g. digits inside the speed sign)
    std::vector<uint8_t> out(static_cast<size_t>(W) * f.h * 4, 0);
    HDC dc = CreateCompatibleDC(nullptr);
    if (!dc) return out;
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = static_cast<LONG>(W);
    bi.bmiHeader.biHeight = -static_cast<LONG>(f.h);  // top-down
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
    memset(bits, 0, out.size());
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(255, 255, 255));

    if (!main.empty()) {
        // shrink until main + suffix fit with a small margin
        int px = f.fontPx;
        SIZE ms{}, ss{};
        HFONT mf = nullptr, sf = nullptr;
        for (;; px -= 2) {
            if (mf) DeleteObject(mf);
            if (sf) DeleteObject(sf);
            mf = MakeFont(f, px);
            sf = MakeFont(f, px * 45 / 100);
            SelectObject(dc, mf);
            GetTextExtentPoint32W(dc, main.c_str(), static_cast<int>(main.size()), &ms);
            ss = {0, 0};
            if (!suffix.empty()) {
                SelectObject(dc, sf);
                GetTextExtentPoint32W(dc, suffix.c_str(), static_cast<int>(suffix.size()), &ss);
                ss.cx += px / 10;  // gap
            }
            if (ms.cx + ss.cx <= static_cast<LONG>(W) - 4 || px <= 20) break;
        }
        const int total = ms.cx + ss.cx;
        const int x = f.center ? (static_cast<int>(W) - total) / 2 : 0;
        const int y = (static_cast<int>(f.h) - ms.cy) / 2;
        SelectObject(dc, mf);
        TextOutW(dc, x, y, main.c_str(), static_cast<int>(main.size()));
        if (!suffix.empty()) {
            SelectObject(dc, sf);
            TextOutW(dc, x + ms.cx + px / 10, y + ms.cy / 8, suffix.c_str(), static_cast<int>(suffix.size()));
        }
        SelectObject(dc, GetStockObject(SYSTEM_FONT));
        DeleteObject(mf);
        DeleteObject(sf);
    }
    GdiFlush();

    const uint8_t* src = static_cast<const uint8_t*>(bits);
    const int fb = GetBValue(f.fg), fgc = GetGValue(f.fg), fr = GetRValue(f.fg);
    const int bb = GetBValue(f.bg), bgc = GetGValue(f.bg), br = GetRValue(f.bg);
    for (size_t i = 0; i < out.size(); i += 4) {
        const int a = (src[i] + src[i + 1] + src[i + 2]) / 3;  // coverage
        if (f.opaque) {
            out[i] = static_cast<uint8_t>((fb * a + bb * (255 - a)) / 255);
            out[i + 1] = static_cast<uint8_t>((fgc * a + bgc * (255 - a)) / 255);
            out[i + 2] = static_cast<uint8_t>((fr * a + br * (255 - a)) / 255);
            out[i + 3] = 255;
        } else {
            out[i] = static_cast<uint8_t>(fb);
            out[i + 1] = static_cast<uint8_t>(fgc);
            out[i + 2] = static_cast<uint8_t>(fr);
            out[i + 3] = static_cast<uint8_t>(a);
        }
    }
    SelectObject(dc, oldBmp);
    DeleteObject(bmp);
    DeleteDC(dc);
    return out;
}

// ---------------------------------------------------------------------------------------------
// NOW PLAYING bar (KARTE page): picture | title / artist + source. Radio wins while a station plays.
// ---------------------------------------------------------------------------------------------
struct NowInfo {
    std::wstring title, sub;  // sub = "Artist · Station"
    std::shared_ptr<const std::vector<uint8_t>> picture;
    std::wstring key;
    bool visible = false;  // false = radio off: the whole bar is transparent
};

NowInfo GetNow() {
    NowInfo n;
    // Radio only (no Spotify / media players). While switching stations the stream is closed for a moment:
    // keep the last radio info for a short while so the bar does not flicker.
    static RadioSnapshot s_last;
    static ULONGLONG s_lastTick = 0;
    RadioSnapshot r = RadioGetSnapshot();
    const ULONGLONG tick = GetTickCount64();
    if (r.active) {
        s_last = r;
        s_lastTick = tick;
    } else if (s_lastTick != 0 && (r.connecting || tick - s_lastTick < static_cast<ULONGLONG>(GetSettings().radioHoldMs))) {
        r = s_last;
        r.active = true;
    }
    if (r.active) {
        n.visible = true;
        const std::string station = r.stationName.empty() ? r.icyName : r.stationName;
        const std::wstring stationW = ToWide(station);
        const std::string raw = r.title;
        std::wstring artist, song;
        const bool hasSong = !raw.empty() && NormalizeKey(ToWide(raw)) != NormalizeKey(stationW);
        if (hasSong) SplitArtistTitle(raw, artist, song);
        n.title = hasSong ? song : (stationW.empty() ? L"Radio" : stationW);
        n.sub = hasSong ? (artist.empty() ? stationW : artist + L"  ·  " + stationW) : L"RADIO";
        if (hasSong && !artist.empty() && GetSettings().coverLookup) {
            CoverResult c = CoverLookupGet(raw);
            if (c.state == CoverState::Found) n.picture = c.image;
        }
        if (!n.picture && GetSettings().coverLookup && !station.empty()) {
            LogoResult l = StationLogoGet(station, r.stationUrl);
            if (l.state == LogoState::Found) n.picture = l.image;
        }
    }
#ifndef _MSC_VER
    else {  // Linux build: system "now playing" while the in-game radio is off
        const MediaSnapshot m = MediaGetSnapshot();
        if (m.hasSession && m.playing && !m.title.empty()) {
            n.visible = true;
            n.title = ToWide(m.title);
            n.sub = ToWide(m.artist);
            if (!m.album.empty() && m.album != m.title) n.sub += (n.sub.empty() ? L"" : L"  \u00b7  ") + ToWide(m.album);
            n.picture = m.cover;
        }
    }
#endif
    wchar_t ptr[32];
    swprintf(ptr, 32, L"|%p", static_cast<const void*>(n.picture.get()));
    n.key = (n.visible ? L"1|" : L"0|") + n.title + L"|" + n.sub + ptr;
    return n;
}

// Decodes PNG/JPEG/ICO bytes and draws them scaled into `size` x `size` at (x, y) of a top-down BGRA DIB.
void BlitPicture(const std::shared_ptr<const std::vector<uint8_t>>& bytes, uint8_t* dib, UINT dibW, int x, int y, UINT size) {
    using Microsoft::WRL::ComPtr;
    if (!bytes || bytes->empty()) return;
    static thread_local bool comInit = false;
    if (!comInit) {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);  // RPC_E_CHANGED_MODE is fine too
        comInit = true;
    }
    ComPtr<IWICImagingFactory> wic;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic)))) return;
    ComPtr<IWICStream> stream;
    if (FAILED(wic->CreateStream(&stream))) return;
    if (FAILED(stream->InitializeFromMemory(const_cast<BYTE*>(bytes->data()), static_cast<DWORD>(bytes->size())))) return;
    ComPtr<IWICBitmapDecoder> dec;
    if (FAILED(wic->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &dec))) return;
    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(dec->GetFrame(0, &frame))) return;
    ComPtr<IWICBitmapScaler> scaler;
    if (FAILED(wic->CreateBitmapScaler(&scaler))) return;
    if (FAILED(scaler->Initialize(frame.Get(), size, size, WICBitmapInterpolationModeFant))) return;
    ComPtr<IWICFormatConverter> conv;
    if (FAILED(wic->CreateFormatConverter(&conv))) return;
    if (FAILED(conv->Initialize(scaler.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                                WICBitmapPaletteTypeCustom)))
        return;
    std::vector<uint8_t> px(static_cast<size_t>(size) * size * 4);
    if (FAILED(conv->CopyPixels(nullptr, size * 4, static_cast<UINT>(px.size()), px.data()))) return;
    // composite (premultiplied) over the DIB; corners rounded a little
    const float rad = size * 0.12f;
    for (UINT yy = 0; yy < size; ++yy) {
        for (UINT xx = 0; xx < size; ++xx) {
            float cx = xx + 0.5f, cy = yy + 0.5f, cov = 1.0f;
            float dx = cx < rad ? rad - cx : (cx > size - rad ? cx - (size - rad) : 0.0f);
            float dy = cy < rad ? rad - cy : (cy > size - rad ? cy - (size - rad) : 0.0f);
            if (dx > 0 && dy > 0) {
                float d = std::sqrt(dx * dx + dy * dy);
                cov = d >= rad ? 0.0f : (d > rad - 1.0f ? rad - d : 1.0f);
            }
            const uint8_t* s = &px[(static_cast<size_t>(yy) * size + xx) * 4];
            uint8_t* d = dib + (static_cast<size_t>(y + yy) * dibW + (x + xx)) * 4;
            const float a = s[3] / 255.0f * cov;
            for (int c = 0; c < 3; ++c) d[c] = static_cast<uint8_t>(s[c] * cov + d[c] * (1.0f - a));
        }
    }
}

std::vector<uint8_t> DrawNowBar(const Field& f, const NowInfo& n) {
    std::vector<uint8_t> out(static_cast<size_t>(f.w) * f.h * 4, 0);
    if (!n.visible) return out;  // radio off: fully transparent, nothing shows on the map
    HDC dc = CreateCompatibleDC(nullptr);
    if (!dc) return out;
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = static_cast<LONG>(f.w);
    bi.bmiHeader.biHeight = -static_cast<LONG>(f.h);
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
    RECT all{0, 0, static_cast<LONG>(f.w), static_cast<LONG>(f.h)};
    HBRUSH bg = CreateSolidBrush(f.bg);
    FillRect(dc, &all, bg);
    DeleteObject(bg);
    GdiFlush();

    const int pic = static_cast<int>(f.h) - 12;  // square picture on the left
    int textX = 14;
    if (n.picture) {
        BlitPicture(n.picture, static_cast<uint8_t*>(bits), f.w, 6, 6, static_cast<UINT>(pic));
        textX = pic + 22;
    }
    SetBkMode(dc, TRANSPARENT);
    HFONT big = CreateFontW(-40, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                            CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Segoe UI Semibold");
    HFONT smallFont = CreateFontW(-28, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                              CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    RECT r1{textX, 4, static_cast<LONG>(f.w) - 12, 52};
    RECT r2{textX, 52, static_cast<LONG>(f.w) - 12, 92};
    HGDIOBJ oldFont = SelectObject(dc, big);
    SetTextColor(dc, f.fg);
    DrawTextW(dc, n.title.c_str(), -1, &r1, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    SelectObject(dc, smallFont);
    SetTextColor(dc, RGB(0x8A, 0x93, 0xA3));
    DrawTextW(dc, n.sub.c_str(), -1, &r2, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    SelectObject(dc, oldFont);
    DeleteObject(big);
    DeleteObject(smallFont);
    GdiFlush();
    memcpy(out.data(), bits, out.size());
    // rounded card: alpha follows the corner shape (the UI has no own card behind the bar any more)
    const float rad = 17.0f;
    for (UINT yy = 0; yy < f.h; ++yy) {
        for (UINT xx = 0; xx < f.w; ++xx) {
            float cx = xx + 0.5f, cy = yy + 0.5f, cov = 1.0f;
            float dx = cx < rad ? rad - cx : (cx > f.w - rad ? cx - (f.w - rad) : 0.0f);
            float dy = cy < rad ? rad - cy : (cy > f.h - rad ? cy - (f.h - rad) : 0.0f);
            if (dx > 0 && dy > 0) {
                float d = std::sqrt(dx * dx + dy * dy);
                cov = d >= rad ? 0.0f : (d > rad - 1.0f ? rad - d : 1.0f);
            }
            out[(static_cast<size_t>(yy) * f.w + xx) * 4 + 3] = static_cast<uint8_t>(cov * 255.0f + 0.5f);
        }
    }
    SelectObject(dc, oldBmp);
    DeleteObject(bmp);
    DeleteDC(dc);
    return out;
}

// Speed limit sign: white disc, red ring, dark digits. No limit known = fully transparent (no empty sign on the map).
std::vector<uint8_t> DrawSign(const Field& f, const std::wstring& digits) {
    if (digits.empty()) return std::vector<uint8_t>(static_cast<size_t>(f.w) * f.h * 4, 0);
    const UINT innerW = f.w * 72 / 100;  // digits must fit inside the ring
    std::vector<uint8_t> txt = Draw(f, digits, L"", innerW);
    std::vector<uint8_t> out(static_cast<size_t>(f.w) * f.h * 4, 0);
    const float cx = f.w * 0.5f, cy = f.h * 0.5f, R = std::min(f.w, f.h) * 0.5f - 1.0f, ring = R * 0.80f;
    const int ox = (static_cast<int>(f.w) - static_cast<int>(innerW)) / 2;
    for (UINT y = 0; y < f.h; ++y) {
        for (UINT x = 0; x < f.w; ++x) {
            const float d = std::sqrt((x + 0.5f - cx) * (x + 0.5f - cx) + (y + 0.5f - cy) * (y + 0.5f - cy));
            const float cover = std::clamp(R - d + 0.5f, 0.0f, 1.0f);       // outer edge
            const float white = std::clamp(ring - d + 0.5f, 0.0f, 1.0f);    // inside the ring
            float r = 0xC2 + (0xDC - 0xC2) * white, g = 0x41 + (0xDE - 0x41) * white, b = 0x3A + (0xE2 - 0x3A) * white;
            const int tx = static_cast<int>(x) - ox;
            if (tx >= 0 && tx < static_cast<int>(innerW)) {
                const float a = txt[(static_cast<size_t>(y) * innerW + tx) * 4 + 3] / 255.0f * white;
                r += (0x10 - r) * a; g += (0x12 - g) * a; b += (0x15 - b) * a;
            }
            uint8_t* o = &out[(static_cast<size_t>(y) * f.w + x) * 4];
            o[0] = static_cast<uint8_t>(b); o[1] = static_cast<uint8_t>(g); o[2] = static_cast<uint8_t>(r);
            o[3] = static_cast<uint8_t>(cover * 255.0f + 0.5f);
        }
    }
    return out;
}

Field* FieldForDesc(const D3D11_TEXTURE2D_DESC* d) {
    for (auto& f : g_fields) {
        if (d->Width == f.phW && d->Height == f.phH) return &f;
    }
    return nullptr;
}

}  // namespace

void EtaSetGameTime(uint32_t minutes) {
    g_gameMinutes = minutes;
    g_haveGameTime = true;
}
void EtaSetNavigationTime(float seconds) { g_navSeconds = seconds; }
std::atomic<unsigned long long> g_electricOffTick{0};
void EtaSetElectric(bool on) {
    if (!on) g_electricOffTick = GetTickCount64();
    const int prev = g_electric.exchange(on ? 1 : 0);
    if (on && prev != 1) g_electricOnEvent = true;  // also the first value after loading: the bus comes alive
}
void EtaSetNavigationDistance(float meters) { g_navMeters = meters; }
void EtaSetPlate(const std::string& plate, const std::string& countryId) {
    std::lock_guard<std::mutex> lock(g_plateMx);
    if (plate == g_plateRaw && countryId == g_plateCountry) return;
    g_plateRaw = plate;
    g_plateCountry = countryId;
    Logf("PLATE telemetry: \"%s\" country \"%s\"", plate.c_str(), countryId.c_str());
}
void EtaSetSpeedLimit(float metersPerSecond) { g_speedLimit = metersPerSecond; }

TripInfo EtaGetTrip() {
    TripInfo t;
    t.haveTime = g_haveGameTime.load();
    t.gameMinutes = g_gameMinutes.load();
    t.navSeconds = g_navSeconds.load();
    t.navMeters = g_navMeters.load();
    t.speedLimit = g_speedLimit.load();
    return t;
}

void EtaArrivalText(std::wstring& main, std::wstring& suffix) {
    main.clear();
    suffix.clear();
    EtaText(main, suffix);
}

unsigned long long EtaElectricOffTick() { return g_electricOffTick.load(); }

bool EtaIsPlaceholder(const D3D11_TEXTURE2D_DESC* d) {
    return FieldForDesc(d) && d->ArraySize == 1 && d->MipLevels == 1 && d->SampleDesc.Count == 1 &&
           d->Usage == D3D11_USAGE_DEFAULT && d->BindFlags == D3D11_BIND_SHADER_RESOURCE && d->CPUAccessFlags == 0 &&
           d->MiscFlags == 0 &&
           (d->Format == DXGI_FORMAT_BC3_UNORM || d->Format == DXGI_FORMAT_BC3_UNORM_SRGB ||
            d->Format == DXGI_FORMAT_BC3_TYPELESS);
}

void EtaOnPlaceholderCreated(ID3D11Device* dev, const D3D11_TEXTURE2D_DESC& d, ID3D11Texture2D* placeholder,
                             CreateTexFn createTex) {
    Field* f = FieldForDesc(&d);
    if (!f) return;
    D3D11_TEXTURE2D_DESC nd{};
    nd.Width = f->w;
    nd.Height = f->h;
    nd.MipLevels = 1;
    nd.ArraySize = 1;
    nd.Format = DXGI_FORMAT_B8G8R8A8_TYPELESS;
    nd.SampleDesc.Count = 1;
    nd.Usage = D3D11_USAGE_DEFAULT;
    nd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    ID3D11Texture2D* tex = nullptr;
    HRESULT hr = createTex(dev, &nd, nullptr, &tex);
    if (FAILED(hr) || !tex) {
        Logf("%s could not create our texture: 0x%08lx", f->name, static_cast<unsigned long>(hr));
        return;
    }
    std::lock_guard<std::mutex> lock(g_mx);
    if (f->srv) {
        f->srvAtomic = nullptr;
        f->srv->Release();
        f->srv = nullptr;
    }
    if (f->tex) f->tex->Release();
    if (f->placeholder) f->placeholder->Release();
    f->tex = tex;
    f->placeholder = placeholder;
    f->placeholder->AddRef();
    f->srgb = d.Format == DXGI_FORMAT_BC3_UNORM_SRGB;
    f->shown.clear();
    f->shownIn = nullptr;
    Logf("%s placeholder found (%ux%u fmt=%u), our %ux%u texture created", f->name, d.Width, d.Height,
         static_cast<unsigned>(d.Format), f->w, f->h);
}

bool EtaIsPlaceholderResource(ID3D11Resource* res) {
    if (!res) return false;
    std::lock_guard<std::mutex> lock(g_mx);
    for (auto& f : g_fields) {
        if (res == f.placeholder) return true;
    }
    return false;
}

void EtaOnPlaceholderViewCreated(ID3D11Device* dev, ID3D11Resource* res, ID3D11ShaderResourceView* gameView,
                                 const D3D11_SHADER_RESOURCE_VIEW_DESC* desc, CreateSrvFn createSrv) {
    std::lock_guard<std::mutex> lock(g_mx);
    Field* f = nullptr;
    for (auto& c : g_fields) {
        if (res == c.placeholder) f = &c;
    }
    if (!f || !f->tex) return;
    const bool srgb = desc ? desc->Format == DXGI_FORMAT_BC3_UNORM_SRGB : f->srgb;
    D3D11_SHADER_RESOURCE_VIEW_DESC vd{};
    vd.Format = srgb ? DXGI_FORMAT_B8G8R8A8_UNORM_SRGB : DXGI_FORMAT_B8G8R8A8_UNORM;
    vd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    vd.Texture2D.MipLevels = 1;
    ID3D11ShaderResourceView* ours = nullptr;
    HRESULT hr = createSrv(dev, f->tex, &vd, &ours);
    if (FAILED(hr) || !ours) {
        Logf("%s could not create our view: 0x%08lx", f->name, static_cast<unsigned long>(hr));
        return;
    }
    ID3D11ShaderResourceView* old = f->srv;
    f->srv = ours;
    f->srvAtomic = ours;
    if (old) old->Release();
    for (auto& v : f->views) {
        if (v.load() == gameView) return;
    }
    for (auto& v : f->views) {
        ID3D11ShaderResourceView* expected = nullptr;
        if (v.compare_exchange_strong(expected, gameView)) break;
    }
    Logf("%s game view %p will show our text (%s)", f->name, static_cast<void*>(gameView), srgb ? "sRGB" : "linear");
}

void EtaForgetView(ID3D11ShaderResourceView* s) {
    for (auto& f : g_fields) {
        for (auto& v : f.views) {
            ID3D11ShaderResourceView* expected = s;
            v.compare_exchange_strong(expected, nullptr);
        }
    }
}

ID3D11ShaderResourceView* EtaSwapFor(ID3D11ShaderResourceView* s) {
    if (!s) return nullptr;
    for (auto& f : g_fields) {
        ID3D11ShaderResourceView* ours = f.srvAtomic.load(std::memory_order_relaxed);
        if (!ours) continue;
        for (auto& v : f.views) {
            if (v.load(std::memory_order_relaxed) == s) {
                f.lastBound.store(GetTickCount64(), std::memory_order_relaxed);
                return ours;
            }
        }
    }
    return nullptr;
}

void EtaStop() {
    {
        std::lock_guard<std::mutex> lock(g_jobMx);
        g_drawStop = true;
        g_jobs.clear();
    }
    g_jobCv.notify_all();
    if (g_drawThread.joinable()) g_drawThread.join();
}

void EtaUpload() {
    const ULONGLONG now = GetTickCount64();
    PaxTick(g_gameMinutes.load(), g_navSeconds.load(), g_navMeters.load());
    {
        const ULONGLONG bootSeen = g_fields[kBootField].lastBound.load(std::memory_order_relaxed);
        // bus switched off: overlay black (ready), bus switched on: animation, whatever page is showing.
        // Coming back from the black OFF page does not play it (only switching the vehicle on does).
        const int electric = g_electric.load();
        if (electric == 0) {
            g_bootArmed = true;
            g_electricOnAt = 0;
        }
        if (g_electricOnEvent.exchange(false)) {
            // switched on (or loaded with the electrics on): play as soon as the screen is actually drawn - not during
            // the game's loading screen or while the player looks from outside
            g_bootArmed = true;
            g_electricOnAt = now;
        }
        if (g_electricOnAt != 0) {
            if (bootSeen > g_electricOnAt) {
                g_bootArmed = false;
                g_electricOnAt = 0;
                g_bootStart = now;
                Logf("BOOT bus switched on: start-up animation");
            } else if (now - g_electricOnAt > 15000) {
                g_bootArmed = false;  // screen not drawn (other vehicle, outside view): give up, no black overlay left
                g_electricOnAt = 0;
            }
        }
    }
    const float bootT = g_bootStart != 0 ? (now - g_bootStart) / 1000.0f : -1.0f;
    const bool bootRunning = bootT >= 0.0f && bootT < kBootSeconds;
    for (auto& f : g_fields) {
        ID3D11Texture2D* tex = nullptr;
        {
            std::lock_guard<std::mutex> lock(g_mx);
            tex = f.tex;
            if (tex) tex->AddRef();
        }
        if (!tex) continue;
        std::wstring main, suffix, key;
        NowInfo now;
        PaxView pax;
        std::wstring plateText;
        std::string plateCountry;
        if (f.kind == 0) EtaText(main, suffix);
        else if (f.kind == 1) LimitText(main);
        if (f.kind == 2) {
            now = GetNow();
            key = now.key;
        } else if (f.kind == 3) {
            pax = PaxGetView(g_gameMinutes.load(), g_haveGameTime.load(), g_navSeconds.load(), g_navMeters.load());
            key = pax.key;
        } else if (f.kind == 4) {
            // while the screen is off the overlay is already opaque black, so the first map frame after switching on
            // shows black (not a flash of the map) and the animation starts from there
            key = bootRunning ? L"run|" + std::to_wstring(static_cast<int>(bootT * 30.0f)) : g_bootArmed ? L"armed" : L"idle";
        } else if (f.kind == 5) {
            key = L"off";
        } else if (f.kind == 6) {
            std::lock_guard<std::mutex> lock(g_plateMx);
            plateText = PlateText(g_plateRaw);
            plateCountry = g_plateCountry;
            key = L"plate|" + plateText + L"|" + std::wstring(plateCountry.begin(), plateCountry.end());
        } else {
            key = main + L"|" + suffix;
        }
        const size_t fi = static_cast<size_t>(&f - g_fields);
        // a picture the drawing thread finished: upload it (cheap), never draw on this thread (the game's render thread)
        DrawDone done;
        {
            std::lock_guard<std::mutex> lock(g_jobMx);
            if (g_done[fi].has) {
                done = std::move(g_done[fi]);
                g_done[fi] = DrawDone{};
            }
        }
        if (done.has && done.tex == tex) {
            ID3D11Device* dev = nullptr;
            tex->GetDevice(&dev);
            if (dev) {
                ID3D11DeviceContext* ctx = nullptr;
                dev->GetImmediateContext(&ctx);
                if (ctx) {
                    ctx->UpdateSubresource(tex, 0, nullptr, done.px.data(), f.w * 4, 0);
                    ctx->Release();
                    f.shown = done.key;
                    f.shownIn = tex;
                }
                dev->Release();
            }
        }
        if (key != f.shown || tex != f.shownIn) {
            Field* fp = &f;
            std::function<std::vector<uint8_t>()> draw;
            const uint32_t gm = g_gameMinutes.load();
            const bool ht = g_haveGameTime.load();
            const float ns = g_navSeconds.load(), nm = g_navMeters.load();
            const float bt = bootRunning ? bootT : g_bootArmed ? 0.0f : -1.0f;
            switch (f.kind) {
                case 6: draw = [fp, plateText, plateCountry] { return PlateDraw(fp->w, fp->h, plateText, plateCountry); }; break;
                case 5: draw = [fp] { return std::vector<uint8_t>(static_cast<size_t>(fp->w) * fp->h * 4, 0); }; break;
                case 4: draw = [fp, bt] { return BootDraw(fp->w, fp->h, bt); }; break;
                case 3: draw = [fp, pax, gm, ht, ns, nm] { return PaxDraw(fp->w, fp->h, pax, gm, ht, ns, nm); }; break;
                case 2: draw = [fp, now] { return DrawNowBar(*fp, now); }; break;
                case 1: draw = [fp, main] { return DrawSign(*fp, main); }; break;
                default: draw = [fp, main, suffix] { return Draw(*fp, main, suffix); }; break;
            }
            DrawQueue(fi, key, tex, std::move(draw));
        }
        tex->Release();
    }
    if (now - g_lastLog > 30000) {
        g_lastLog = now;
        Logf("TEXT game.time=%u min, navigation.time=%.0f s, distance=%.0f m, speed_limit=%.2f m/s -> eta '%ls' limit '%ls'",
             g_gameMinutes.load(), g_navSeconds.load(), g_navMeters.load(), g_speedLimit.load(), g_fields[0].shown.c_str(),
             g_fields[1].shown.c_str());
    }
}

}  // namespace ep
