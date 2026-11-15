#include "plate.h"

#include "text_util.h"

#include <windows.h>

#include <cmath>
#include <cstring>

namespace ep {
namespace {

struct CountryInfo { const char* id; const wchar_t* code; bool eu; };
// country ids of ETS2 -> code on the plate, EU member (blue band with stars)
const CountryInfo kCountries[] = {
    {"germany", L"D", true},      {"austria", L"A", true},      {"belgium", L"B", true},       {"czech", L"CZ", true},
    {"czech_republic", L"CZ", true}, {"denmark", L"DK", true},  {"france", L"F", true},        {"italy", L"I", true},
    {"luxembourg", L"L", true},   {"netherlands", L"NL", true}, {"poland", L"PL", true},       {"slovakia", L"SK", true},
    {"hungary", L"H", true},      {"sweden", L"S", true},       {"finland", L"FIN", true},     {"estonia", L"EST", true},
    {"latvia", L"LV", true},      {"lithuania", L"LT", true},   {"romania", L"RO", true},      {"bulgaria", L"BG", true},
    {"portugal", L"P", true},     {"spain", L"E", true},        {"greece", L"GR", true},       {"croatia", L"HR", true},
    {"slovenia", L"SLO", true},   {"ireland", L"IRL", true},    {"switzerland", L"CH", false}, {"norway", L"N", false},
    {"uk", L"UK", false},         {"united_kingdom", L"UK", false}, {"turkey", L"TR", false},  {"russia", L"RUS", false},
    {"serbia", L"SRB", false},    {"bosnia", L"BIH", false},    {"montenegro", L"MNE", false}, {"albania", L"AL", false},
    {"macedonia", L"NMK", false}, {"north_macedonia", L"NMK", false}, {"kosovo", L"RKS", false}, {"iceland", L"IS", false},
};

const CountryInfo* FindCountry(const std::string& id) {
    for (const CountryInfo& c : kCountries) if (id == c.id) return &c;
    return nullptr;
}

HFONT Font(int px, int weight, const wchar_t* face) {
    return CreateFontW(-px, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                       ANTIALIASED_QUALITY, DEFAULT_PITCH, face);
}

void Star(HDC dc, double cx, double cy, double r) {
    POINT p[10];
    for (int i = 0; i < 10; ++i) {
        const double a = -3.14159265 / 2 + i * 3.14159265 / 5, rr = (i % 2) ? r * 0.42 : r;
        p[i].x = static_cast<LONG>(std::lround(cx + rr * std::cos(a)));
        p[i].y = static_cast<LONG>(std::lround(cy + rr * std::sin(a)));
    }
    Polygon(dc, p, 10);
}

}  // namespace

std::wstring PlateText(const std::string& raw) {
    std::string s = raw.substr(0, raw.find('|'));
    std::string out;
    bool inTag = false;
    for (char ch : s) {
        if (ch == '<') { inTag = true; continue; }
        if (ch == '>') { inTag = false; out += ' '; continue; }
        if (!inTag) out += ch;
    }
    // collapse spaces, trim
    std::string t;
    for (char ch : out) { if (ch == ' ' && (t.empty() || t.back() == ' ')) continue; t += ch; }
    while (!t.empty() && t.back() == ' ') t.pop_back();
    return ToWide(t);
}

std::vector<uint8_t> PlateDraw(unsigned w, unsigned h, const std::wstring& text, const std::string& countryId) {
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
    if (!bmp || !bits) { DeleteDC(dc); return out; }
    HGDIOBJ oldBmp = SelectObject(dc, bmp);

    const CountryInfo* ci = FindCountry(countryId);
    const bool uk = ci && wcscmp(ci->code, L"UK") == 0;
    const COLORREF bg = uk ? RGB(0xF7, 0xC8, 0x00) : RGB(0xF4, 0xF4, 0xF2);   // UK rear plates are yellow
    const int W = static_cast<int>(w), H = static_cast<int>(h);

    // dark plate holder, plate with a thin black rim
    RECT all{0, 0, W, H};
    HBRUSH b = CreateSolidBrush(RGB(0x14, 0x14, 0x14)); FillRect(dc, &all, b); DeleteObject(b);
    const int m = H / 30, rim = H / 32;
    HBRUSH plateB = CreateSolidBrush(bg); HPEN rimP = CreatePen(PS_SOLID, rim, RGB(0x10, 0x10, 0x10));
    HGDIOBJ ob = SelectObject(dc, plateB), op = SelectObject(dc, rimP);
    RoundRect(dc, m, m, W - m, H - m, H / 6, H / 6);
    SelectObject(dc, ob); SelectObject(dc, op); DeleteObject(plateB); DeleteObject(rimP);

    // blue band with the country code (EU: circle of stars)
    int textLeft = m + rim + H / 10;
    if (ci) {
        const int bw = static_cast<int>(H * 0.40);
        RECT band{m + rim, m + rim, m + rim + bw, H - m - rim};
        HBRUSH blue = CreateSolidBrush(RGB(0x00, 0x33, 0x99)); FillRect(dc, &band, blue); DeleteObject(blue);
        const double cx = (band.left + band.right) / 2.0;
        if (ci->eu) {
            HBRUSH yel = CreateSolidBrush(RGB(0xFF, 0xCC, 0x00)); HPEN np = CreatePen(PS_NULL, 0, 0);
            HGDIOBJ yb = SelectObject(dc, yel), yp = SelectObject(dc, np);
            const double cy = H * 0.33, rr = H * 0.15;
            for (int i = 0; i < 12; ++i) Star(dc, cx + rr * std::cos(i * 3.14159265 / 6), cy + rr * std::sin(i * 3.14159265 / 6), H * 0.032);
            SelectObject(dc, yb); SelectObject(dc, yp); DeleteObject(yel); DeleteObject(np);
        }
        HFONT cf = Font(static_cast<int>(H * (wcslen(ci->code) > 2 ? 0.20 : 0.27)), FW_BOLD, L"Bahnschrift");
        HGDIOBJ of = SelectObject(dc, cf);
        SetBkMode(dc, TRANSPARENT); SetTextColor(dc, RGB(255, 255, 255));
        RECT cr{band.left, static_cast<LONG>(H * (ci->eu ? 0.55 : 0.30)), band.right, H - m - rim - H / 20};
        DrawTextW(dc, ci->code, -1, &cr, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        SelectObject(dc, of); DeleteObject(cf);
        textLeft = band.right + H / 12;
    }

    // plate number: condensed DIN-like font, as large as it fits
    const int textRight = W - m - rim - H / 10;
    int px = static_cast<int>(H * 0.74);
    HFONT tf = nullptr;
    SIZE sz{};
    for (; px > 20; px -= 2) {
        if (tf) DeleteObject(tf);
        tf = Font(px, FW_SEMIBOLD, L"Bahnschrift SemiBold Condensed");
        SelectObject(dc, tf);
        GetTextExtentPoint32W(dc, text.c_str(), static_cast<int>(text.size()), &sz);
        if (sz.cx <= textRight - textLeft) break;
    }
    SetBkMode(dc, TRANSPARENT); SetTextColor(dc, RGB(0x12, 0x12, 0x12));
    RECT tr{textLeft, m + rim, textRight, H - m - rim};
    DrawTextW(dc, text.c_str(), -1, &tr, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(dc, GetStockObject(SYSTEM_FONT));
    if (tf) DeleteObject(tf);
    GdiFlush();

    const uint8_t* src = static_cast<const uint8_t*>(bits);
    for (size_t i = 0; i < out.size(); i += 4) { out[i] = src[i]; out[i + 1] = src[i + 1]; out[i + 2] = src[i + 2]; out[i + 3] = 255; }
    SelectObject(dc, oldBmp);
    DeleteObject(bmp);
    DeleteDC(dc);
    return out;
}

}  // namespace ep
