#include "d3d11_hooks.h"

#include "engine_hooks.h"
#include "eta_screen.h"
#include "log.h"
#include "renderer.h"
#include "screen_assets.h"
#include "screen_fit.h"
#include "settings.h"
#include "touch.h"

#include <MinHook.h>
#include <d3d11.h>
#include <dxgi1_2.h>

#include <algorithm>
#include <bitset>
#include <cmath>
#include <cwchar>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace ep {
namespace {

using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using Present1Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
using PSSetSRVFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, ID3D11ShaderResourceView* const*);
using CreateSRVFn = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, ID3D11Resource*,
                                                const D3D11_SHADER_RESOURCE_VIEW_DESC*,
                                                ID3D11ShaderResourceView**);
using CreateTex2DFn = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, const D3D11_TEXTURE2D_DESC*,
                                                  const D3D11_SUBRESOURCE_DATA*, ID3D11Texture2D**);

PresentFn g_present;
Present1Fn g_present1;
PSSetSRVFn g_psSetSrv;
using OMSetRTFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, ID3D11RenderTargetView* const*, ID3D11DepthStencilView*);
OMSetRTFn g_omSetRt;
CreateSRVFn g_createSrv;
CreateTex2DFn g_createTex2D;
std::atomic<bool> g_off{false};

// ---------------------------------------------------------------------------------------------
// Cabin screen. The game loads our 64x2048 BC3 placeholder (see screen_assets.h) in place of its GPS
// texture and fills it itself. We leave that texture completely alone and only swap the view at bind
// time: whenever the game binds a shader resource view of the placeholder, the pixel shader gets a view
// of our own BGRA texture instead. The renderer's frames are copied into that texture from Present.
// ---------------------------------------------------------------------------------------------
std::mutex g_screenMx;
ID3D11Texture2D* g_placeholder = nullptr;  // the game's texture (we keep one reference to compare pointers safely)
ID3D11Texture2D* g_ourTex = nullptr;       // our private BGRA texture
ID3D11ShaderResourceView* g_ourSrv = nullptr;
std::atomic<ID3D11ShaderResourceView*> g_ourSrvAtomic{nullptr};
// last time the game bound the MUSIK placeholder (= the music page is being drawn); uploads pause while it is hidden
std::atomic<ULONGLONG> g_screenBoundTick{0};
bool g_placeholderSrgb = false;

constexpr int kMaxSwap = 8;
std::atomic<ID3D11ShaderResourceView*> g_swap[kMaxSwap];  // the game's views of the placeholder

uint64_t g_screenUploaded = 0;
ID3D11Texture2D* g_screenUploadedTo = nullptr;
bool g_replaceTexture = true;
bool g_uploadFrames = true;
uint64_t g_uploadCount = 0;
ULONGLONG g_lastHeartbeat = 0;
bool g_removedLogged = false;

const char* RemovedReasonName(HRESULT hr) {
    switch (hr) {
        case S_OK: return "S_OK";
        case DXGI_ERROR_DEVICE_HUNG: return "DEVICE_HUNG";
        case DXGI_ERROR_DEVICE_REMOVED: return "DEVICE_REMOVED";
        case DXGI_ERROR_DEVICE_RESET: return "DEVICE_RESET";
        case DXGI_ERROR_DRIVER_INTERNAL_ERROR: return "DRIVER_INTERNAL_ERROR";
        case DXGI_ERROR_INVALID_CALL: return "INVALID_CALL";
        default: return "other";
    }
}

void LogDesc(const char* what, const D3D11_TEXTURE2D_DESC& d, bool hasInit) {
    Logf("SCREEN %s: %ux%u fmt=%u mips=%u array=%u samples=%u usage=%u bind=0x%x cpu=0x%x misc=0x%x init=%d", what, d.Width,
         d.Height, static_cast<unsigned>(d.Format), d.MipLevels, d.ArraySize, d.SampleDesc.Count,
         static_cast<unsigned>(d.Usage), d.BindFlags, d.CPUAccessFlags, d.MiscFlags, hasInit ? 1 : 0);
}

// CoachNowPlaying: the placeholder is the mod's own file /material/ui/cpc_np/np.dds, a BC3 texture with one mip level
// and the deliberately odd size 60x1996 that no game texture uses. The game creates it without data (it streams the
// pixels in later), so it can only be recognised by its description: every field has to match.
bool IsPlaceholder(const D3D11_TEXTURE2D_DESC* d) {
    return d->Width == kPlaceholderWidth && d->Height == kPlaceholderHeight && d->ArraySize == 1 && d->MipLevels == 1 &&
           d->SampleDesc.Count == 1 && d->Usage == D3D11_USAGE_DEFAULT && d->BindFlags == D3D11_BIND_SHADER_RESOURCE &&
           d->CPUAccessFlags == 0 && d->MiscFlags == 0 &&
           (d->Format == DXGI_FORMAT_BC3_UNORM || d->Format == DXGI_FORMAT_BC3_UNORM_SRGB ||
            d->Format == DXGI_FORMAT_BC3_TYPELESS);
}

bool IsSrgbFormat(DXGI_FORMAT f) {
    return f == DXGI_FORMAT_BC3_UNORM_SRGB || f == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || f == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
}

// GPS takeover (no mod, e.g. TruckersMP): the game draws its cab screen into a 1024x1024 RGBA8 render target that is
// also sampled (bind = shader resource | render target). Found with the texture finder on 2026-11-25: on a coach only
// 2 of ~1750 bound textures look like this, so the user picks the right one with Ctrl+Shift+F10.
// Idea credit: PrismTextureStreamer by Baldywaldy09 showed that a plugin can take over a cab screen on TruckersMP
// (https://github.com/Baldywaldy09/PrismTextureStreamer). No code taken from it; this reuses the placeholder swap.
bool IsGpsRenderTarget(const D3D11_TEXTURE2D_DESC* d) {
    return d->Width == 1024 && d->Height == 1024 && d->ArraySize == 1 && d->MipLevels == 1 && d->SampleDesc.Count == 1 &&
           d->Usage == D3D11_USAGE_DEFAULT && d->BindFlags == (D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET) &&
           d->CPUAccessFlags == 0 && d->MiscFlags == 0 &&
           (d->Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || d->Format == DXGI_FORMAT_R8G8B8A8_UNORM ||
            d->Format == DXGI_FORMAT_R8G8B8A8_TYPELESS);
}

bool IsSwapSrv(ID3D11ShaderResourceView* s) {
    for (auto& slot : g_swap) {
        if (slot.load(std::memory_order_relaxed) == s) return true;
    }
    return false;
}

void AddSwapSrv(ID3D11ShaderResourceView* s) {
    for (auto& slot : g_swap) {
        if (slot.load() == s) return;
    }
    for (auto& slot : g_swap) {
        ID3D11ShaderResourceView* expected = nullptr;
        if (slot.compare_exchange_strong(expected, s)) return;
    }
    Logf("SCREEN too many views of the placeholder, ignoring one");
}

void RemoveSwapSrv(ID3D11ShaderResourceView* s) {
    for (auto& slot : g_swap) {
        ID3D11ShaderResourceView* expected = s;
        slot.compare_exchange_strong(expected, nullptr);
    }
}

