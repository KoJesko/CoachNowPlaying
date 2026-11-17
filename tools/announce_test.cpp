// Prints, for every ETS2 game language, the announcement text and the Piper voice the plugin would use:
//   announce_test.exe <CoachNowPlaying_piper dir> <out.txt>   -> lines "code<TAB>voice file<TAB>text" (UTF-8)
#include "../src/announce_text.h"
#include "../src/text_util.h"

#include <cstdio>

int wmain(int argc, wchar_t** argv) {
    if (argc < 3) return 1;
    const wchar_t* codes[] = {L"bg_bg", L"ca_es", L"cs_cz", L"da_dk", L"de_de", L"el_gr", L"en_gb", L"en_us", L"es_es",
                              L"es_la", L"et_ee", L"eu_es", L"fi_fi", L"fr_fr", L"fr_ca", L"gl_es", L"hr_hr", L"hu_hu",
                              L"it_it", L"ja_jp", L"ka_ge", L"ko_kr", L"lt_lt", L"lv_lv", L"mk_mk", L"nl_be", L"nl_nl",
                              L"no_no", L"pl_pl", L"pt_br", L"pt_pt", L"ro_ro", L"ru_ru", L"sk_sk", L"sl_sl", L"sr_sp",
                              L"sv_se", L"tr_tr", L"uk_uk", L"vi_vn", L"zh_cn", L"zh_tw", L"xx_yy", L"auto,en", L"de,en,fr"};
    FILE* f = _wfopen(argv[2], L"wb");
    if (!f) return 2;
    for (const wchar_t* c : codes) {
        for (const ep::AnnounceLang& l : ep::AnnounceLanguages(c)) {
            const std::wstring v = ep::PiperVoiceFile(argv[1], l);
            const std::wstring line = std::wstring(c) + L"\t" + l.Code() + L"\t" + v.substr(v.find_last_of(L'\\') + 1) +
                                      L"\t" + ep::AnnounceText(l, l.base == L"de" ? L"Kiel" : L"Gdańsk", 20) + L"\n";
            const std::string u = ep::ToUtf8(line);
            fwrite(u.data(), 1, u.size(), f);
        }
    }
    fclose(f);
    return 0;
}
