// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include "surface/controls.h"
#include "surface/midi.h"
#include <array>
#include <atomic>
#include <cstdint>

// Surface LEDs on AC50 1.3.0.12. The message types come from the stock app's
// surface writes; values and placement were measured with tools/probe_leds.py.
//
// Pads and the fader light: inMusic SysEx 0x65, F0 47 7F 5C 65 00 04 I R G B F7.
//   I 0-15 is the printed pad number minus one; I 16 is the fader light.
//   R, G and B are 0-127. Stock leaves the fader at 101,101,101.
// Pad bank A-H lights: channel-1 CC 57-64. 20 and 127 both looked fully on, so
//   only on (127, as stock sends) and off (0) are offered.
// Button lights: channel-1 CC numbered like the button note. Shift, Minus,
//   Plus, the three buttons above the screen and wheel Push have no light.
// Level meters right of the screen: channel-1 CC 83 the left strip, 84 the
//   right. Five LEDs each, green, green, green, yellow, red from the bottom;
//   the value is a mask, bit 0 the bottom LED (2 lit only the second, 3 the
//   bottom two). Stock also clears CC 85 and 86; nothing showed for them.
struct Rgb { uint8_t r = 0, g = 0, b = 0; };
constexpr unsigned surface_pad_lights = 16, surface_fader_light = 16, surface_rgb_lights = 17;
constexpr unsigned surface_bank_lights = 8;
constexpr uint8_t surface_bank_light_cc = 57; // A; H is 64
constexpr unsigned surface_meter_lights = 2, surface_meter_segments = 5;
constexpr uint8_t surface_meter_cc = 83; // left; right is 84
constexpr uint8_t surface_meter_all = (1u << surface_meter_segments) - 1;

enum class ButtonLightKind : uint8_t { None, White, WhiteRed, Red, Green };
// Values 1 and 2 are the button's own colour, dim and full. Only white/red
// buttons show red: 3 is dim red and 4 is red with some white showing. A
// single-colour light shows 3 and 4 at full, so they are refused there.
enum class ButtonLight : uint8_t { Off = 0, Dim = 1, Bright = 2, RedDim = 3, RedBright = 4 };

constexpr std::array<ButtonLightKind, size_t(SurfaceButton::Count)> surface_button_lights {{
    ButtonLightKind::WhiteRed, ButtonLightKind::WhiteRed,  // Sample, SEQ
    ButtonLightKind::WhiteRed, ButtonLightKind::WhiteRed,  // Pad FX, Knob FX
    ButtonLightKind::None, ButtonLightKind::White,         // Shift, Pad Bank
    ButtonLightKind::WhiteRed, ButtonLightKind::White,     // Erase, Note Repeat
    ButtonLightKind::None, ButtonLightKind::None, ButtonLightKind::None, // above the screen
    ButtonLightKind::WhiteRed, ButtonLightKind::WhiteRed,  // Chop, Mute
    ButtonLightKind::WhiteRed, ButtonLightKind::WhiteRed,  // Loop, 16 Levels
    ButtonLightKind::WhiteRed, ButtonLightKind::WhiteRed,  // Sample Select, Tap Tempo
    ButtonLightKind::None, ButtonLightKind::None,          // Minus, Plus
    ButtonLightKind::Red, ButtonLightKind::Red,            // Sample Record, Seq Record
    ButtonLightKind::White, ButtonLightKind::Green,        // Stop, Play
    ButtonLightKind::None,                                 // Wheel push
}};

inline bool button_light_supported(SurfaceButton button, ButtonLight light) {
    if (button >= SurfaceButton::Count) return false;
    auto kind = surface_button_lights[size_t(button)];
    if (kind == ButtonLightKind::None) return false;
    return light <= ButtonLight::Bright || kind == ButtonLightKind::WhiteRed;
}

inline MidiPacket surface_cc_packet(uint8_t number, uint8_t value) {
    MidiPacket packet; packet.size = 3;
    packet.bytes[0] = 0xb0 | surface_control_channel; packet.bytes[1] = number & 0x7f; packet.bytes[2] = value & 0x7f;
    return packet;
}
// index 0-15 for pads, surface_fader_light for the fader; colours above 127 clamp.
inline MidiPacket surface_rgb_packet(unsigned index, Rgb colour) {
    auto clamp = [](uint8_t value) { return uint8_t(value > 127 ? 127 : value); };
    const uint8_t bytes[] {0xf0, 0x47, 0x7f, 0x5c, 0x65, 0x00, 0x04, uint8_t(index & 0x7f),
                           clamp(colour.r), clamp(colour.g), clamp(colour.b), 0xf7};
    MidiPacket packet; packet.size = sizeof(bytes);
    for (unsigned n = 0; n < sizeof(bytes); ++n) packet.bytes[n] = bytes[n];
    return packet;
}

