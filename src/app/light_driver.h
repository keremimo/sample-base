// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include "leds.h"
#include "app/app_state.h"
#include <array>
#include <chrono>
#include <cstdint>

// Decides what the surface lights show: the light show or the pad touch
// lights while their page is open, and the recorder's transport lights
// unless the light show is running. It only requests changes through
// SurfaceLights; the MIDI thread sends them. MIDI thread only.
class LightDriver {
public:
    using Clock = std::chrono::steady_clock;
    explicit LightDriver(SurfaceLights& lights) : lights(lights) {}
    void update(AppState& state, Clock::time_point now);
    // True while a page owns the pad, fader and bank lights.
    bool lights_in_use() const { return running || pads_running; }
private:
    void light_show(AppState& state, Clock::time_point now);
    void close_light_show(AppState& state);
    void pad_lights(AppState& state, Clock::time_point now);
    void close_pad_lights();
    void recorder_lights(AppState& state);
    SurfaceLights& lights;
    // Transport lights last requested by recorder_lights(), 0xff for none.
    std::array<uint8_t, 4> shown_lights {{0xff, 0xff, 0xff, 0xff}};

    // Light show.
    bool running = false;
    Clock::time_point show_start, next_frame;
    std::array<Clock::time_point, surface_pad_lights> touched {}; // Last time each pad was held.

    // Pad touch page.
    bool pads_running = false;
    Clock::time_point next_pad_frame;
    std::array<uint16_t, surface_pad_lights> strikes_seen {};
    std::array<Clock::time_point, surface_pad_lights> struck {}, last_held {};
    std::array<float, surface_pad_lights> force {}; // Last force shown, kept for the release fade.
};
