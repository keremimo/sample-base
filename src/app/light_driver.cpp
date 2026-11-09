// SPDX-License-Identifier: AGPL-3.0-only
#include "app/light_driver.h"
#include "controls.h"
#include "app/light_show.h"
#include <utility>

void LightDriver::update(AppState& state, Clock::time_point now) {
    light_show(state, now);
    if (!running) recorder_lights(state);
}

// While the light page is open, sends a show frame every 25 ms; a held pad
// glows white and fades back over 400 ms after release. Closing the page
// turns every light off and hands the transport lights back to the recorder.
// A manual `led` command lasts until the next frame.
void LightDriver::light_show(AppState& state, Clock::time_point now) {
    using namespace std::chrono;
    if (state.page.load() != Page::Lights) {
        if (running) {
            running = false; lights.all_off(); shown_lights.fill(0xff);
            state.scene_request.store(-1); state.light_position.store(0);
        }
        return;
    }
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