// The game created its placeholder texture: make our private texture next to it.
void OnPlaceholderCreated(ID3D11Device* dev, const D3D11_TEXTURE2D_DESC& d, ID3D11Texture2D* placeholder, bool hasInit) {
    LogDesc("placeholder created by the game", d, hasInit);

    D3D11_TEXTURE2D_DESC nd{};
    nd.Width = kScreenWidth;
    nd.Height = kScreenHeight;
    nd.MipLevels = 1;
    nd.ArraySize = 1;
    nd.Format = DXGI_FORMAT_B8G8R8A8_TYPELESS;
    nd.SampleDesc.Count = 1;
    nd.Usage = D3D11_USAGE_DEFAULT;
    nd.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    ID3D11Texture2D* tex = nullptr;
    HRESULT hr = g_createTex2D(dev, &nd, nullptr, &tex);  // the original function: do not re-enter our hook
    if (FAILED(hr) || !tex) {
        Logf("SCREEN could not create our texture: 0x%08lx", static_cast<unsigned long>(hr));
        return;
    }
    std::lock_guard<std::mutex> lock(g_screenMx);
    if (g_ourSrv) {
        g_ourSrvAtomic = nullptr;
        g_ourSrv->Release();
        g_ourSrv = nullptr;
    }
    if (g_ourTex) g_ourTex->Release();
    if (g_placeholder) g_placeholder->Release();
    g_ourTex = tex;
    g_placeholder = placeholder;
    g_placeholder->AddRef();
    g_placeholderSrgb = IsSrgbFormat(d.Format);
    Logf("SCREEN our %ux%u texture created next to the game's placeholder %p (thread %lu)", nd.Width, nd.Height,
         static_cast<void*>(placeholder), GetCurrentThreadId());
    FitReset("new screen texture");  // a new truck / reload: measure the display again
    TouchResetScan();
}

// The game made a view of the placeholder: make the matching view of our texture.
void OnPlaceholderViewCreated(ID3D11Device* dev, ID3D11ShaderResourceView* gameView,
                              const D3D11_SHADER_RESOURCE_VIEW_DESC* desc) {
    std::lock_guard<std::mutex> lock(g_screenMx);
    if (!g_ourTex) return;
    bool srgb = desc && desc->Format != DXGI_FORMAT_UNKNOWN ? IsSrgbFormat(desc->Format) : g_placeholderSrgb;

    D3D11_SHADER_RESOURCE_VIEW_DESC vd{};
    vd.Format = srgb ? DXGI_FORMAT_B8G8R8A8_UNORM_SRGB : DXGI_FORMAT_B8G8R8A8_UNORM;
    vd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    vd.Texture2D.MipLevels = 1;
    ID3D11ShaderResourceView* ours = nullptr;
    HRESULT hr = g_createSrv(dev, g_ourTex, &vd, &ours);
    if (FAILED(hr) || !ours) {
        Logf("SCREEN could not create our view: 0x%08lx", static_cast<unsigned long>(hr));
        return;
    }
    ID3D11ShaderResourceView* old = g_ourSrv;
    g_ourSrv = ours;
    g_ourSrvAtomic = ours;
    TouchSetScreenView(ours);
    if (old) old->Release();  // leaked bind slots keep their own reference
    AddSwapSrv(gameView);
    Logf("SCREEN game view %p of the placeholder will show our texture (%s)", static_cast<void*>(gameView),
         srgb ? "sRGB" : "linear");
}

// ---- GPS takeover ---------------------------------------------------------------------------------------------------
// The game makes these render targets in pairs (cab screen first, then a second one of the same size) and throws them
// away and makes new ones whenever the bus is switched off and on again. So the picture follows the screen itself:
// when the texture we show is released, the first one the game makes next is the new screen.
struct GpsCand {
    ID3D11Texture2D* tex;  // one reference held by us
    D3D11_TEXTURE2D_DESC desc;
    std::vector<ID3D11ShaderResourceView*> views;  // the game's views of it (keys only, no reference)
    unsigned seq;  // creation number: the game makes them in pairs, odd = second of a pair (the flipped copy)
};
unsigned g_gpsSeq = 0;
std::vector<GpsCand> g_gps;            // under g_screenMx, in creation order
std::atomic<bool> g_drawsReset{false};
std::atomic<bool> g_takeoverFlip{false};  // shown texture is the second of its pair (a flipped copy of the first)  // a new screen texture: the displays drawn from it are found again
ID3D11Texture2D* g_gpsShown = nullptr;  // candidate showing our picture, nullptr = none
bool g_gpsOff = false;                 // switched off with Ctrl+Shift+F10: the stock screen stays until switched on
int g_gpsSkip = 0;                     // [screen] gps_takeover_pick: skip this many first candidates at the start
int g_gpsLogged = 0;

// Only we still hold it (our candidate reference, plus the placeholder reference if it is shown): the game let go.
bool OnlyWeHold(ID3D11Texture2D* t) {
    const ULONG ours = 1 + (t == g_placeholder ? 1 : 0);
    t->AddRef();
    return t->Release() <= ours;
}

void ClearSwapSrvs() {
    for (auto& slot : g_swap) slot.store(nullptr);
}

// Show our picture on `tex` (a known candidate) or on nothing (nullptr). Called without g_screenMx held.
void GpsSelect(ID3D11Texture2D* tex, const char* why) {
    GpsCand cand{};
    size_t index = 0, count = 0;
    {
        std::lock_guard<std::mutex> lock(g_screenMx);
        ClearSwapSrvs();
        g_gpsShown = nullptr;
        g_drawsReset.store(true);
        count = g_gps.size();
        for (size_t i = 0; tex && i < g_gps.size(); ++i) {
            if (g_gps[i].tex == tex) {
                cand = g_gps[i];
                index = i;
            }
        }
        if (!cand.tex) {
            Logf("TAKEOVER off (%s): the stock cab screen shows (%zu candidates known)", why, count);
            return;
        }
        g_gpsShown = cand.tex;
        g_takeoverFlip.store(cand.seq % 2 == 1);
        cand.tex->AddRef();
    }
    ID3D11Device* dev = nullptr;
    cand.tex->GetDevice(&dev);
    if (dev) {
        OnPlaceholderCreated(dev, cand.desc, cand.tex, false);  // our BGRA texture + this texture as "placeholder"
        for (ID3D11ShaderResourceView* v : cand.views) OnPlaceholderViewCreated(dev, v, nullptr);
        Logf("TAKEOVER CarPlay on candidate #%zu of %zu = %p (%s, %zu views%s)", index, count,
             static_cast<void*>(cand.tex), why, cand.views.size(), cand.seq % 2 == 1 ? ", second of a pair: drawn flipped" : "");
        dev->Release();
    }
    cand.tex->Release();
}

void OnGpsTargetCreated(const D3D11_TEXTURE2D_DESC& d, ID3D11Texture2D* tex) {
    bool select = false, lostShown = false;
    size_t index = 0;
    {
        std::lock_guard<std::mutex> lock(g_screenMx);
        for (size_t i = 0; i < g_gps.size();) {  // forget screens the game has let go of
            if (OnlyWeHold(g_gps[i].tex)) {
                if (g_gps[i].tex == g_gpsShown) {
                    g_gpsShown = nullptr;
                    ClearSwapSrvs();
                    lostShown = true;
                }
                g_gps[i].tex->Release();
                g_gps.erase(g_gps.begin() + static_cast<std::ptrdiff_t>(i));
            } else {
                ++i;
            }
        }
        const unsigned seq = g_gpsSeq++;  // counted even when not kept, so the pair halves stay right
        if (g_gps.size() >= 8) return;
        tex->AddRef();
        g_gps.push_back({tex, d, {}, seq});
        if (lostShown) g_gpsSkip = std::max(0, GetSettings().gpsTakeoverPick);  // a new pair: same pick as at the start
        index = g_gps.size() - 1;
        if (!g_gpsOff && !g_gpsShown) {
            if (g_gpsSkip > 0) --g_gpsSkip;
            else select = true;
        }
    }
    if (lostShown) Logf("TAKEOVER the shown screen was released by the game (bus switched off/on, reload)");
    if (g_gpsLogged < 16 || select) {
        ++g_gpsLogged;
        LogDesc("TAKEOVER cab screen candidate", d, false);
        Logf("TAKEOVER candidate #%zu = %p%s", index, static_cast<void*>(tex), select ? " (shown)" : "");
    }
    if (select) GpsSelect(tex, lostShown ? "new screen after the old one was released" : "first screen");
}

