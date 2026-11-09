// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include "leds.h"
#include "app/app_state.h"
#include <array>
#include <chrono>
#include <cstdint>

// Decides what the surface lights show: the light show while its page is
// open, otherwise the recorder's transport lights. It only requests changes
// through SurfaceLights; the MIDI thread sends them. MIDI thread only.
class LightDriver {
public:
    using Clock = std::chrono::steady_clock;
    explicit LightDriver(SurfaceLights& lights) : lights(lights) {}
    void update(AppState& state, Clock::time_point now);
    bool show_running() const { return running; }
private:
    void light_show(AppState& state, Clock::time_point now);
    void recorder_lights(AppState& state);
    SurfaceLights& lights;
    // Transport lights last requested by recorder_lights(), 0xff for none.
    std::array<uint8_t, 4> shown_lights {{0xff, 0xff, 0xff, 0xff}};
    bool running = false;
    Clock::time_point show_start, next_frame;
    std::array<Clock::time_point, surface_pad_lights> touched {}; // Last time each pad was held.
};
