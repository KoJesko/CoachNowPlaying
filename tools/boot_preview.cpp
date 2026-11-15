// Offline preview of the boot animation: writes BMP frames (not part of the plugin).
#include "boot_anim.h"
#include <windows.h>
#include <cstdio>
#include <string>
int main(int argc, char** argv) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const char* dir = argc > 1 ? argv[1] : ".";
    const unsigned w = 640, h = 360;
    const float times[] = {0.15f, 0.40f, 0.70f, 0.95f, 1.20f, 1.55f, 1.90f, 2.15f, 2.30f, 2.45f, 2.60f, 3.00f, 3.60f, 3.80f};
    for (float t : times) {
        std::vector<uint8_t> px = ep::BootDraw(w, h, t);
        // composite over a grey "map" so the fade at the end is visible
        BITMAPFILEHEADER fh{}; BITMAPINFOHEADER ih{};
        ih.biSize = sizeof(ih); ih.biWidth = w; ih.biHeight = -static_cast<LONG>(h); ih.biPlanes = 1; ih.biBitCount = 24;
        const unsigned stride = (w * 3 + 3) & ~3u;
        std::vector<uint8_t> rgb(static_cast<size_t>(stride) * h, 0);
        for (unsigned y = 0; y < h; ++y) for (unsigned x = 0; x < w; ++x) {
            const uint8_t* s = &px[(static_cast<size_t>(y) * w + x) * 4];
            const float a = s[3] / 255.0f; const uint8_t map = ((x / 40 + y / 40) & 1) ? 90 : 70;
            for (int c = 0; c < 3; ++c) rgb[static_cast<size_t>(y) * stride + x * 3 + c] = static_cast<uint8_t>(s[c] * a + map * (1 - a));
        }
        fh.bfType = 0x4D42; fh.bfOffBits = sizeof(fh) + sizeof(ih); fh.bfSize = fh.bfOffBits + static_cast<DWORD>(rgb.size());
        char name[512]; snprintf(name, sizeof(name), "%s\\boot_%04d.bmp", dir, static_cast<int>(t * 1000));
        FILE* f = fopen(name, "wb"); if (!f) return 1;
        fwrite(&fh, sizeof(fh), 1, f); fwrite(&ih, sizeof(ih), 1, f); fwrite(rgb.data(), 1, rgb.size(), f); fclose(f);
    }
    return 0;
}