// A new view may reuse the address of a released one: drop it from every candidate's list.
void GpsForgetView(ID3D11ShaderResourceView* view) {
    std::lock_guard<std::mutex> lock(g_screenMx);
    for (GpsCand& c : g_gps) c.views.erase(std::remove(c.views.begin(), c.views.end(), view), c.views.end());
}

// A view of a known candidate: remember it, and swap it at once if that candidate is the one shown.
void OnGpsViewCreated(ID3D11Device* dev, ID3D11Resource* res, ID3D11ShaderResourceView* view,
                      const D3D11_SHADER_RESOURCE_VIEW_DESC* desc) {
    bool shown = false;
    {
        std::lock_guard<std::mutex> lock(g_screenMx);
        for (GpsCand& c : g_gps) {
            if (c.tex != res) continue;
            c.views.push_back(view);
            shown = c.tex == g_gpsShown && g_placeholder == c.tex;
            break;
        }
    }
    if (shown) OnPlaceholderViewCreated(dev, view, desc);
}

// Ctrl+Shift+F10: next candidate -> ... -> off (stock screen) -> first candidate
void GpsCycle() {
    ID3D11Texture2D* next = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_screenMx);
        size_t at = g_gps.size();
        for (size_t i = 0; i < g_gps.size(); ++i) {
            if (g_gps[i].tex == g_gpsShown) at = i;
        }
        if (!g_gpsShown) next = g_gps.empty() ? nullptr : g_gps[0].tex;
        else if (at + 1 < g_gps.size()) next = g_gps[at + 1].tex;
        g_gpsOff = next == nullptr;
    }
    GpsSelect(next, "Ctrl+Shift+F10");
}

// ---- GPS takeover: which displays show the picture ------------------------------------------------------------------
// Some vehicles draw two displays from the same cab screen texture: on the Volvo 9700 the centre screen AND the middle
// of the instrument cluster (where the game shows the map). Swapping the texture puts CarPlay on both. Each display is
// drawn on its own: a small quad from a vertex buffer the UI refills all the time (so its offsets change whenever the UI
// does), into the display's own render target. So a display is recognised by that render target's size and the
// viewport in it, which stay the same for a vehicle model. For a hidden display the draw hook binds the game's own view again, draws, and rebinds ours.
// [screen] gps_takeover_hide lists the hidden meshes; Ctrl+Shift+F11 cycles: all displays -> only #0 -> only #1 -> ...
// Everything here runs on the render thread (immediate context: binds, draws and Present).
struct ScreenDraw {  // where a display is drawn: render target size, viewport, clip rectangle, order in the frame
    int rtW, rtH, x, y, w, h;
    int sx, sy, sw, sh;  // scissor rectangle (all 0 = none set)
    int ord;             // n-th draw of the screen texture in this frame; -1 unless the order is needed to tell them apart
    int px0, py0, px1, py1;  // the quad's corners on the dashboard target (pixels, or -1..1 x 1000); all 0 = unknown
    bool operator==(const ScreenDraw& o) const {
        return rtW == o.rtW && rtH == o.rtH && x == o.x && y == o.y && w == o.w && h == o.h && sx == o.sx &&
               sy == o.sy && sw == o.sw && sh == o.sh && ord == o.ord && px0 == o.px0 && py0 == o.py0 && px1 == o.px1 &&
               py1 == o.py1;
    }
};
// On the Volvo 9700 both displays are drawn into the same 1024x512 target with the same viewport. If everything above
// but the order is the same while two or more draws per frame show the screen, the order in the frame is used too.
bool g_useOrder = false;
int g_frameDraws = 0;       // screen draws so far in this frame
int g_framesWithMany = 0;   // frames that drew the screen at least twice
constexpr int kMaxDraws = 8;
std::vector<ScreenDraw> g_draws;    // displays seen drawing the current screen texture, in first-seen order
ULONGLONG g_drawTick[kMaxDraws] = {};  // last time each display was drawn with the screen texture
std::atomic<ULONGLONG> g_rtTick{0};    // last time the game rendered into the shown screen texture
std::vector<ScreenDraw> g_hidden;   // from [screen] gps_takeover_hide (may include meshes of other vehicles)
ID3D11DeviceContext* g_screenCtx = nullptr;   // the immediate context that binds the screen (no reference)
std::bitset<128> g_screenSlots;                // PS slots where we put our view instead of the game's
ID3D11ShaderResourceView* g_screenOrig[128];   // the game's view in those slots (one of g_swap, no reference)

void LoadHidden() {
    g_hidden.clear();
    const wchar_t* p = GetSettings().gpsTakeoverHide.c_str();
    while (*p) {
        ScreenDraw d{};
        int used = 0;
        if (swscanf(p, L" %dx%d@%d,%d,%d,%d/s%d,%d,%d,%d/o%d/p%d,%d,%d,%d%n", &d.rtW, &d.rtH, &d.x, &d.y, &d.w, &d.h, &d.sx,
                    &d.sy, &d.sw, &d.sh, &d.ord, &d.px0, &d.py0, &d.px1, &d.py1, &used) == 15 && used > 0) {
            g_hidden.push_back(d);
            if (d.ord >= 0) g_useOrder = true;
            p += used;
        } else {
            ++p;
        }
        while (*p == L',' || *p == L' ') ++p;
    }
}

bool IsHidden(const ScreenDraw& d) { return std::find(g_hidden.begin(), g_hidden.end(), d) != g_hidden.end(); }

void SaveHidden() {
    std::wstring v;
    for (const ScreenDraw& d : g_hidden) {
        wchar_t one[128];
        swprintf(one, 128, L"%s%dx%d@%d,%d,%d,%d/s%d,%d,%d,%d/o%d/p%d,%d,%d,%d", v.empty() ? L"" : L",", d.rtW, d.rtH, d.x,
                 d.y, d.w, d.h, d.sx, d.sy, d.sw, d.sh, d.ord, d.px0, d.py0, d.px1, d.py1);
        v += one;
    }
    WritePrivateProfileStringW(L"screen", L"gps_takeover_hide", v.c_str(), GetSettings().ini.c_str());
}

// Called from the screen bind (HookPSSetSRV) on the immediate context: remember where our view went.
void NoteScreenSlots(ID3D11DeviceContext* ctx, UINT start, UINT n, ID3D11ShaderResourceView* const* game, const bool* isScreen) {
    if (g_screenCtx != ctx) {
        if (ctx->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE) return;
        g_screenCtx = ctx;
        g_screenSlots.reset();
    }
    for (UINT i = 0; i < n && start + i < 128; ++i) {
        g_screenSlots.set(start + i, isScreen[i]);
        if (isScreen[i]) g_screenOrig[start + i] = game[i];
    }
}

// ---- where each display quad sits ------------------------------------------------------------------------------------
// Once the fit has worked out the quad's vertex layout, the corners of every screen draw are copied into small staging
// buffers and read kRing frames later (never waiting for the GPU). Draw number k of a frame is then known to be the quad
// at that position, so a display keeps its identity whatever else is shown (cluster mode, infotainment page).
constexpr int kRing = 4, kOrds = 4;
ID3D11Buffer* g_posStage[kRing][kOrds] = {};
bool g_posPending[kRing][kOrds] = {};
int g_posFrame = 0;
struct OrdPos {
    bool ok = false;
    int x0, y0, x1, y1;
};
OrdPos g_ordPos[kOrds];
ID3D11Buffer* g_quadIb = nullptr;  // the index buffer + start the quads come from, and the first corner's index there
UINT g_quadIbStart = ~0u;
uint32_t g_quadMinIndex = 0;
bool g_quadIbOk = false;
bool g_posResolved = false;     // the current draw's display is known by its position
bool g_fitResetDone = false;    // the fit was restarted once the displays could be told apart

