#pragma once
#include <cstdint>
#include <vector>

namespace ep {

// Start-up animation shown on the CarPlay map page when the screen is switched on again after the black "off" page:
// four coloured dots circle, contract with a little stretch, merge in the centre and the ROADWORKS logo pops up,
// then everything fades and the map shows.

constexpr float kBootSeconds = 3.9f;

// Draws frame `t` (seconds since the start) as top-down BGRA with straight alpha, w x h.
// t < 0 or t >= kBootSeconds: fully transparent.
std::vector<uint8_t> BootDraw(unsigned w, unsigned h, float t);

}  // namespace ep
