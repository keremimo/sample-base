// SPDX-License-Identifier: AGPL-3.0-only
#include "app/light_show.h"
#include <cassert>
#include <set>
#include <tuple>
#include <vector>

static bool dark(const LightFrame& frame) {
    for (auto pad : frame.pads) if (pad.r || pad.g || pad.b) return false;
    for (auto bank : frame.banks) if (bank) return false;
    for (auto button : frame.buttons) if (button != ButtonLight::Off) return false;
    return !frame.fader.r && !frame.fader.g && !frame.fader.b;
}
static bool on(Rgb colour) { return colour.r || colour.g || colour.b; }
static unsigned lit_count(const LightFrame& frame) {
    unsigned count = 0;
    for (auto pad : frame.pads) count += on(pad);
    for (auto bank : frame.banks) count += bank;
    for (auto button : frame.buttons) count += button != ButtonLight::Off;
    return count + on(frame.fader);
}

int main() {
    // Scene timing: ten scenes, 48.9 s, wrapping round.
    assert(light_scenes.size() == 10 && light_show_length() == 48900);
    assert(light_scene_start(0) == 0 && light_scene_start(1) == 6000 && light_scene_start(10) == 48900);
    for (unsigned scene = 0; scene < light_scenes.size(); ++scene) {
        assert(light_scene_at(light_scene_start(scene)) == scene);
        assert(light_scene_at(light_scene_start(scene) + light_scenes[scene].length_ms - 1) == scene);
    }
    assert(light_scene_at(light_show_length()) == 0 && light_scene_at(light_show_length() + 6000) == 1);

    // The panel order holds exactly the buttons that have a light.
    std::set<SurfaceButton> lit_buttons(panel_lit_buttons.begin(), panel_lit_buttons.end());
    assert(lit_buttons.size() == panel_lit_buttons.size());
    for (unsigned n = 0; n < size_t(SurfaceButton::Count); ++n)
        assert(lit_buttons.count(SurfaceButton(n)) == (surface_button_lights[n] != ButtonLightKind::None));

    // The roll call names every light once: banks A-H first, Play last.
    std::set<std::pair<int, int>> called;
    for (unsigned n = 0; n < roll_call_count; ++n) called.insert({int(roll_call_light(n).kind), roll_call_light(n).index});
    assert(roll_call_count == 42 && called.size() == 42);
    assert(roll_call_light(0).kind == LightId::Kind::Bank && roll_call_light(0).index == 0);
    assert(roll_call_light(13).kind == LightId::Kind::Fader);
    assert(roll_call_light(16).kind == LightId::Kind::Pad && roll_call_light(16).index == 0);
    assert(roll_call_light(41).kind == LightId::Kind::Button && roll_call_light(41).index == uint8_t(SurfaceButton::Play));

    // Roll call: one light more every 110 ms, then all of them, then dark.
    assert(lit_count(light_frame(5)) == 1 && light_frame(5).banks[0]);
    assert(lit_count(light_frame(13 * 110 + 5)) == 14 && on(light_frame(13 * 110 + 5).fader));
    assert(lit_count(light_frame(42 * 110 + 5)) == 42);
    assert(light_frame(42 * 110 + 5).buttons[size_t(SurfaceButton::Play)] == ButtonLight::Bright);
    assert(dark(light_frame(5700)) && dark(light_frame(light_show_length() - 1)));

    // Button colours: red only on white/red buttons; single-colour ones go dim and bright.
    auto red = light_frame(light_scene_start(1) + 2500);
    assert(red.buttons[size_t(SurfaceButton::Sample)] == ButtonLight::RedBright);
    assert(red.buttons[size_t(SurfaceButton::Play)] == ButtonLight::Bright && red.pads[0].r == 127 && !red.pads[0].g);

    // Sixteen colours: by the end every pad has its own colour.
    auto sixteen = light_frame(light_scene_start(3) - 1);
    std::set<std::tuple<int, int, int>> hues;
    for (auto pad : sixteen.pads) hues.insert({pad.r, pad.g, pad.b});
    assert(hues.size() == 16);

    // Breathe: pads fade through many levels, out to dark and back.
    std::set<int> levels;
    bool faded_out = false;
    for (unsigned t = 0; t < 2000; t += 10) {
        auto pad = light_frame(light_scene_start(4) + t).pads[0];
        levels.insert(pad.r + pad.g + pad.b); faded_out |= !on(pad);
    }
    assert(levels.size() >= 30 && faded_out);

    // Every frame, at the MIDI thread's 25 ms frame rate: only values the
    // hardware accepts, every light used, each pad in many colours, and a
    // whole frame fits one 48-message update.
    std::array<std::set<std::tuple<int, int, int>>, surface_pad_lights> pad_colours;
    std::array<bool, surface_bank_lights> bank_used {};
    std::array<bool, size_t(SurfaceButton::Count)> button_used {};
    bool fader_used = false;
    SurfaceLights lights;
    unsigned largest = 0;
    auto send = [](const MidiPacket&) { return true; };
    for (unsigned t = 0; t < light_show_length(); t += 25) {
        auto frame = light_frame(t), again = light_frame(t + light_show_length()); // Deterministic and looping.
        for (unsigned pad = 0; pad < surface_pad_lights; ++pad) {
            auto colour = frame.pads[pad];
            assert(colour.r == again.pads[pad].r && colour.g == again.pads[pad].g && colour.b == again.pads[pad].b);
            assert(colour.r <= 127 && colour.g <= 127 && colour.b <= 127);
            pad_colours[pad].insert({colour.r, colour.g, colour.b});
        }
        for (unsigned n = 0; n < frame.buttons.size(); ++n) {
            auto light = frame.buttons[n];
            if (surface_button_lights[n] == ButtonLightKind::None) assert(light == ButtonLight::Off);
            else assert(light == ButtonLight::Off || button_light_supported(SurfaceButton(n), light));
            button_used[n] |= light != ButtonLight::Off;
        }
        for (unsigned bank = 0; bank < surface_bank_lights; ++bank) bank_used[bank] |= frame.banks[bank];
        fader_used |= on(frame.fader);
        show_light_frame(lights, frame);
        unsigned sent = lights.update(send, 48);
        assert(lights.update(send, 48) == 0);
        largest = sent > largest ? sent : largest;
    }
    for (const auto& colours : pad_colours) assert(colours.size() >= 100);
    for (auto used : bank_used) assert(used);
    for (unsigned n = 0; n < button_used.size(); ++n) assert(button_used[n] == (surface_button_lights[n] != ButtonLightKind::None));
    assert(fader_used && largest <= roll_call_count);

    // A pad touch blends towards white.
    auto touch = mix_light({0, 0, 127}, {127, 127, 127}, 0.5f);
    assert(touch.r == 64 && touch.g == 64 && touch.b == 127);
    assert(mix_light({1, 2, 3}, {127, 127, 127}, 0).r == 1 && mix_light({1, 2, 3}, {127, 127, 127}, 2).g == 127);
}