void CopyQuadCorners(ID3D11DeviceContext* ctx, int ord, UINT start, INT base, const QuadLayout& ql) {
    if (ord >= kOrds) return;
    ID3D11Buffer* ib = nullptr;
    DXGI_FORMAT ibFmt = DXGI_FORMAT_UNKNOWN;
    UINT ibOff = 0;
    ctx->IAGetIndexBuffer(&ib, &ibFmt, &ibOff);
    if (ib && (ib != g_quadIb || start != g_quadIbStart)) {  // index buffer of the quads: read once (a static pattern)
        g_quadIb = ib;
        g_quadIbStart = start;
        g_quadIbOk = false;
        const UINT isz = ibFmt == DXGI_FORMAT_R16_UINT ? 2 : ibFmt == DXGI_FORMAT_R32_UINT ? 4 : 0;
        std::vector<uint8_t> bytes;
        if (isz && FitReadBuffer(ctx, ib, ibOff + start * isz, 6 * isz, bytes)) {
            uint32_t mn = ~0u;
            for (int i = 0; i < 6; ++i) {
                uint32_t v = 0;
                memcpy(&v, &bytes[static_cast<size_t>(i) * isz], isz);
                mn = std::min(mn, v);
            }
            g_quadMinIndex = mn;
            g_quadIbOk = true;
        }
    }
    if (ib) ib->Release();
    if (!g_quadIbOk) return;
    ID3D11Buffer* vb = nullptr;
    UINT stride = 0, offset = 0;
    ctx->IAGetVertexBuffers(0, 1, &vb, &stride, &offset);
    if (!vb) return;
    if (stride == ql.stride) {
        ID3D11Buffer*& stage = g_posStage[g_posFrame][ord];
        if (!stage) {
            ID3D11Device* dev = nullptr;
            ctx->GetDevice(&dev);
            D3D11_BUFFER_DESC bd{};
            bd.ByteWidth = 4 * 64;
            bd.Usage = D3D11_USAGE_STAGING;
            bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            if (dev) {
                dev->CreateBuffer(&bd, nullptr, &stage);
                dev->Release();
            }
        }
        D3D11_BUFFER_DESC vd{};
        vb->GetDesc(&vd);
        const int64_t first = static_cast<int64_t>(offset) + (static_cast<int64_t>(base) + g_quadMinIndex) * stride;
        if (stage && first >= 0 && first + 4 * stride <= vd.ByteWidth) {
            D3D11_BOX box{static_cast<UINT>(first), 0, 0, static_cast<UINT>(first) + 4 * stride, 1, 1};
            ctx->CopySubresourceRegion(stage, 0, 0, 0, 0, vb, 0, &box);
            g_posPending[g_posFrame][ord] = true;
        }
    }
    vb->Release();
}

// Once per frame (Present): read the oldest copies, then move on to the next ring slot.
void ReadQuadCorners(ID3D11DeviceContext* ctx, const QuadLayout& ql) {
    const int slot = (g_posFrame + 1) % kRing;  // written kRing-1 frames ago
    for (int k = 0; k < kOrds; ++k) {
        if (!g_posPending[slot][k] || !g_posStage[slot][k]) continue;
        D3D11_MAPPED_SUBRESOURCE m{};
        HRESULT hr = ctx->Map(g_posStage[slot][k], 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m);
        if (hr == DXGI_ERROR_WAS_STILL_DRAWING) continue;  // try again next time round
        g_posPending[slot][k] = false;
        if (FAILED(hr)) continue;
        double x0 = 1e30, y0 = 1e30, x1 = -1e30, y1 = -1e30;
        for (int i = 0; i < 4; ++i) {
            float xy[2];
            memcpy(xy, static_cast<const uint8_t*>(m.pData) + static_cast<size_t>(i) * ql.stride + ql.posOffset, 8);
            x0 = std::min(x0, static_cast<double>(xy[0]));
            x1 = std::max(x1, static_cast<double>(xy[0]));
            y0 = std::min(y0, static_cast<double>(xy[1]));
            y1 = std::max(y1, static_cast<double>(xy[1]));
        }
        ctx->Unmap(g_posStage[slot][k], 0);
        const double sc = ql.ndc ? 1000.0 : 1.0;
        if (std::isfinite(x0 + x1 + y0 + y1) && std::fabs(x0) < 1e5 && std::fabs(y1) < 1e5) {
            g_ordPos[k] = OrdPos{true, static_cast<int>(std::lround(x0 * sc)), static_cast<int>(std::lround(y0 * sc)),
                                 static_cast<int>(std::lround(x1 * sc)), static_cast<int>(std::lround(y1 * sc))};
        }
    }
    g_posFrame = (g_posFrame + 1) % kRing;
}

// True if this draw shows the screen on a display that keeps the game's picture.
bool ScreenDrawHidden(ID3D11DeviceContext* ctx, UINT start, INT base) {
    if (ctx != g_screenCtx || g_screenSlots.none()) return false;
    if (g_drawsReset.exchange(false)) g_draws.clear();
    ScreenDraw d{};
    {
        ID3D11RenderTargetView* rtv = nullptr;
        ctx->OMGetRenderTargets(1, &rtv, nullptr);
        if (rtv) {
            ID3D11Resource* r = nullptr;
            rtv->GetResource(&r);
            ID3D11Texture2D* t = nullptr;
            if (r && SUCCEEDED(r->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&t))) && t) {
                D3D11_TEXTURE2D_DESC td{};
                t->GetDesc(&td);
                d.rtW = static_cast<int>(td.Width);
                d.rtH = static_cast<int>(td.Height);
                t->Release();
            }
            if (r) r->Release();
            rtv->Release();
        }
        D3D11_VIEWPORT vp{};
        UINT nvp = 1;
        ctx->RSGetViewports(&nvp, &vp);
        if (nvp) {
            d.x = static_cast<int>(vp.TopLeftX + 0.5f);
            d.y = static_cast<int>(vp.TopLeftY + 0.5f);
            d.w = static_cast<int>(vp.Width + 0.5f);
            d.h = static_cast<int>(vp.Height + 0.5f);
        }
        D3D11_RECT sc{};
        UINT nsc = 1;
        ctx->RSGetScissorRects(&nsc, &sc);
        if (nsc) {
            d.sx = sc.left;
            d.sy = sc.top;
            d.sw = sc.right - sc.left;
            d.sh = sc.bottom - sc.top;
        }
    }
    const int ord = g_frameDraws++;
    const QuadLayout ql = FitQuadLayout();
    if (ql.ok) {  // known by position (see above); until its position is in, a draw is left alone and not listed
        CopyQuadCorners(ctx, ord, start, base, ql);
        g_posResolved = ord < kOrds && g_ordPos[ord].ok;
        if (!g_posResolved) return false;
        if (g_useOrder) {
            g_useOrder = false;
            g_draws.clear();
            Logf("TAKEOVER the displays are now told apart by their position on the dashboard");
        }
        d.ord = -1;
        d.px0 = g_ordPos[ord].x0;
        d.py0 = g_ordPos[ord].y0;
        d.px1 = g_ordPos[ord].x1;
        d.py1 = g_ordPos[ord].y1;
    } else {
        g_posResolved = true;
    }
    d.ord = ql.ok ? -1 : (g_useOrder ? ord : -1);
    if (!ql.ok && !g_useOrder && ord == 1 && g_draws.size() == 1 && ++g_framesWithMany >= 30) {
        g_useOrder = true;  // two draws per frame look the same: tell them apart by their order
        g_draws.clear();
        Logf("TAKEOVER the displays share target, viewport and clip rectangle: telling them apart by draw order");
        d.ord = ord;
    }
    auto it = std::find(g_draws.begin(), g_draws.end(), d);
    if (it != g_draws.end()) g_drawTick[it - g_draws.begin()] = GetTickCount64();
    if (it == g_draws.end() && g_draws.size() < kMaxDraws) {
        g_drawTick[g_draws.size()] = GetTickCount64();
        g_draws.push_back(d);
        Logf("TAKEOVER display #%zu = %dx%d@%d,%d,%d,%d clip %d,%d,%d,%d order %d at %d,%d..%d,%d (%s)", g_draws.size() - 1,
             d.rtW, d.rtH, d.x, d.y, d.w, d.h, d.sx, d.sy, d.sw, d.sh, d.ord, d.px0, d.py0, d.px1, d.py1,
             IsHidden(d) ? "game picture, gps_takeover_hide" : "CarPlay");
    }
    const bool hidden = !g_hidden.empty() && IsHidden(d);
    if (hidden && ql.ok && !g_fitResetDone) {  // the fit may have measured the other display before they were told apart
        g_fitResetDone = true;
        FitReset("the displays are told apart now: measure the one that shows CarPlay");
    }
    return hidden;
}

