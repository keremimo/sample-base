// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include "surface/leds.h"
#include <cmath>
#include <cstdint>

// Colours for the surface's RGB lights (pads and fader) that look even to
// the eye. The LEDs are linear in drive while eyes are not, so levels are
// squared before they reach the 0-127 drive.
namespace led_colour {
inline float clamp01(float x) { return x < 0 ? 0 : x > 1 ? 1 : x; }
// Red, green and blue 0..1 at `level` 0..1.
inline Rgb rgb(float r, float g, float b, float level = 1) {
    float drive = clamp01(level); drive *= drive;
    auto channel = [&](float x) { return uint8_t(std::lround(clamp01(x) * drive * 127)); };
    return {channel(r), channel(g), channel(b)};
}
inline Rgb white(float level = 1) { return rgb(1, 1, 1, level); }
// Fully saturated colour; `h` wraps, 0 red, 1/3 green, 2/3 blue.
inline Rgb hue(float h, float level = 1) {
    h -= std::floor(h);
    float x = h * 6, f = x - std::floor(x);
    switch (int(x) % 6) {
    case 0: return rgb(1, f, 0, level);
    case 1: return rgb(1 - f, 1, 0, level);
    case 2: return rgb(0, 1, f, level);
    case 3: return rgb(0, 1 - f, 1, level);
    case 4: return rgb(f, 0, 1, level);
    default: return rgb(1, 0, 1 - f, level);
    }
}
} // namespace led_colour

// `from` moved `amount` 0..1 of the way to `to`, in drive values.
inline Rgb mix_light(Rgb from, Rgb to, float amount) {
    amount = led_colour::clamp01(amount);
    auto channel = [&](uint8_t a, uint8_t b) { return uint8_t(std::lround(a + (int(b) - int(a)) * amount)); };
    return {channel(from.r, to.r), channel(from.g, to.g), channel(from.b, to.b)};
}
