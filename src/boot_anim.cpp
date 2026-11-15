#include "boot_anim.h"

#include "boot_logo.h"

#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <mutex>

namespace ep {
namespace {

constexpr float kPi = 3.14159265358979f;

// ---- timeline (seconds) ----------------------------------------------------------------------------------------------
constexpr float kAppearEnd = 0.40f;   // dots grow in one after another
constexpr float kOrbitEnd = 1.95f;    // two "breaths": contract and spread while circling
constexpr float kMergeEnd = 2.25f;    // dots fly into the centre and vanish there
constexpr float kLogoStart = 2.25f;   // logo pops up once the dots are gone
constexpr float kLogoEnd = 2.75f;
constexpr float kFadeStart = 3.45f;   // whole overlay fades, the map shows

// CarPlay page colours of the mod (map blue, trip green, bus orange, music pink), sRGB
struct Rgb { float r, g, b; };
constexpr Rgb kDotColours[4] = {{0x2F, 0x6F, 0xC4}, {0x2F, 0x9E, 0x52}, {0xC9, 0x7A, 0x25}, {0xC2, 0x3A, 0x55}};

float Clamp01(float v) { return std::clamp(v, 0.0f, 1.0f); }
float Phase(float t, float a, float b) { return Clamp01((t - a) / (b - a)); }
float EaseInOut(float p) { return p < 0.5f ? 4 * p * p * p : 1 - std::pow(-2 * p + 2, 3.0f) / 2; }
float EaseIn(float p) { return p * p * p; }
float EaseOutBack(float p) {
    const float c1 = 1.70158f, c3 = c1 + 1;
    return 1 + c3 * std::pow(p - 1, 3.0f) + c1 * std::pow(p - 1, 2.0f);
}

// ---- dot motion (units: fraction of the picture height) ----------------------------------------------------------------
struct Dot { float x, y, r; };

Dot DotAt(int i, float t) {
    const float baseR = 0.17f;       // orbit radius
    const float dotR = 0.052f;       // dot radius
    // grow in, staggered
    const float grow = EaseOutBack(Phase(t, i * 0.06f, i * 0.06f + 0.20f));
    float orbit = baseR, angle = -kPi / 2 + i * kPi / 2, r = dotR * std::max(grow, 0.0f);
    // two breaths: the ring contracts to a small knot and opens again, turning faster while it is tight
    const float p = Phase(t, kAppearEnd, kOrbitEnd);
    const float breath = std::pow(std::sin(kPi * 2.0f * p), 2.0f);           // 0 open .. 1 tight, twice
    orbit *= 1.0f - 0.78f * breath;
    angle += 2 * kPi * 1.25f * EaseInOut(p) + 0.9f * std::sin(2 * kPi * 2 * p);
    // merge: into the centre, dots swell a little and then disappear behind the logo
    const float m = Phase(t, kOrbitEnd, kMergeEnd);
    orbit *= 1.0f - EaseIn(m);
    angle += 1.6f * kPi * EaseIn(m);
    r *= (1.0f + 0.45f * m) * (1.0f - Phase(t, kMergeEnd - 0.12f, kMergeEnd));
    return {orbit * std::cos(angle), orbit * std::sin(angle), r};
}

// ---- logo (embedded PNG, decoded once to premultiplied BGRA) -----------------------------------------------------------
std::once_flag g_logoOnce;
std::vector<uint8_t> g_logo;  // 256 x 256 PBGRA
int g_logoSize = 0;

void DecodeLogo() {
    using Microsoft::WRL::ComPtr;
    // Present thread: COM stays initialised (the cover pictures use WIC on the same thread, see BlitPicture)
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ComPtr<IWICImagingFactory> wic;
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapDecoder> dec;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICFormatConverter> conv;
    UINT w = 0, h = 0;
    if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))) &&
        SUCCEEDED(wic->CreateStream(&stream)) &&
        SUCCEEDED(stream->InitializeFromMemory(const_cast<BYTE*>(kBootLogoPng), static_cast<DWORD>(kBootLogoPngSize))) &&
        SUCCEEDED(wic->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &dec)) &&
        SUCCEEDED(dec->GetFrame(0, &frame)) && SUCCEEDED(frame->GetSize(&w, &h)) && w == h && w > 0 &&
        SUCCEEDED(wic->CreateFormatConverter(&conv)) &&
        SUCCEEDED(conv->Initialize(frame.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                                   WICBitmapPaletteTypeCustom))) {
        std::vector<uint8_t> px(static_cast<size_t>(w) * h * 4);
        if (SUCCEEDED(conv->CopyPixels(nullptr, w * 4, static_cast<UINT>(px.size()), px.data()))) {
            g_logo = std::move(px);
            g_logoSize = static_cast<int>(w);
        }
    }
}

// bilinear sample of the premultiplied logo at (u, v) in 0..1; returns false outside
bool SampleLogo(float u, float v, float out[4]) {
    if (u < 0 || v < 0 || u >= 1 || v >= 1 || g_logoSize == 0) return false;
    const float fx = u * g_logoSize - 0.5f, fy = v * g_logoSize - 0.5f;
    const int x0 = static_cast<int>(std::floor(fx)), y0 = static_cast<int>(std::floor(fy));
    const float ax = fx - x0, ay = fy - y0;
    for (int c = 0; c < 4; ++c) out[c] = 0;
    for (int k = 0; k < 4; ++k) {
        const int xx = std::clamp(x0 + (k & 1), 0, g_logoSize - 1), yy = std::clamp(y0 + (k >> 1), 0, g_logoSize - 1);
        const float wgt = ((k & 1) ? ax : 1 - ax) * ((k >> 1) ? ay : 1 - ay);
        const uint8_t* s = &g_logo[(static_cast<size_t>(yy) * g_logoSize + xx) * 4];
        for (int c = 0; c < 4; ++c) out[c] += s[c] * wgt;
    }
    return true;
}