// Bind the game's own views (restore = true) or ours again, only into the slots that still hold the other one.
void SwapScreenSlots(ID3D11DeviceContext* ctx, bool restore) {
    ID3D11ShaderResourceView* ours = g_ourSrvAtomic.load(std::memory_order_relaxed);
    if (!ours) return;
    for (UINT s = 0; s < 128; ++s) {
        if (!g_screenSlots.test(s)) continue;
        ID3D11ShaderResourceView* game = g_screenOrig[s];
        if (!IsSwapSrv(game)) continue;  // the game let go of that view meanwhile
        ID3D11ShaderResourceView* cur = nullptr;
        ctx->PSGetShaderResources(s, 1, &cur);
        const bool expected = cur == (restore ? ours : game);
        if (cur) cur->Release();
        if (expected) g_psSetSrv(ctx, s, 1, restore ? &game : &ours);
    }
}

// Page probe (for CarPlay pages without the mod): logs when the game renders its screen texture and when each display
// draws it, so a press of the Infotainment button can be told apart from looking away from the dashboard.
void STDMETHODCALLTYPE HookOMSetRT(ID3D11DeviceContext* ctx, UINT n, ID3D11RenderTargetView* const* rtvs,
                                   ID3D11DepthStencilView* dsv) {
    ID3D11Texture2D* shown = g_gpsShown;
    if (!g_off && shown && rtvs) {
        for (UINT i = 0; i < n && i < 8; ++i) {
            if (!rtvs[i]) continue;
            ID3D11Resource* r = nullptr;
            rtvs[i]->GetResource(&r);
            const bool hit = r == shown;
            if (r) r->Release();
            if (hit) {
                g_rtTick.store(GetTickCount64(), std::memory_order_relaxed);
                break;
            }
        }
    }
    g_omSetRt(ctx, n, rtvs, dsv);
}

void PageProbe() {
    static unsigned last = ~0u;
    static ULONGLONG since = 0;
    const ULONGLONG now = GetTickCount64();
    unsigned state = (now - g_rtTick.load(std::memory_order_relaxed) < 300) ? 1u : 0u;
    for (size_t i = 0; i < g_draws.size(); ++i) state |= (now - g_drawTick[i] < 300 ? 1u : 0u) << (i + 1);
    if (state == last) return;
    char shown[64] = "";
    for (size_t i = 0; i < g_draws.size(); ++i) {
        char one[16];
        snprintf(one, sizeof one, " d%zu=%u", i, (state >> (i + 1)) & 1u);
        strncat(shown, one, sizeof shown - strlen(shown) - 1);
    }
    Logf("TAKEOVER probe: game renders the screen=%u, drawn on:%s (previous state %.1f s)", state & 1u,
         shown[0] ? shown : " nothing", since ? (now - since) / 1000.0 : 0.0);
    last = state;
    since = now;
}

// Ctrl+Shift+F11: CarPlay on all displays -> only display #0 -> only #1 -> ... -> all.
void GpsCycleDisplays() {
    if (g_draws.size() < 2) {
        Logf("TAKEOVER Ctrl+Shift+F11: only %zu display(s) use this screen, nothing to choose", g_draws.size());
        return;
    }
    // Which one is alone now? -1 = all of them show CarPlay.
    int alone = -1, shown = 0;
    for (size_t i = 0; i < g_draws.size(); ++i) {
        if (!IsHidden(g_draws[i])) {
            ++shown;
            alone = static_cast<int>(i);
        }
    }
    if (shown != 1) alone = -1;
    const int next = alone + 1 >= static_cast<int>(g_draws.size()) ? -1 : alone + 1;
    for (const ScreenDraw& d : g_draws) g_hidden.erase(std::remove(g_hidden.begin(), g_hidden.end(), d), g_hidden.end());
    for (int i = 0; next >= 0 && i < static_cast<int>(g_draws.size()); ++i) {
        if (i != next) g_hidden.push_back(g_draws[static_cast<size_t>(i)]);
    }
    SaveHidden();
    FitReset("Ctrl+Shift+F11: another display shows CarPlay");
    if (next < 0) Logf("TAKEOVER Ctrl+Shift+F11: CarPlay on all %zu displays", g_draws.size());
    else Logf("TAKEOVER Ctrl+Shift+F11: CarPlay only on display #%d, the others show the game's picture", next);
}

HRESULT STDMETHODCALLTYPE HookCreateTexture2D(ID3D11Device* dev, const D3D11_TEXTURE2D_DESC* desc,
                                              const D3D11_SUBRESOURCE_DATA* init, ID3D11Texture2D** out) {
    HRESULT hr = g_createTex2D(dev, desc, init, out);
    if (g_off || !desc || !out) return hr;
    if (FAILED(hr)) {
        static std::atomic<int> failures{0};
        if (failures.fetch_add(1) < 12) {
            LogDesc("CreateTexture2D FAILED", *desc, init != nullptr);
            Logf("SCREEN   hr=0x%08lx deviceRemovedReason=%s", static_cast<unsigned long>(hr),
                 RemovedReasonName(dev->GetDeviceRemovedReason()));
        }
        return hr;
    }
    if (g_replaceTexture && *out && IsPlaceholder(desc)) OnPlaceholderCreated(dev, *desc, *out, init != nullptr);
    else if (g_replaceTexture && *out && EtaIsPlaceholder(desc)) EtaOnPlaceholderCreated(dev, *desc, *out, g_createTex2D);
    else if (g_replaceTexture && *out && GetSettings().gpsTakeover && IsGpsRenderTarget(desc)) OnGpsTargetCreated(*desc, *out);
    return hr;
}

void UploadScreenFrame() {
    ID3D11Texture2D* tex = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_screenMx);
        tex = g_ourTex;
        if (tex) tex->AddRef();
    }
    if (!tex) return;

    ID3D11Device* dev = nullptr;
    tex->GetDevice(&dev);
    if (dev) {
        if (!g_removedLogged) {
            HRESULT reason = dev->GetDeviceRemovedReason();
            if (reason != S_OK) {
                g_removedLogged = true;
                Logf("SCREEN !!! the graphics device was removed: %s (0x%08lx) after %llu uploads", RemovedReasonName(reason),
                     static_cast<unsigned long>(reason), static_cast<unsigned long long>(g_uploadCount));
            }
        }
        ULONGLONG now = GetTickCount64();
        if (now - g_lastHeartbeat > 60000) {
            g_lastHeartbeat = now;
            Logf("SCREEN alive: %llu frames uploaded so far", static_cast<unsigned long long>(g_uploadCount));
        }
    }

    // only while the music page is on screen (bound within the last second): no 2.6 MB uploads for a hidden page
    const bool pageVisible = GetTickCount64() - g_screenBoundTick.load(std::memory_order_relaxed) < 1000;
    if (g_uploadFrames && dev && !g_removedLogged && pageVisible) {
        if (tex != g_screenUploadedTo) {
            g_screenUploadedTo = tex;
            g_screenUploaded = 0;
        }
        FrameView frame;
        if (RendererLockFrame(g_screenUploaded, &frame)) {
            ID3D11DeviceContext* ctx = nullptr;
            dev->GetImmediateContext(&ctx);
            if (ctx) {
                ctx->UpdateSubresource(tex, 0, nullptr, frame.data, frame.pitch, 0);
                ctx->Release();
                if (g_uploadCount == 0) Logf("SCREEN first frame uploaded (%ux%u)", frame.width, frame.height);
                ++g_uploadCount;
            }
            g_screenUploaded = frame.version;
            RendererUnlockFrame();
        }
    }
    if (dev) dev->Release();
    tex->Release();
}

