// SPDX-License-Identifier: AGPL-3.0-only
#include "leds.h"
#include <cassert>
#include <vector>

static std::vector<uint8_t> bytes(const MidiPacket& packet) {
    return std::vector<uint8_t>(packet.bytes.begin(), packet.bytes.begin() + packet.size);
}

int main() {
    // Measured message shapes: pad index is the printed pad number minus one,
    // index 16 is the fader light, and colours are 7-bit.
    assert(bytes(surface_rgb_packet(0, {127, 0, 0})) ==
           std::vector<uint8_t>({0xf0,0x47,0x7f,0x5c,0x65,0x00,0x04,0x00,0x7f,0x00,0x00,0xf7}));
    assert(bytes(surface_rgb_packet(surface_fader_light, {200, 101, 1})) ==
           std::vector<uint8_t>({0xf0,0x47,0x7f,0x5c,0x65,0x00,0x04,0x10,0x7f,0x65,0x01,0xf7}));
    assert(bytes(surface_cc_packet(81, 2)) == std::vector<uint8_t>({0xb0,81,2}));

    // Which buttons light, and in which colours.
    unsigned lit = 0, red = 0;
    for (unsigned n = 0; n < surface_buttons.size(); ++n) {
        lit += button_light_supported(SurfaceButton(n), ButtonLight::Dim);
        red += button_light_supported(SurfaceButton(n), ButtonLight::RedBright);
    }
    assert(lit == 17 && red == 11);
    for (auto button : {SurfaceButton::Shift, SurfaceButton::Minus, SurfaceButton::Plus, SurfaceButton::TopLeft,
                        SurfaceButton::TopMiddle, SurfaceButton::TopRight, SurfaceButton::WheelPush})
        assert(!button_light_supported(button, ButtonLight::Dim) && !button_light_supported(button, ButtonLight::Off));
    for (auto button : {SurfaceButton::PadBank, SurfaceButton::NoteRepeat, SurfaceButton::Stop, SurfaceButton::Play,
                        SurfaceButton::SampleRecord, SurfaceButton::SeqRecord})
        assert(button_light_supported(button, ButtonLight::Bright) && !button_light_supported(button, ButtonLight::RedDim));
    assert(button_light_supported(SurfaceButton::TapTempo, ButtonLight::RedDim));

    SurfaceLights lights;
    std::vector<std::vector<uint8_t>> wire;
    bool accept = true;
    auto send = [&](const MidiPacket& packet) { if (!accept) return false; wire.push_back(bytes(packet)); return true; };

    // Nothing is sent until the application sets a light.
    assert(lights.update(send, 64) == 0 && wire.empty());
    assert(!lights.button(SurfaceButton::Shift, ButtonLight::Dim));
    assert(!lights.button(SurfaceButton::Stop, ButtonLight::RedBright));
    assert(!lights.pad(16, {1, 2, 3}) && !lights.bank(8, true));
    assert(lights.button(SurfaceButton::Stop, ButtonLight::Bright));
    assert(lights.bank(0, true) && lights.bank(7, true) && lights.bank(7, false));
    assert(lights.pad(15, {127, 127, 127}) && lights.fader({101, 101, 101}));
    assert(lights.update(send, 64) == 5);
    assert(wire[0] == std::vector<uint8_t>({0xb0,81,2}));
    assert(wire[1] == std::vector<uint8_t>({0xb0,57,127}) && wire[2] == std::vector<uint8_t>({0xb0,64,0}));
    assert(wire[3][7] == 15 && wire[3][8] == 127 && wire[4][7] == 16 && wire[4][9] == 101);

    // Unchanged lights are not resent; only the last requested state is.
    wire.clear();
    assert(lights.update(send, 64) == 0);
    lights.button(SurfaceButton::Stop, ButtonLight::Dim);
    lights.button(SurfaceButton::Stop, ButtonLight::Bright);
    assert(lights.update(send, 64) == 0 && wire.empty());

    // A limit or a full queue leaves the rest for the next update.
    lights.all_off();
    assert(lights.update(send, 10) == 10);
    accept = false;
    assert(lights.update(send, 64) == 0 && wire.size() == 10);
    accept = true;
    // 17 buttons, 7 banks (H is already off) and 17 RGB lights changed.
    assert(lights.update(send, 64) == 17 + 7 + 17 - 10);
    assert(lights.update(send, 64) == 0);
}
