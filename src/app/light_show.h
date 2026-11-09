// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include "surface/leds.h"
#include <array>
#include <cmath>
#include <cstdint>
#include <initializer_list>

// LED showcase: a looping sequence of scenes that uses every measured surface
// light. light_frame() is a pure function of the position in the loop, so the
// MIDI thread can send a frame and the screen can mirror the same frame
// without sharing any state. Pads use printed numbering: index 0 is pad 1,
// bottom left, and index 15 is pad 16, top right.
struct LightFrame {
    std::array<Rgb, surface_pad_lights> pads {};
    Rgb fader {};
    std::array<bool, surface_bank_lights> banks {};
    std::array<ButtonLight, size_t(SurfaceButton::Count)> buttons {}; // Off where a button has no light
};

// Buttons with a light, in panel order: the left side top to bottom, then the right.
constexpr std::array<SurfaceButton, 17> panel_lit_buttons {{
    SurfaceButton::Sample, SurfaceButton::Seq, SurfaceButton::PadFx, SurfaceButton::KnobFx, SurfaceButton::PadBank,
    SurfaceButton::Erase, SurfaceButton::NoteRepeat,
    SurfaceButton::Chop, SurfaceButton::Mute, SurfaceButton::Loop, SurfaceButton::SixteenLevels,
    SurfaceButton::SampleSelect, SurfaceButton::TapTempo, SurfaceButton::SampleRecord, SurfaceButton::SeqRecord,
    SurfaceButton::Stop, SurfaceButton::Play,
}};

// Every light once, in the order the roll call lights them: the left side top
// to bottom (bank lights beside main volume, its buttons and the fader), pads
// 1 to 16, then the right side top to bottom.
struct LightId { enum class Kind : uint8_t { Bank, Button, Fader, Pad } kind; uint8_t index; };
constexpr unsigned roll_call_count = surface_bank_lights + 17 + 1 + surface_pad_lights;
inline LightId roll_call_light(unsigned n) {
    using Kind = LightId::Kind;
    if (n < surface_bank_lights) return {Kind::Bank, uint8_t(n)};
    n -= surface_bank_lights;
    if (n < 5) return {Kind::Button, uint8_t(panel_lit_buttons[n])};          // Sample to Pad Bank
    if (n == 5) return {Kind::Fader, 0};
    if (n < 8) return {Kind::Button, uint8_t(panel_lit_buttons[n - 1])};      // Erase, Note Repeat
    n -= 8;
    if (n < surface_pad_lights) return {Kind::Pad, uint8_t(n)};
    n -= surface_pad_lights;
    return {Kind::Button, uint8_t(panel_lit_buttons[7 + (n < 10 ? n : 9)])}; // Chop to Play
}