// ---------------------------------------------------------------------------------------------
// Texture finder (diagnostic).
//
// Goal: find out which texture the cabin GPS screen samples, so later milestones can swap it.
// Method: record every 2D texture the game binds for ~2.5 s, then bisect: paint half of the
// candidates magenta, the user tells us whether the GPS turned magenta, repeat.
//   Ctrl+Shift+F6  start (GPS must be visible in the cabin)
//   Ctrl+Shift+F7  "the GPS screen is magenta"
//   Ctrl+Shift+F8  "the GPS screen is NOT magenta"
//   Ctrl+Shift+F9  stop
// ---------------------------------------------------------------------------------------------
struct Cand {
    ID3D11Texture2D* tex;
    D3D11_TEXTURE2D_DESC desc;
};

enum Mode { kIdle = 0, kRecording = 1, kTinting = 2 };

std::mutex g_mx;
std::unordered_map<ID3D11ShaderResourceView*, ID3D11Texture2D*> g_srvCache;
std::unordered_map<ID3D11Texture2D*, size_t> g_seen;
std::vector<Cand> g_cands;
std::unordered_set<ID3D11Texture2D*> g_tint;
size_t g_lo = 0, g_hi = 0, g_mid = 0;
ID3D11ShaderResourceView* g_tintSrv = nullptr;
std::atomic<int> g_mode{kIdle};
ULONGLONG g_recordUntil = 0;

const char* FormatName(DXGI_FORMAT f) {
    switch (f) {
        case DXGI_FORMAT_R8G8B8A8_UNORM: return "RGBA8";
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return "RGBA8_SRGB";
        case DXGI_FORMAT_B8G8R8A8_UNORM: return "BGRA8";
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return "BGRA8_SRGB";
        case DXGI_FORMAT_R16G16B16A16_FLOAT: return "RGBA16F";
        case DXGI_FORMAT_R11G11B10_FLOAT: return "R11G11B10F";
        case DXGI_FORMAT_BC1_UNORM: return "BC1";
        case DXGI_FORMAT_BC1_UNORM_SRGB: return "BC1_SRGB";
        case DXGI_FORMAT_BC2_UNORM: return "BC2";
        case DXGI_FORMAT_BC3_UNORM: return "BC3";
        case DXGI_FORMAT_BC3_UNORM_SRGB: return "BC3_SRGB";
        case DXGI_FORMAT_BC4_UNORM: return "BC4";
        case DXGI_FORMAT_BC5_UNORM: return "BC5";
        case DXGI_FORMAT_BC7_UNORM: return "BC7";
        case DXGI_FORMAT_BC7_UNORM_SRGB: return "BC7_SRGB";
        case DXGI_FORMAT_R8_UNORM: return "R8";
        case DXGI_FORMAT_R8G8_UNORM: return "RG8";
        case DXGI_FORMAT_R16_TYPELESS: return "R16_TYPELESS";
        case DXGI_FORMAT_R32_TYPELESS: return "R32_TYPELESS";
        case DXGI_FORMAT_R24G8_TYPELESS: return "R24G8_TYPELESS";
        default: return "?";
    }
}

void LogCand(const char* prefix, size_t index, const Cand& c) {
    const D3D11_TEXTURE2D_DESC& d = c.desc;
    Logf("%s #%zu tex=%p %ux%u fmt=%u(%s) mips=%u usage=%u bind=0x%x cpu=0x%x misc=0x%x", prefix, index,
         static_cast<void*>(c.tex), d.Width, d.Height, static_cast<unsigned>(d.Format), FormatName(d.Format),
         d.MipLevels, static_cast<unsigned>(d.Usage), d.BindFlags, d.CPUAccessFlags, d.MiscFlags);
}

ID3D11Texture2D* ResolveTexLocked(ID3D11ShaderResourceView* srv) {
    auto it = g_srvCache.find(srv);
    if (it != g_srvCache.end()) return it->second;

    ID3D11Texture2D* tex = nullptr;
    D3D11_SHADER_RESOURCE_VIEW_DESC vd{};
    srv->GetDesc(&vd);
    if (vd.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2D) {
        ID3D11Resource* res = nullptr;
        srv->GetResource(&res);
        if (res) {
            res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&tex));
            res->Release();
            if (tex) tex->Release();  // the SRV keeps the texture alive; we only keep a key
        }
    }
    g_srvCache[srv] = tex;
    return tex;
}

void EnsureTintSrvLocked(ID3D11DeviceContext* ctx) {
    if (g_tintSrv) return;
    ID3D11Device* dev = nullptr;
    ctx->GetDevice(&dev);
    if (!dev) return;
    uint32_t pixels[16];
    std::fill(std::begin(pixels), std::end(pixels), 0xFFFF00FFu);  // ABGR: opaque magenta
    D3D11_TEXTURE2D_DESC td{};
    td.Width = td.Height = 4;
    td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_IMMUTABLE;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA sd{pixels, 16, 0};
    ID3D11Texture2D* tex = nullptr;
    if (SUCCEEDED(dev->CreateTexture2D(&td, &sd, &tex))) {
        // Call the original: going through the vtable would enter HookCreateSRV and re-lock g_mx.
        g_createSrv(dev, tex, nullptr, &g_tintSrv);
        tex->Release();
    }
    dev->Release();
}

void STDMETHODCALLTYPE HookPSSetSRV(ID3D11DeviceContext* ctx, UINT start, UINT n, ID3D11ShaderResourceView* const* srvsIn) {
    if (g_off || !srvsIn || n == 0 || n > 128) return g_psSetSrv(ctx, start, n, srvsIn);

    // Cabin screen: the game binds its view of the placeholder, the shader gets ours.
    ID3D11ShaderResourceView* swapped[128];
    ID3D11ShaderResourceView* const* srvs = srvsIn;
    {
        // MUSIK picture (our main texture) and ANKUNFT time (eta_screen) are swapped independently.
        ID3D11ShaderResourceView* ours = g_ourSrvAtomic.load(std::memory_order_relaxed);
        bool any = false;
        bool isScreen[128];
        for (UINT i = 0; i < n; ++i) {
            ID3D11ShaderResourceView* s = srvsIn[i];
            isScreen[i] = ours && s && IsSwapSrv(s);
            if (isScreen[i]) g_screenBoundTick.store(GetTickCount64(), std::memory_order_relaxed);
            ID3D11ShaderResourceView* rep = isScreen[i] ? ours : EtaSwapFor(s);
            if (rep) {
                if (!any) {
                    memcpy(swapped, srvsIn, n * sizeof(swapped[0]));
                    any = true;
                }
                swapped[i] = rep;
            }
        }
        if (any) srvs = swapped;
        if (ours && (any || TouchHasBindings())) TouchNoteBind(ctx, start, n, isScreen);  // beta: where is the screen bound?
        if (ours && g_gpsShown && (any || g_screenSlots.any())) NoteScreenSlots(ctx, start, n, srvsIn, isScreen);
    }

    int mode = g_mode.load(std::memory_order_relaxed);
    if (mode == kIdle) return g_psSetSrv(ctx, start, n, srvs);

    ID3D11ShaderResourceView* local[128];
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(g_mx);
        if (mode == kTinting) EnsureTintSrvLocked(ctx);
        for (UINT i = 0; i < n; ++i) {
            local[i] = srvs[i];
            if (!srvs[i]) continue;
            ID3D11Texture2D* tex = ResolveTexLocked(srvs[i]);
            if (!tex) continue;
            if (mode == kRecording) {
                if (g_seen.find(tex) == g_seen.end()) {
                    D3D11_TEXTURE2D_DESC d{};
                    tex->GetDesc(&d);
                    bool plain = d.ArraySize == 1 && !(d.MiscFlags & D3D11_RESOURCE_MISC_TEXTURECUBE) &&
                                 d.SampleDesc.Count == 1 && d.Width >= 32 && d.Height >= 32;
                    g_seen[tex] = plain ? g_cands.size() : SIZE_MAX;
                    if (plain) g_cands.push_back({tex, d});
                }
            } else if (g_tintSrv && g_tint.count(tex)) {
                local[i] = g_tintSrv;
                changed = true;
            }
        }
    }
    g_psSetSrv(ctx, start, n, changed ? local : srvs);
}

