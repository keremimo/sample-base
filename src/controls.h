// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include "midi.h"
#include <array>
#include <cstdint>
#include <string>

// Non-pad surface controls, measured on AC50 1.3.0.12 with
// tools/capture_controls.py. Every control uses MIDI channel 1. Buttons send
// note-on velocity 127 when pressed and note-off velocity 0 when released.
// Main volume, K1-K3 and the fader are absolute CCs from 0 to 127. The wheel
// is a relative CC: 1 is one step right and 127 is one step left. Pads use
// channel 10, so a channel-1 note with a pad's number is a button.
// Main volume changes no ALSA mixer control; applications apply it.
constexpr uint8_t surface_control_channel = 0;

enum class SurfaceButton : uint8_t {
    Sample, Seq, PadFx, KnobFx, Shift, PadBank, Erase, NoteRepeat,
    TopLeft, TopMiddle, TopRight,
    Chop, Mute, Loop, SixteenLevels, SampleSelect, TapTempo, Minus, Plus,
    SampleRecord, SeqRecord, Stop, Play, WheelPush, Count
};
enum class SurfaceControl : uint8_t { MainVolume, K1, K2, K3, Fader, Wheel, Count };

struct SurfaceControlInfo { uint8_t number; const char* name; };
constexpr std::array<SurfaceControlInfo, size_t(SurfaceButton::Count)> surface_buttons {{
    {6, "Sample"}, {7, "SEQ"}, {1, "Pad FX"}, {0, "Knob FX"}, {49, "Shift"}, {35, "Pad Bank"},
    {9, "Erase"}, {11, "Note Repeat"},
    {12, "Top left"}, {13, "Top middle"}, {14, "Top right"},
    {75, "Chop"}, {43, "Mute"}, {74, "Loop"}, {40, "16 Levels"}, {36, "Sample Select"},
    {53, "Tap Tempo"}, {55, "Minus"}, {54, "Plus"},
    {73, "Sample Record"}, {80, "Seq Record"}, {81, "Stop"}, {82, "Play"}, {111, "Wheel push"},
}};
constexpr std::array<SurfaceControlInfo, size_t(SurfaceControl::Count)> surface_controls {{
    {19, "Main volume"}, {16, "K1"}, {17, "K2"}, {18, "K3"}, {20, "Fader"}, {100, "Wheel"},
}};

struct SurfaceInput {
    enum class Kind : uint8_t { None, Button, Control } kind = Kind::None;
    uint8_t index = 0;   // SurfaceButton or SurfaceControl
    bool pressed = false; // Button state
    uint8_t value = 0;    // Absolute control position, 0..127
    int delta = 0;        // Wheel steps; positive is clockwise
};

inline SurfaceInput surface_input(MidiEvent event) {
    SurfaceInput input;
    if ((event.status & 0x0f) != surface_control_channel) return input;
    unsigned kind = event.status & 0xf0;
    if (kind == 0x80 || kind == 0x90) {
        for (unsigned n = 0; n < surface_buttons.size(); ++n)
            if (surface_buttons[n].number == event.key) {
                input.kind = SurfaceInput::Kind::Button; input.index = uint8_t(n);
                input.pressed = kind == 0x90 && event.value != 0;
                return input;
            }
    } else if (kind == 0xb0) {
        for (unsigned n = 0; n < surface_controls.size(); ++n)
            if (surface_controls[n].number == event.key) {
                input.kind = SurfaceInput::Kind::Control; input.index = uint8_t(n);
                if (n == unsigned(SurfaceControl::Wheel)) input.delta = event.value < 64 ? int(event.value) : int(event.value) - 128;
                else input.value = event.value;
                return input;
            }
    }
    return input;
}
inline bool is_button(const SurfaceInput& input, SurfaceButton button) {
    return input.kind == SurfaceInput::Kind::Button && input.index == uint8_t(button);
}
inline bool is_control(const SurfaceInput& input, SurfaceControl control) {
    return input.kind == SurfaceInput::Kind::Control && input.index == uint8_t(control);
}
// Position request, measured from the stock app's startup traffic: inMusic
// SysEx command 0x42 for product 0x5C with an empty payload. The surface
// answers at once with absolute CCs for main volume, K1-K3 and the fader at
// their current physical positions, followed by wheel CC 100 value 0.
constexpr std::array<uint8_t, 8> surface_position_request {0xf0, 0x47, 0x7f, 0x5c, 0x42, 0x00, 0x00, 0xf7};
inline MidiPacket surface_position_packet() {
    MidiPacket packet; packet.size = surface_position_request.size();
    for (unsigned n = 0; n < packet.size; ++n) packet.bytes[n] = surface_position_request[n];
    return packet;
}

// Most recent input, packed for lock-free hand-off to the screen thread;
// 0 means none. The power button is not MIDI; see power_button.h.
enum class InputKind : uint8_t { None, Button, Control, Power };
constexpr uint32_t pack_last_input(InputKind kind, uint8_t index = 0, uint8_t value = 0) {
    return uint32_t(kind) << 16 | uint32_t(index) << 8 | value;
}
inline uint32_t pack_last_input(const SurfaceInput& input) {
    if (input.kind == SurfaceInput::Kind::Button) return pack_last_input(InputKind::Button, input.index, input.pressed);
    if (input.kind != SurfaceInput::Kind::Control) return 0;
    bool wheel = input.index == uint8_t(SurfaceControl::Wheel);
    return pack_last_input(InputKind::Control, input.index, wheel ? uint8_t(int8_t(input.delta)) : input.value);
}
inline std::string describe_last_input(uint32_t packed) {
    auto kind = InputKind((packed >> 16) & 0xff);
    unsigned index = (packed >> 8) & 0xff, value = packed & 0xff;
    if (kind == InputKind::Power) return "Power";
    if (kind == InputKind::Button && index < surface_buttons.size()) return surface_buttons[index].name;
    if (kind != InputKind::Control || index >= surface_controls.size()) return {};
    std::string name = surface_controls[index].name;
    if (index != unsigned(SurfaceControl::Wheel)) return name + ' ' + std::to_string(value);
    int delta = int8_t(uint8_t(value));
    return name + ' ' + (delta > 0 ? "+" : "") + std::to_string(delta);
}
// Software output gain for the main volume knob: a squared taper gives finer
// control near silence than a linear map of the 0..127 position.
inline float main_volume_gain(uint8_t value) {
    float position = (value > 127 ? 127 : value) / 127.0f;
    return position * position;
}
