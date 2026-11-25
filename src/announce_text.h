#pragma once
#include "common.h"

#include <string>
#include <vector>

namespace ep {

// Language of an arrival announcement: base = "de", region = "de" / "gb" / "br" ... (lower case, may be empty).
struct AnnounceLang {
    std::wstring base, region;
    std::wstring Code() const { return region.empty() ? base : base + L"_" + region; }
};

// [announce] language: comma separated list, "auto" = the game's language (config.cfg g_lang), e.g. "auto", "fr",
// "auto,en". Unknown languages fall back to English. Never empty.
std::vector<AnnounceLang> AnnounceLanguages(const std::wstring& setting);

// The announcement in that language ("Ladies and gentlemen, we will arrive in <dest> ...", wish by the hour).
std::wstring AnnounceText(const AnnounceLang& lang, const std::wstring& dest, int hour);
bool HaveAnnounceText(const std::wstring& base);

// The language set in the game (config.cfg g_lang, else g_lang_init, else Windows).
AnnounceLang CurrentGameLanguage();

// Piper voice file for the language in <dir>\voice (best region match), "" = none.
std::wstring PiperVoiceFile(const std::wstring& dir, const AnnounceLang& lang);

}  // namespace ep
