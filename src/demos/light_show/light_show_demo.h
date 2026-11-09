// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include "surface/controls.h"
#include "surface/leds.h"
#include "demos/demo_state.h"
#include "demos/light_show/light_show.h"
#include <array>
#include <chrono>
#include <cstdint>
#include <string>

// Light show: Pad FX opens a 49-second looping LED showcase (light_show.h)
// that uses every surface light. The left and middle buttons above the
// screen step through the scenes; pads glow white under a finger.

// MIDI thread: sends a show frame every 25 ms while the page is open.
class LightShowLights {
public:
    using Clock = std::chrono::steady_clock;
    // `held` is the pads held now (Session::held).
    void update(SurfaceLights& lights, DemoState& state, uint32_t held, Clock::time_point now);
    // Turns every light off once the page has closed. Returns true if the
    // show was running, so other lights know to send themselves again.
    bool close(SurfaceLights& lights, DemoState& state);
    bool running() const { return showing; }
private:
    bool showing = false;
    Clock::time_point show_start, next_frame;
    std::array<Clock::time_point, surface_pad_lights> touched {}; // Last time each pad was held.
};

// MIDI thread: the scene buttons while the page is open. Returns false for
// any other button.
bool light_show_button(DemoState& state, SurfaceButton button);
// The light show scene `step` scenes after the one playing, or the first
// scene when the show is closed.
int next_light_scene(const DemoState& state, int step);
// Service thread: `lights 1-N | next | previous` opens the show at a scene.
std::string light_show_command(DemoState& state, const std::string& argument);
