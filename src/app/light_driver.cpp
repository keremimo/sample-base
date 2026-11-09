// SPDX-License-Identifier: AGPL-3.0-only
#include "app/light_driver.h"
#include "controls.h"
#include "app/light_show.h"
#include "app/pad_touch.h"
#include <algorithm>
#include <utility>

// Closes the page that was left before drawing the open one, so the old
// page's last lights never cover the new page's first frame.
void LightDriver::update(AppState& state, Clock::time_point now) {
    auto page = state.page.load();
    if (page != Page::Lights) close_light_show(state);
    if (page != Page::Pads) close_pad_lights();
    if (page == Page::Lights) light_show(state, now);
    if (page == Page::Pads) pad_lights(state, now);
    if (!running) recorder_lights(state);
}

// While the light page is open, sends a show frame every 25 ms; a held pad
// glows white and fades back over 400 ms after release. A manual `led`
// command lasts until the next frame.
void LightDriver::light_show(AppState& state, Clock::time_point now) {
    using namespace std::chrono;
    if (!running) { running = true; show_start = next_frame = now; }
    int scene = state.scene_request.exchange(-1);
    if (scene >= 0) { show_start = now - milliseconds(light_scene_start(unsigned(scene))); next_frame = now; }
    if (now < next_frame) return;
    next_frame = now + milliseconds(25);
    auto position = unsigned(duration_cast<milliseconds>(now - show_start).count() % light_show_length());
    state.light_position.store(position);
    auto frame = light_frame(position);
    uint32_t held = state.held.load();
    for (unsigned pad = 0; pad < surface_pad_lights; ++pad) {
        if (held & (1u << pad)) touched[pad] = now;
        float glow = 1 - duration<float, std::milli>(now - touched[pad]).count() / 400.0f;
        if (glow > 0) frame.pads[pad] = mix_light(frame.pads[pad], {127, 127, 127}, glow);
    }
    show_light_frame(lights, frame);
}

// Closing the show turns every light off and hands the transport lights
// back to the recorder.
void LightDriver::close_light_show(AppState& state) {
    if (!running) return;
    running = false; lights.all_off(); shown_lights.fill(0xff);
    state.scene_request.store(-1); state.light_position.store(0);
}

// While the pad page is open, a frame every 20 ms: each pad shows how hard
// it is played (pad_touch.h) and fades out over 300 ms after release, the
// bank lights meter the hardest held pad, the fader light takes its colour
// and 16 Levels stays lit. Strikes are counted, so a tap shorter than a
// frame still flashes.
void LightDriver::pad_lights(AppState& state, Clock::time_point now) {
    using namespace std::chrono;
    if (!pads_running) {
        pads_running = true; next_pad_frame = now;
        for (unsigned pad = 0; pad < surface_pad_lights; ++pad) strikes_seen[pad] = state.pads.touch(pad).strikes;
        last_held.fill({}); force.fill(0);
        lights.button(SurfaceButton::SixteenLevels, ButtonLight::Bright);
    }
    if (now < next_pad_frame) return;
    next_pad_frame = now + milliseconds(20);
    uint32_t held = state.held.load();
    float hardest = 0;
    for (unsigned pad = 0; pad < surface_pad_lights; ++pad) {
        auto touch = state.pads.touch(pad);
        bool down = (held & (1u << pad)) != 0;
        if (touch.strikes != strikes_seen[pad]) {
            strikes_seen[pad] = touch.strikes; struck[pad] = now;
            if (!down) { force[pad] = pad_touch::held_force(touch.velocity, 0, 0); last_held[pad] = now; }
        }
        if (down) {
            auto since = unsigned(duration_cast<milliseconds>(now - struck[pad]).count());
            force[pad] = pad_touch::held_force(touch.velocity, touch.pressure, since);
            last_held[pad] = now; hardest = std::max(hardest, force[pad]);
        }
        float fade = 1 - duration<float, std::milli>(now - last_held[pad]).count() / pad_touch::fade_ms;
        lights.pad(pad, fade > 0 ? pad_touch::force_colour(force[pad], fade) : Rgb {});
    }
    lights.fader(hardest > 0 ? pad_touch::force_colour(hardest) : Rgb {});
    unsigned banks = pad_touch::force_banks(hardest);
    for (unsigned bank = 0; bank < surface_bank_lights; ++bank) lights.bank(bank, bank < banks);
}

// Turns off only the lights the pad page set, leaving the transport lights alone.
void LightDriver::close_pad_lights() {
    if (!pads_running) return;
    pads_running = false;
    for (unsigned pad = 0; pad < surface_pad_lights; ++pad) lights.pad(pad, {});
    lights.fader({});
    for (unsigned bank = 0; bank < surface_bank_lights; ++bank) lights.bank(bank, false);
    lights.button(SurfaceButton::SixteenLevels, ButtonLight::Off);
}

// Sets the transport lights when the recorder's state calls for a different
// light, so a manual `led` command stays until then.
void LightDriver::recorder_lights(AppState& state) {
    auto mode = state.recorder.state();
    bool recording = mode == Recorder::State::Recording, playing = mode == Recorder::State::Playing;
    bool source = state.source.load() != uint8_t(InputSource::Off);
    const std::array<std::pair<SurfaceButton, ButtonLight>, 4> wanted {{
        {SurfaceButton::Sample, state.page.load() == Page::Recorder ? ButtonLight::Bright : ButtonLight::Off},
        {SurfaceButton::SampleRecord, recording ? ButtonLight::Bright : source ? ButtonLight::Dim : ButtonLight::Off},
        {SurfaceButton::Play, playing ? ButtonLight::Bright : state.recorder.length() ? ButtonLight::Dim : ButtonLight::Off},
        {SurfaceButton::Stop, recording || playing ? ButtonLight::Dim : ButtonLight::Off},
    }};
    for (unsigned n = 0; n < wanted.size(); ++n)
        if (shown_lights[n] != uint8_t(wanted[n].second) && lights.button(wanted[n].first, wanted[n].second))
            shown_lights[n] = uint8_t(wanted[n].second);
}
