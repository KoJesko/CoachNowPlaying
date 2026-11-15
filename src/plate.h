#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace ep {

// The game only draws the number plate at the bus's own plate point (cabin). For the rear luggage box the plugin draws
// a plate from the telemetry: plate text (markup removed) and country id (e.g. "germany").

// Plate text from the telemetry value: tags like <offset ...> removed, front part of "front|rear" values, trimmed.
std::wstring PlateText(const std::string& raw);

// Draws the plate (top-down BGRA, opaque), w x h (about 4.7 : 1).
std::vector<uint8_t> PlateDraw(unsigned w, unsigned h, const std::wstring& text, const std::string& countryId);

}  // namespace ep