namespace light_detail {
constexpr float pi = 3.14159265f;
inline float clamp01(float x) { return x < 0 ? 0 : x > 1 ? 1 : x; }
// LEDs are linear in drive while eyes are not; squaring the level makes fades look even.
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
inline uint32_t hash(uint32_t x) {
    x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu; x ^= x >> 16;
    return x;
}
inline int column(unsigned pad) { return int(pad % 4); }
inline int row(unsigned pad) { return int(pad / 4); } // 0 is the bottom row
inline ButtonLight button_level(float level) {
    return level > 0.66f ? ButtonLight::Bright : level > 0.2f ? ButtonLight::Dim : ButtonLight::Off;
}
inline void set_buttons(LightFrame& frame, ButtonLight light) {
    for (auto button : panel_lit_buttons) frame.buttons[size_t(button)] = light;
}
inline void set_banks(LightFrame& frame, unsigned count) {
    for (unsigned bank = 0; bank < surface_bank_lights; ++bank) frame.banks[bank] = bank < count;
}

// Each light comes on in turn at full and settles; then all of them, then dark.
inline void roll_call(LightFrame& frame, unsigned t) {
    constexpr unsigned step = 110, lit = roll_call_count * step;
    if (t >= lit + 980) return;
    bool all = t >= lit;
    for (unsigned n = 0; n < roll_call_count && (all || n * step <= t); ++n) {
        unsigned age = all ? 0 : t - n * step;
        float level = age < 300 ? 1 - 0.6f * age / 300.0f : 0.4f;
        auto light = roll_call_light(n);
        switch (light.kind) {
        case LightId::Kind::Bank: frame.banks[light.index] = true; break;
        case LightId::Kind::Button: frame.buttons[light.index] = age < 300 ? ButtonLight::Bright : ButtonLight::Dim; break;
        case LightId::Kind::Fader: frame.fader = white(level); break;
        case LightId::Kind::Pad: frame.pads[light.index] = white(level); break;
        }
    }
}
// Every button value, with the pads and fader in matching colours.
inline void button_colours(LightFrame& frame, unsigned t) {
    unsigned step = t / 800 < 3 ? t / 800 : 3;
    constexpr ButtonLight white_red[] {ButtonLight::Dim, ButtonLight::Bright, ButtonLight::RedDim, ButtonLight::RedBright};
    constexpr ButtonLight single[] {ButtonLight::Dim, ButtonLight::Bright, ButtonLight::Dim, ButtonLight::Bright};
    for (auto button : panel_lit_buttons)
        frame.buttons[size_t(button)] = surface_button_lights[size_t(button)] == ButtonLightKind::WhiteRed ? white_red[step] : single[step];
    Rgb colour = step == 0 ? white(0.35f) : step == 1 ? white() : step == 2 ? rgb(1, 0, 0, 0.35f) : rgb(1, 0, 0);
    frame.pads.fill(colour); frame.fader = colour;
    set_banks(frame, (step + 1) * 2);
}
// Pads fade in one by one, each in its own hue.
inline void sixteen_colours(LightFrame& frame, unsigned t) {
    constexpr unsigned step = 180;
    for (unsigned pad = 0; pad < surface_pad_lights; ++pad)
        if (t >= pad * step) frame.pads[pad] = hue(pad / 16.0f, (t - pad * step) / 160.0f);
    unsigned newest = t / step < 15 ? t / step : 15;
    frame.fader = hue(newest / 16.0f);
    set_banks(frame, (newest + 2) / 2);
    for (unsigned n = 0; n < panel_lit_buttons.size(); ++n)
        if (t >= n * 170) frame.buttons[size_t(panel_lit_buttons[n])] = ButtonLight::Dim;
}
// A diagonal rainbow flowing across the pads.
inline void rainbow(LightFrame& frame, unsigned t) {
    float s = t / 1000.0f;
    for (unsigned pad = 0; pad < surface_pad_lights; ++pad) frame.pads[pad] = hue((column(pad) + row(pad)) / 8.0f - s * 0.5f);
    frame.fader = hue(-s * 0.5f);
    unsigned bank = unsigned(s * 8) % surface_bank_lights;
    frame.banks[bank] = frame.banks[(bank + 1) % surface_bank_lights] = true;
    unsigned wave = unsigned(s * 10) % panel_lit_buttons.size();
    for (unsigned n = 0; n < panel_lit_buttons.size(); ++n)
        frame.buttons[size_t(panel_lit_buttons[n])] = n == wave ? ButtonLight::Bright : ButtonLight::Dim;
}
// Three slow breaths: teal, magenta, amber.
inline void breathe(LightFrame& frame, unsigned t) {
    constexpr float hues[] {0.47f, 0.83f, 0.09f};
    unsigned breath = t / 2000 < 2 ? t / 2000 : 2;
    float level = std::sin(pi * float(t % 2000) / 2000.0f);
    frame.pads.fill(hue(hues[breath], level)); frame.fader = hue(hues[breath], level);
    set_banks(frame, unsigned(std::lround(level * 8)));
    set_buttons(frame, button_level(level));
}
// Rings of blue and violet moving out from the centre of the pads.
inline void ripple(LightFrame& frame, unsigned t) {
    float s = t / 1000.0f, drift = 0.1f * std::sin(s * 0.9f);
    auto wave = [&](float distance) { return 0.5f + 0.5f * std::cos(2 * pi * (distance / 1.6f - s * 1.2f)); };
    for (unsigned pad = 0; pad < surface_pad_lights; ++pad) {
        float dx = column(pad) - 1.5f, dy = row(pad) - 1.5f, distance = std::sqrt(dx * dx + dy * dy);
        frame.pads[pad] = hue(0.55f + 0.12f * distance + drift, wave(distance));
    }
    frame.fader = hue(0.55f + drift, wave(0));
    for (unsigned bank = 0; bank < surface_bank_lights; ++bank) frame.banks[bank] = wave(std::fabs(bank - 3.5f) / 1.7f) > 0.5f;
    set_buttons(frame, ButtonLight::Dim);
}
// Random pads flash in random colours and fade over a dim blue night.
inline void sparkle(LightFrame& frame, unsigned t) {
    constexpr unsigned slot = 80, fade = 700;
    std::array<float, surface_pad_lights> brightest {};
    frame.pads.fill(rgb(0.25f, 0.25f, 1, 0.15f));
    brightest.fill(0.15f);
    std::array<unsigned, 17> button_age {}; button_age.fill(fade);
    std::array<unsigned, surface_bank_lights> bank_age {}; bank_age.fill(fade);
    unsigned now = t / slot;
    for (unsigned back = 0; back * slot <= fade && back <= now; ++back) {
        unsigned spark = now - back, age = t - spark * slot;
        uint32_t h = hash(spark + 0x5bd1e995u);
        float level = 1 - float(age) / fade;
        unsigned pad = h % surface_pad_lights;
        if (level > brightest[pad]) { brightest[pad] = level; frame.pads[pad] = hue((h >> 8) / 16777216.0f, level); }
        if (back == 0) frame.fader = hue((h >> 8) / 16777216.0f, level);
        unsigned& button = button_age[hash(spark ^ 0x27d4eb2fu) % button_age.size()];
        if (age < button) button = age;
        unsigned& bank = bank_age[hash(spark ^ 0x165667b1u) % surface_bank_lights];
        if (age < bank) bank = age;
    }
    for (unsigned n = 0; n < panel_lit_buttons.size(); ++n)
        frame.buttons[size_t(panel_lit_buttons[n])] = button_age[n] < 160 ? ButtonLight::Bright : button_age[n] < 480 ? ButtonLight::Dim : ButtonLight::Off;
    for (unsigned bank = 0; bank < surface_bank_lights; ++bank) frame.banks[bank] = bank_age[bank] < 240;
}
// A comet with a fading tail runs round the edge of the pads, a new colour
// every lap; the bank lights scan back and forth and the buttons chase.
inline void comet(LightFrame& frame, unsigned t) {
    constexpr std::array<uint8_t, 12> edge {{0, 1, 2, 3, 7, 11, 15, 14, 13, 12, 8, 4}};
    constexpr unsigned step = 75;
    unsigned head = t / step;
    float colour = float(head / edge.size()) * 0.17f;
    for (unsigned pad : {5u, 6u, 9u, 10u}) frame.pads[pad] = hue(colour + 0.5f, 0.3f + 0.15f * std::sin(2 * pi * t / 1000.0f));
    for (unsigned tail = 0; tail < 5 && tail <= head; ++tail)
        frame.pads[edge[(head - tail) % edge.size()]] = hue(colour + tail * 0.03f, 1 - tail / 5.0f);
    frame.fader = hue(colour);
    unsigned phase = (t / 100) % 14;
    frame.banks[phase < 8 ? phase : 14 - phase] = true;
    unsigned chase = (t / 90) % panel_lit_buttons.size();
    frame.buttons[size_t(panel_lit_buttons[chase])] = ButtonLight::Bright;
    frame.buttons[size_t(panel_lit_buttons[(chase + panel_lit_buttons.size() - 1) % panel_lit_buttons.size()])] = ButtonLight::Dim;
}
// Pad columns bounce like a level meter, green to yellow to red; Tap Tempo
// keeps a 120 BPM beat as the stock app does.
inline void levels(LightFrame& frame, unsigned t) {
    constexpr float rates[] {1.3f, 1.7f, 1.1f, 2.1f}, offsets[] {0, 0.3f, 0.6f, 0.15f}, row_hues[] {0.33f, 0.33f, 0.15f, 0};
    float s = t / 1000.0f, loudest = 0, total = 0;
    for (unsigned col = 0; col < 4; ++col) {
        float level = std::fabs(std::sin(pi * (s * rates[col] + offsets[col])));
        loudest = level > loudest ? level : loudest; total += level;
        for (unsigned r = 0; r < 4; ++r) {
            float fill = clamp01(level * 4 - r);
            if (fill > 0) frame.pads[r * 4 + col] = hue(row_hues[r], fill);
        }
    }
    frame.fader = hue(0.33f * (1 - loudest));
    set_banks(frame, unsigned(std::lround(total / 4 * 8)));
    frame.buttons[size_t(SurfaceButton::TapTempo)] = (t / 250) % 2 ? ButtonLight::Dim : ButtonLight::Bright;
    frame.buttons[size_t(SurfaceButton::Play)] = ButtonLight::Bright;
}
// Four flashes on the beat, a last rainbow fading out, then dark.
inline void finale(LightFrame& frame, unsigned t) {
    if (t < 1500) {
        unsigned beat = t / 375;
        float level = 1 - float(t % 375) / 375.0f;
        Rgb colour = beat == 1 ? hue(0, level) : beat == 2 ? hue(0.62f, level) : white(level);
        frame.pads.fill(colour); frame.fader = colour;
        set_banks(frame, level > 0.4f ? surface_bank_lights : 0);
        set_buttons(frame, level > 0.5f ? ButtonLight::Bright : ButtonLight::Dim);
        return;
    }
    if (t >= 3000) return;
    float s = (t - 1500) / 1000.0f, level = 1 - s / 1.5f;
    for (unsigned pad = 0; pad < surface_pad_lights; ++pad) frame.pads[pad] = hue(pad / 16.0f + s * 0.6f, level);
    frame.fader = white(level);
    set_banks(frame, unsigned(std::ceil(level * 8)));
    set_buttons(frame, button_level(level));
}
} // namespace light_detail