HRESULT STDMETHODCALLTYPE HookCreateSRV(ID3D11Device* dev, ID3D11Resource* res,
                                        const D3D11_SHADER_RESOURCE_VIEW_DESC* desc, ID3D11ShaderResourceView** out) {
    HRESULT hr = g_createSrv(dev, res, desc, out);
    if (g_off) return hr;
    static std::atomic<int> srvFailures{0};
    if (FAILED(hr) && srvFailures.fetch_add(1) < 12) {
        Logf("SCREEN CreateShaderResourceView FAILED hr=0x%08lx deviceRemovedReason=%s", static_cast<unsigned long>(hr),
             RemovedReasonName(dev->GetDeviceRemovedReason()));
    }
    if (SUCCEEDED(hr) && out && *out) {
        // A new view may reuse the address of a released one: forget what we knew about it.
        RemoveSwapSrv(*out);
        EtaForgetView(*out);
        if (GetSettings().gpsTakeover) GpsForgetView(*out);
        if (EtaIsPlaceholderResource(res)) EtaOnPlaceholderViewCreated(dev, res, *out, desc, g_createSrv);
        {
            std::lock_guard<std::mutex> lock(g_mx);
            g_srvCache.erase(*out);
        }
        bool isPlaceholder = false;
        if (res) {
            std::lock_guard<std::mutex> lock(g_screenMx);
            isPlaceholder = res == g_placeholder;
        }
        if (isPlaceholder) OnPlaceholderViewCreated(dev, *out, desc);
        else if (res && GetSettings().gpsTakeover) OnGpsViewCreated(dev, res, *out, desc);
    }
    return hr;
}

void SelectTintRangeLocked() {
    g_tint.clear();
    for (size_t i = g_lo; i < g_mid; ++i) g_tint.insert(g_cands[i].tex);
}

void ReportStepLocked() {
    if (g_hi - g_lo <= 1) {
        Logf("FINDER ====== RESULT: texture candidate #%zu ======", g_lo);
        LogCand("FINDER result", g_lo, g_cands[g_lo]);
        size_t same = 0;
        for (const Cand& c : g_cands) {
            if (c.desc.Width == g_cands[g_lo].desc.Width && c.desc.Height == g_cands[g_lo].desc.Height &&
                c.desc.Format == g_cands[g_lo].desc.Format)
                ++same;
        }
        Logf("FINDER %zu of %zu recorded textures share this width/height/format", same, g_cands.size());
        g_mid = g_hi;
        SelectTintRangeLocked();
        MessageBeep(MB_ICONASTERISK);
        return;
    }
    g_mid = g_lo + (g_hi - g_lo) / 2;
    SelectTintRangeLocked();
    Logf("FINDER range [%zu,%zu) -> painting [%zu,%zu) magenta. F7 = GPS is magenta, F8 = not magenta", g_lo, g_hi,
         g_lo, g_mid);
    MessageBeep(MB_OK);
}

void FinishRecording() {
    std::lock_guard<std::mutex> lock(g_mx);
    if (g_mode.load() != kRecording) return;
    if (g_cands.size() < 2) {
        Logf("FINDER recorded only %zu textures - is the game rendering? Stopping.", g_cands.size());
        g_mode = kIdle;
        return;
    }
    Logf("FINDER recorded %zu candidate textures", g_cands.size());
    g_lo = 0;
    g_hi = g_cands.size();
    g_mode = kTinting;
    ReportStepLocked();
}

void FinderKey(int vk) {
    switch (vk) {
        case VK_F6: {
            std::lock_guard<std::mutex> lock(g_mx);
            g_seen.clear();
            g_cands.clear();
            g_tint.clear();
            g_recordUntil = GetTickCount64() + 2500;
            g_mode = kRecording;
            Logf("FINDER recording textures for 2.5 s - keep the GPS screen in view");
            break;
        }
        case VK_F7:
        case VK_F8: {
            std::lock_guard<std::mutex> lock(g_mx);
            if (g_mode.load() != kTinting) break;
            if (g_hi - g_lo <= 1) {
                Logf("FINDER already at a single candidate; press Ctrl+Shift+F6 to restart");
                break;
            }
            if (vk == VK_F7) g_hi = g_mid;  // GPS was painted: it is in the painted half
            else g_lo = g_mid;              // not painted: it is in the other half
            ReportStepLocked();
            break;
        }
        case VK_F9: {
            std::lock_guard<std::mutex> lock(g_mx);
            g_mode = kIdle;
            g_tint.clear();
            Logf("FINDER stopped");
            break;
        }
    }
}

bool ForegroundIsUs() {
    HWND w = GetForegroundWindow();
    DWORD pid = 0;
    if (w) GetWindowThreadProcessId(w, &pid);
    return pid == GetCurrentProcessId();
}

void OnFrame(IDXGISwapChain* sc) {
    static bool down[4] = {};
    static const int keys[4] = {VK_F6, VK_F7, VK_F8, VK_F9};

    if (g_mode.load() == kRecording && GetTickCount64() >= g_recordUntil) FinishRecording();
    UploadScreenFrame();
    EtaUpload();
    TouchOnPresent(sc);

    if (GetSettings().gpsTakeover) {  // Ctrl+Shift+F10: CarPlay on the next candidate screen / back to the stock GPS
        static bool f10 = false;
        bool now = (GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_SHIFT) & 0x8000) &&
                   (GetAsyncKeyState(VK_F10) & 0x8000) && ForegroundIsUs();
        if (now && !f10) GpsCycle();
        f10 = now;
        static bool f11 = false;  // Ctrl+Shift+F11: which displays show CarPlay (vehicles with two on one texture)
        now = (GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_SHIFT) & 0x8000) &&
              (GetAsyncKeyState(VK_F11) & 0x8000) && ForegroundIsUs();
        if (now && !f11) GpsCycleDisplays();
        f11 = now;
        PageProbe();
        g_frameDraws = 0;
        g_posResolved = true;  // draws outside the screen pass are not affected
        if (g_screenCtx) {
            const QuadLayout ql = FitQuadLayout();
            if (ql.ok) ReadQuadCorners(g_screenCtx, ql);
        }
    }

    if (!GetSettings().textureFinder) return;  // developer tool, off unless [debug] texture_finder=1
    bool chord = (GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_SHIFT) & 0x8000) && ForegroundIsUs();
    for (int i = 0; i < 4; ++i) {
        bool now = chord && (GetAsyncKeyState(keys[i]) & 0x8000);
        if (now && !down[i]) FinderKey(keys[i]);
        down[i] = now;
    }
}

thread_local int t_presentDepth = 0;

HRESULT STDMETHODCALLTYPE HookPresent(IDXGISwapChain* sc, UINT sync, UINT flags) {
    if (!g_off && t_presentDepth == 0) OnFrame(sc);
    ++t_presentDepth;
    HRESULT hr = g_present(sc, sync, flags);
    --t_presentDepth;
    return hr;
}

// Beta touch: the draw calls that show the screen are repeated once into a 1x1 probe target.
using DrawIndexedFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, INT);
using DrawIndexedInstancedFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT, INT, UINT);
DrawIndexedFn g_drawIndexed;
DrawIndexedInstancedFn g_drawIndexedInstanced;

