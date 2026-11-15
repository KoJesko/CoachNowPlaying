// Offline preview of the number plate drawing (not part of the plugin).
#include "plate.h"
#include <windows.h>
#include <cstdio>
static void Save(const char* name, const std::vector<uint8_t>& px, unsigned w, unsigned h) {
    BITMAPFILEHEADER fh{}; BITMAPINFOHEADER ih{};
    ih.biSize = sizeof(ih); ih.biWidth = w; ih.biHeight = -static_cast<LONG>(h); ih.biPlanes = 1; ih.biBitCount = 32;
    fh.bfType = 0x4D42; fh.bfOffBits = sizeof(fh) + sizeof(ih); fh.bfSize = fh.bfOffBits + static_cast<DWORD>(px.size());
    FILE* f = fopen(name, "wb"); if (!f) return; fwrite(&fh, sizeof(fh), 1, f); fwrite(&ih, sizeof(ih), 1, f); fwrite(px.data(), 1, px.size(), f); fclose(f);
}
int main(int argc, char** argv) {
    std::string dir = argc > 1 ? argv[1] : ".";
    Save((dir + "\\plate_de.bmp").c_str(), ep::PlateDraw(1040, 220, ep::PlateText("<offset hshift=0>DO BV 909"), "germany"), 1040, 220);
    Save((dir + "\\plate_uk.bmp").c_str(), ep::PlateDraw(1040, 220, ep::PlateText("LV19 ABC"), "uk"), 1040, 220);
    Save((dir + "\\plate_ch.bmp").c_str(), ep::PlateDraw(1040, 220, ep::PlateText("ZH 123456"), "switzerland"), 1040, 220);
    return 0;
}