struct LightScene { const char* name; const char* detail; unsigned length_ms; void (*draw)(LightFrame&, unsigned); };
constexpr std::array<LightScene, 10> light_scenes {{
    {"Roll call", "Every light, one by one", 6000, light_detail::roll_call},
    {"Button colours", "Dim, bright, dim red, red", 3200, light_detail::button_colours},
    {"Sixteen colours", "A colour for every pad", 4200, light_detail::sixteen_colours},
    {"Rainbow", "Colours flow across the pads", 6000, light_detail::rainbow},
    {"Breathe", "Slow fades in three colours", 6000, light_detail::breathe},
    {"Ripple", "Waves from the centre", 5000, light_detail::ripple},
    {"Sparkle", "Pads flash and fade", 5000, light_detail::sparkle},
    {"Comet", "A tail runs round the edge", 5000, light_detail::comet},
    {"Levels", "Columns bounce like a meter", 5000, light_detail::levels},
    {"Finale", "Flashes, then darkness", 3500, light_detail::finale},
}};
constexpr unsigned light_scene_start(unsigned scene) {
    unsigned start = 0;
    for (unsigned n = 0; n < scene && n < light_scenes.size(); ++n) start += light_scenes[n].length_ms;
    return start;
}
constexpr unsigned light_show_length() { return light_scene_start(unsigned(light_scenes.size())); }
// Scene playing at `ms` into the show; positions past the end wrap round.
inline unsigned light_scene_at(unsigned ms) {
    ms %= light_show_length();
    unsigned scene = 0;
    while (ms >= light_scenes[scene].length_ms) ms -= light_scenes[scene++].length_ms;
    return scene;
}
inline LightFrame light_frame(unsigned ms) {
    LightFrame frame;
    ms %= light_show_length();
    unsigned scene = light_scene_at(ms);
    light_scenes[scene].draw(frame, ms - light_scene_start(scene));
    return frame;
}
inline Rgb mix_light(Rgb from, Rgb to, float amount) {
    amount = light_detail::clamp01(amount);
    auto channel = [&](uint8_t a, uint8_t b) { return uint8_t(std::lround(a + (int(b) - int(a)) * amount)); };
    return {channel(from.r, to.r), channel(from.g, to.g), channel(from.b, to.b)};
}
// Requests every light in the frame; SurfaceLights sends only what changed.
inline void show_light_frame(SurfaceLights& lights, const LightFrame& frame) {
    for (unsigned pad = 0; pad < surface_pad_lights; ++pad) lights.pad(pad, frame.pads[pad]);
    lights.fader(frame.fader);
    for (unsigned bank = 0; bank < surface_bank_lights; ++bank) lights.bank(bank, frame.banks[bank]);
    for (auto button : panel_lit_buttons) lights.button(button, frame.buttons[size_t(button)]);
}