// The display mesh is read once per screen (shape for the fit), and replayed for the mouse / orientation scan.
void STDMETHODCALLTYPE HookDrawIndexed(ID3D11DeviceContext* ctx, UINT count, UINT start, INT base) {
    if (!g_off && ScreenDrawHidden(ctx, start, base)) {  // a display that keeps the game's picture
        SwapScreenSlots(ctx, true);
        g_drawIndexed(ctx, count, start, base);
        SwapScreenSlots(ctx, false);
        return;
    }
    if (!g_off) {
        if (TouchHasBindings() && FitNeedsGeometry() && g_posResolved && TouchScreenBound(ctx)) {
            FitCaptureGeometry(ctx, count, start, base);
        }
        if (TouchNeedsReplay(ctx)) TouchReplay(ctx, [&] { g_drawIndexed(ctx, count, start, base); });
    }
    g_drawIndexed(ctx, count, start, base);
}

void STDMETHODCALLTYPE HookDrawIndexedInstanced(ID3D11DeviceContext* ctx, UINT count, UINT instances, UINT start, INT base,
                                                UINT startInstance) {
    if (!g_off && ScreenDrawHidden(ctx, start, base)) {
        SwapScreenSlots(ctx, true);
        g_drawIndexedInstanced(ctx, count, instances, start, base, startInstance);
        SwapScreenSlots(ctx, false);
        return;
    }
    if (!g_off) {
        if (TouchHasBindings() && FitNeedsGeometry() && g_posResolved && TouchScreenBound(ctx)) {
            FitCaptureGeometry(ctx, count, start, base);
        }
        if (TouchNeedsReplay(ctx)) {
            TouchReplay(ctx, [&] { g_drawIndexedInstanced(ctx, count, instances, start, base, startInstance); });
        }
    }
    g_drawIndexedInstanced(ctx, count, instances, start, base, startInstance);
}

using CreateInputLayoutFn = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, const D3D11_INPUT_ELEMENT_DESC*, UINT, const void*,
                                                        SIZE_T, ID3D11InputLayout**);
CreateInputLayoutFn g_createInputLayout;

HRESULT STDMETHODCALLTYPE HookCreateInputLayout(ID3D11Device* dev, const D3D11_INPUT_ELEMENT_DESC* elements, UINT count,
                                                const void* code, SIZE_T codeLen, ID3D11InputLayout** out) {
    HRESULT hr = g_createInputLayout(dev, elements, count, code, codeLen, out);
    if (SUCCEEDED(hr) && out && *out && !g_off) FitOnCreateInputLayout(*out, elements, count);  // remembered for the screen fit
    return hr;
}

HRESULT STDMETHODCALLTYPE HookPresent1(IDXGISwapChain1* sc, UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS* pp) {
    if (!g_off && t_presentDepth == 0) OnFrame(sc);
    ++t_presentDepth;
    HRESULT hr = g_present1(sc, sync, flags, pp);
    --t_presentDepth;
    return hr;
}

LRESULT CALLBACK DummyWndProc(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcW(h, m, w, l); }

}  // namespace

bool D3D11HooksInstall(bool replaceTexture, bool uploadFrames, bool touch) {
    g_replaceTexture = replaceTexture;
    g_uploadFrames = uploadFrames;
    g_gpsSkip = std::max(0, GetSettings().gpsTakeoverPick);
    LoadHidden();
    g_gpsOff = GetSettings().gpsTakeoverPick < 0;
    if (GetSettings().gpsTakeover) Logf("TAKEOVER on: CarPlay replaces the game's cab screen (start with candidate %d)", GetSettings().gpsTakeoverPick);
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = DummyWndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"CoachNowPlayingDummy";
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPED, 0, 0, 16, 16, nullptr, nullptr,
                                wc.hInstance, nullptr);

    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 1;
    sd.BufferDesc.Width = sd.BufferDesc.Height = 16;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    IDXGISwapChain* sc = nullptr;
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    D3D_FEATURE_LEVEL fl{};
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
                                               D3D11_SDK_VERSION, &sd, &sc, &dev, &fl, &ctx);
    if (FAILED(hr)) {
        Logf("D3D11 dummy device failed: 0x%08lx", static_cast<unsigned long>(hr));
        if (hwnd) DestroyWindow(hwnd);
        UnregisterClassW(wc.lpszClassName, wc.hInstance);
        return false;
    }

    auto vt = [](void* obj) { return *reinterpret_cast<void***>(obj); };
    void* present = vt(sc)[8];
    void* createTex2D = vt(dev)[5];
    void* createSrv = vt(dev)[7];
    void* psSetSrv = vt(ctx)[8];
    void* omSetRt = vt(ctx)[33];
    void* drawIndexed = vt(ctx)[12];
    void* drawIndexedInstanced = vt(ctx)[20];
    void* createInputLayout = vt(dev)[11];
    void* present1 = nullptr;
    IDXGISwapChain1* sc1 = nullptr;
    if (SUCCEEDED(sc->QueryInterface(__uuidof(IDXGISwapChain1), reinterpret_cast<void**>(&sc1)))) {
        present1 = vt(sc1)[22];
        sc1->Release();
    }

    ctx->Release();
    dev->Release();
    sc->Release();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);

    bool ok = true;
    auto hook = [&](const char* name, void* target, void* detour, void** orig) {
        if (!target) return;
        MH_STATUS st = MH_CreateHook(target, detour, orig);
        if (st != MH_OK) {
            Logf("D3D11 hook %s failed: %s", name, MH_StatusToString(st));
            ok = false;
        }
    };
    hook("Present", present, reinterpret_cast<void*>(&HookPresent), reinterpret_cast<void**>(&g_present));
    hook("Present1", present1, reinterpret_cast<void*>(&HookPresent1), reinterpret_cast<void**>(&g_present1));
    hook("CreateTexture2D", createTex2D, reinterpret_cast<void*>(&HookCreateTexture2D),
         reinterpret_cast<void**>(&g_createTex2D));
    hook("CreateShaderResourceView", createSrv, reinterpret_cast<void*>(&HookCreateSRV),
         reinterpret_cast<void**>(&g_createSrv));
    hook("PSSetShaderResources", psSetSrv, reinterpret_cast<void*>(&HookPSSetSRV),
         reinterpret_cast<void**>(&g_psSetSrv));
    if (GetSettings().gpsTakeover) {
        hook("OMSetRenderTargets", omSetRt, reinterpret_cast<void*>(&HookOMSetRT), reinterpret_cast<void**>(&g_omSetRt));
    }
    if (touch || FitEnabled() || GetSettings().gpsTakeover) {  // draw hooks cost a little on every draw call, so only when they are needed
        hook("DrawIndexed", drawIndexed, reinterpret_cast<void*>(&HookDrawIndexed), reinterpret_cast<void**>(&g_drawIndexed));
        hook("DrawIndexedInstanced", drawIndexedInstanced, reinterpret_cast<void*>(&HookDrawIndexedInstanced),
             reinterpret_cast<void**>(&g_drawIndexedInstanced));
    }
    if (FitEnabled()) {
        hook("CreateInputLayout", createInputLayout, reinterpret_cast<void*>(&HookCreateInputLayout),
             reinterpret_cast<void**>(&g_createInputLayout));
    }
    if (!ok) return false;
    if (MH_EnableHook(MH_ALL_HOOKS) != MH_OK) {
        Logf("D3D11 MH_EnableHook failed");
        return false;
    }
    TouchInit(touch, g_psSetSrv);
    Logf("D3D11 hooks installed (Present=%p Present1=%p CreateTex2D=%p CreateSRV=%p PSSetSRV=%p)", present, present1,
         createTex2D, createSrv, psSetSrv);
    return true;
}

void D3D11HooksDisable() {
    g_off = true;
    g_mode = kIdle;
}

}  // namespace ep

namespace ep {
bool TakeoverFlip() { return g_takeoverFlip.load(std::memory_order_relaxed); }
}  // namespace ep