// Requested LED state. Any thread may request changes (last write wins); one
// thread calls update() to send what changed. Nothing is sent for a light the
// application has never set, so the stock app's last state stays untouched
// until it is replaced.
class SurfaceLights {
public:
    SurfaceLights() {
        for (auto& light : buttons) light.store(unset_light, std::memory_order_relaxed);
        for (auto& light : banks) light.store(unset_light, std::memory_order_relaxed);
        for (auto& light : meters) light.store(unset_light, std::memory_order_relaxed);
        for (auto& light : colours) light.store(unset_rgb, std::memory_order_relaxed);
        sent_buttons.fill(unset_light); sent_banks.fill(unset_light); sent_meters.fill(unset_light); sent_colours.fill(unset_rgb);
    }
    SurfaceLights(const SurfaceLights&) = delete;
    SurfaceLights& operator=(const SurfaceLights&) = delete;
    bool pad(unsigned pad, Rgb colour) { return pad < surface_pad_lights && rgb(pad, colour); }
    bool fader(Rgb colour) { return rgb(surface_fader_light, colour); }
    bool bank(unsigned bank, bool on) {
        if (bank >= surface_bank_lights) return false;
        banks[bank].store(on ? 127 : 0, std::memory_order_relaxed); return true;
    }
    // side 0 left, 1 right; `lit` a mask of surface_meter_segments bits, bit 0 the bottom LED.
    bool meter(unsigned side, uint8_t lit) {
        if (side >= surface_meter_lights || lit > surface_meter_all) return false;
        meters[side].store(lit, std::memory_order_relaxed); return true;
    }
    bool button(SurfaceButton button, ButtonLight light) {
        if (!button_light_supported(button, light)) return false;
        buttons[size_t(button)].store(uint8_t(light), std::memory_order_relaxed); return true;
    }
    void all_off() {
        for (unsigned n = 0; n < surface_rgb_lights; ++n) rgb(n, {});
        for (unsigned n = 0; n < surface_bank_lights; ++n) bank(n, false);
        for (unsigned n = 0; n < surface_meter_lights; ++n) meter(n, 0);
        for (unsigned n = 0; n < buttons.size(); ++n) button(SurfaceButton(n), ButtonLight::Off);
    }
    // Turns off every light the application has set, leaving the others
    // untouched. The runtime calls it on exit, so the next app starts dark.
    void release() {
        for (unsigned n = 0; n < surface_rgb_lights; ++n)
            if (colours[n].load(std::memory_order_relaxed) != unset_rgb) rgb(n, {});
        for (unsigned n = 0; n < surface_bank_lights; ++n)
            if (banks[n].load(std::memory_order_relaxed) != unset_light) bank(n, false);
        for (unsigned n = 0; n < surface_meter_lights; ++n)
            if (meters[n].load(std::memory_order_relaxed) != unset_light) meter(n, 0);
        for (unsigned n = 0; n < buttons.size(); ++n)
            if (buttons[n].load(std::memory_order_relaxed) != unset_light) button(SurfaceButton(n), ButtonLight::Off);
    }
    // Sends up to `limit` changed lights through `send(const MidiPacket&)`,
    // which returns false when its queue is full; the rest wait for the next
    // call. Returns the number of messages sent.
    template <class Send> unsigned update(Send&& send, unsigned limit) {
        unsigned count = 0;
        auto changed = [&](auto& wanted, auto& sent, auto unset, auto packet) {
            auto value = wanted.load(std::memory_order_relaxed);
            if (value == unset || value == sent) return true;
            if (count == limit || !send(packet(value))) return false;
            sent = value; ++count; return true;
        };
        for (unsigned n = 0; n < buttons.size(); ++n)
            if (!changed(buttons[n], sent_buttons[n], unset_light, [&](uint8_t value) { return surface_cc_packet(surface_buttons[n].number, value); })) return count;
        for (unsigned n = 0; n < surface_bank_lights; ++n)
            if (!changed(banks[n], sent_banks[n], unset_light, [&](uint8_t value) { return surface_cc_packet(uint8_t(surface_bank_light_cc + n), value); })) return count;
        for (unsigned n = 0; n < surface_meter_lights; ++n)
            if (!changed(meters[n], sent_meters[n], unset_light, [&](uint8_t value) { return surface_cc_packet(uint8_t(surface_meter_cc + n), value); })) return count;
        for (unsigned n = 0; n < surface_rgb_lights; ++n)
            if (!changed(colours[n], sent_colours[n], unset_rgb, [&](uint32_t value) {
                return surface_rgb_packet(n, {uint8_t(value >> 16), uint8_t(value >> 8), uint8_t(value)}); })) return count;
        return count;
    }
private:
    static constexpr uint8_t unset_light = 0xff;
    static constexpr uint32_t unset_rgb = 0xffffffff;
    bool rgb(unsigned index, Rgb colour) {
        auto clamp = [](uint8_t value) { return uint32_t(value > 127 ? 127 : value); };
        colours[index].store(clamp(colour.r) << 16 | clamp(colour.g) << 8 | clamp(colour.b), std::memory_order_relaxed);
        return true;
    }
    std::array<std::atomic<uint8_t>, size_t(SurfaceButton::Count)> buttons;
    std::array<std::atomic<uint8_t>, surface_bank_lights> banks;
    std::array<std::atomic<uint8_t>, surface_meter_lights> meters;
    std::array<std::atomic<uint32_t>, surface_rgb_lights> colours;
    std::array<uint8_t, size_t(SurfaceButton::Count)> sent_buttons;
    std::array<uint8_t, surface_bank_lights> sent_banks;
    std::array<uint8_t, surface_meter_lights> sent_meters;
    std::array<uint32_t, surface_rgb_lights> sent_colours;
};
