#pragma once
#include "announce_text.h"

#include <string>

namespace ep {

// Texts of the passenger TV in one language.
struct TvText {
    bool english = false;   // the language is English (no second English line needed)
    std::wstring destination, welcome, arrival /* {T} = time, {K} = km */, nextStop, target, belongings, goodTrip, seatBelt,
        wifi, days[7] /* Monday first */;
};
TvText GetTvText(const AnnounceLang& lang);  // unknown language -> English

}  // namespace ep