// distance from p to the segment a-b
float SegDist(float px, float py, float ax, float ay, float bx, float by) {
    const float vx = bx - ax, vy = by - ay, wx = px - ax, wy = py - ay;
    const float len2 = vx * vx + vy * vy;
    const float s = len2 > 0 ? Clamp01((wx * vx + wy * vy) / len2) : 0.0f;
    const float dx = wx - s * vx, dy = wy - s * vy;
    return std::sqrt(dx * dx + dy * dy);
}

}  // namespace

std::vector<uint8_t> BootDraw(unsigned w, unsigned h, float t) {
    std::vector<uint8_t> out(static_cast<size_t>(w) * h * 4, 0);
    if (t < 0 || t >= kBootSeconds || w == 0 || h == 0) return out;
    std::call_once(g_logoOnce, DecodeLogo);

    // working buffer: colour in 0..255 (sRGB), over opaque black
    std::vector<float> col(static_cast<size_t>(w) * h * 3, 0.0f);
    const float H = static_cast<float>(h), cx = w * 0.5f, cy = h * 0.5f;

    // dots as capsules stretched along their motion (the "squash and stretch" of the reference)
    if (t < kMergeEnd) {
        for (int i = 0; i < 4; ++i) {
            const Dot d = DotAt(i, t), p = DotAt(i, std::max(0.0f, t - 1.0f / 60.0f));
            if (d.r <= 0.0005f) continue;
            const float px = cx + d.x * H, py = cy + d.y * H, rad = d.r * H;
            float vx = (d.x - p.x) * H * 60.0f, vy = (d.y - p.y) * H * 60.0f;   // px per second
            const float speed = std::sqrt(vx * vx + vy * vy);
            const float stretch = std::min(speed / (H * 2.6f), 1.3f);           // 0 = round
            const float half = rad * stretch;                                    // half length of the capsule core
            const float thick = rad / std::sqrt(1.0f + 0.6f * stretch);           // thinner while stretched
            if (speed > 0) { vx /= speed; vy /= speed; }
            const float ax = px - vx * half, ay = py - vy * half, bx = px + vx * half, by = py + vy * half;
            const int x0 = std::max(0, static_cast<int>(std::min(ax, bx) - thick - 2));
            const int x1 = std::min(static_cast<int>(w) - 1, static_cast<int>(std::max(ax, bx) + thick + 2));
            const int y0 = std::max(0, static_cast<int>(std::min(ay, by) - thick - 2));
            const int y1 = std::min(static_cast<int>(h) - 1, static_cast<int>(std::max(ay, by) + thick + 2));
            for (int y = y0; y <= y1; ++y) {
                for (int x = x0; x <= x1; ++x) {
                    const float cov = Clamp01(thick - SegDist(x + 0.5f, y + 0.5f, ax, ay, bx, by) + 0.5f);
                    if (cov <= 0) continue;
                    float* o = &col[(static_cast<size_t>(y) * w + x) * 3];
                    o[0] += (kDotColours[i].b - o[0]) * cov;
                    o[1] += (kDotColours[i].g - o[1]) * cov;
                    o[2] += (kDotColours[i].r - o[2]) * cov;
                }
            }
        }
    }

    // logo: pops up with a small overshoot
    const float lp = Phase(t, kLogoStart, kLogoEnd);
    if (lp > 0 && g_logoSize > 0) {
        const float scale = std::max(0.0f, EaseOutBack(lp));
        const float alpha = Clamp01(lp * 2.5f);
        const float size = H * 0.50f * scale;  // logo size
        if (size > 1) {
            const int x0 = std::max(0, static_cast<int>(cx - size / 2) - 1), x1 = std::min(static_cast<int>(w) - 1, static_cast<int>(cx + size / 2) + 1);
            const int y0 = std::max(0, static_cast<int>(cy - size / 2) - 1), y1 = std::min(static_cast<int>(h) - 1, static_cast<int>(cy + size / 2) + 1);
            float s[4];
            for (int y = y0; y <= y1; ++y) {
                for (int x = x0; x <= x1; ++x) {
                    if (!SampleLogo((x + 0.5f - (cx - size / 2)) / size, (y + 0.5f - (cy - size / 2)) / size, s)) continue;
                    const float a = s[3] / 255.0f * alpha;
                    if (a <= 0) continue;
                    float* o = &col[(static_cast<size_t>(y) * w + x) * 3];
                    for (int c = 0; c < 3; ++c) o[c] = s[c] * alpha + o[c] * (1 - a);   // premultiplied over
                }
            }
        }
    }

    // straight alpha: opaque black screen, fading out at the end so the map shows through
    const float fade = 1.0f - EaseInOut(Phase(t, kFadeStart, kBootSeconds));
    const uint8_t alpha8 = static_cast<uint8_t>(std::lround(fade * 255.0f));
    for (size_t i = 0, n = static_cast<size_t>(w) * h; i < n; ++i) {
        out[i * 4 + 0] = static_cast<uint8_t>(std::clamp(col[i * 3 + 0], 0.0f, 255.0f));
        out[i * 4 + 1] = static_cast<uint8_t>(std::clamp(col[i * 3 + 1], 0.0f, 255.0f));
        out[i * 4 + 2] = static_cast<uint8_t>(std::clamp(col[i * 3 + 2], 0.0f, 255.0f));
        out[i * 4 + 3] = alpha8;
    }
    return out;
}

}  // namespace ep
