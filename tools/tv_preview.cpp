// Draws the passenger TV picture for every language into BMP files (two states: on the way / arriving):
//   tv_preview.exe <out dir>  ->  <code>_drive.bmp, <code>_arrive.bmp
#include "../src/passenger.h"

#include <cstdio>

static void SaveBmp(const std::wstring& path, const std::vector<uint8_t>& px, int w, int h) {
    BITMAPFILEHEADER fh{};
    BITMAPINFOHEADER ih{};
    ih.biSize = sizeof(ih); ih.biWidth = w; ih.biHeight = -h; ih.biPlanes = 1; ih.biBitCount = 32; ih.biCompression = BI_RGB;
    fh.bfType = 0x4D42; fh.bfOffBits = sizeof(fh) + sizeof(ih); fh.bfSize = fh.bfOffBits + static_cast<DWORD>(px.size());
    FILE* f = _wfopen(path.c_str(), L"wb");
    if (!f) return;
    fwrite(&fh, sizeof(fh), 1, f); fwrite(&ih, sizeof(ih), 1, f); fwrite(px.data(), 1, px.size(), f);
    fclose(f);
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) return 1;
    const wchar_t* codes[] = {L"en_gb", L"en_us", L"de_de", L"fr_fr", L"es_es", L"es_la", L"it_it", L"pt_pt", L"pt_br", L"nl_nl",
                              L"da_dk", L"no_no", L"sv_se", L"fi_fi", L"et_ee", L"lv_lv", L"lt_lt", L"pl_pl", L"cs_cz", L"sk_sk",
                              L"hu_hu", L"ro_ro", L"sl_sl", L"hr_hr", L"sr_sp", L"bg_bg", L"mk_mk", L"ru_ru", L"uk_uk", L"el_gr",
                              L"tr_tr", L"ca_es", L"gl_es", L"eu_es", L"ka_ge", L"ko_kr", L"ja_jp", L"zh_cn", L"zh_tw", L"vi_vn"};
    const int W = 1024, H = 760;
    for (const wchar_t* c : codes) {
        std::wstring code = c;
        ep::AnnounceLang l{code.substr(0, 2), code.substr(3)};
        const ep::TvText tx = ep::GetTvText(l);
        for (int arriving = 0; arriving < 2; ++arriving) {
            ep::PaxView v;
            v.source = L"Kraków";
            v.destination = L"Mönchengladbach";
            v.progress = arriving ? 0.93f : 0.42f;
            v.arriving = arriving != 0;
            // Wednesday 21:37, ETA in 34 / 4 minutes
            const uint32_t gm = 2 * 1440 + 21 * 60 + 37;
            const auto px = ep::PaxDrawText(W, H, v, gm, true, arriving ? 240.0f : 2040.0f, arriving ? 4200.0f : 287000.0f, tx);
            SaveBmp(std::wstring(argv[1]) + L"\\" + code + (arriving ? L"_arrive.bmp" : L"_drive.bmp"), px, W, H);
        }
    }
    return 0;
}
