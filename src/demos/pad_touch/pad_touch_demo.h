// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include "runtime/app.h"
#include "surface/leds.h"
#include "demos/pad_touch/pad_touch.h"
#include <array>
#include <chrono>
#include <cstdint>

// Pad touch: 16 Levels opens a page that shows how hard the pads are played.
// Pads light by force (pad_touch.h), the bank lights A-H meter the hardest
// held pad, the fader light takes its colour, and the screen shows velocity,
// pressure and five seconds of pressure history. Pressure is not played.

// MIDI thread: records a pad's strike, pressure or release.
void record_pad_touch(PadTouches& pads, const SurfaceEvent& event);

// MIDI thread: a light frame every 20 ms while the page is open. Strikes
// are counted, so a tap shorter than a frame still flashes.
class PadTouchLights {
public:
    using Clock = std::chrono::steady_clock;
    // `held` is the pads held now (Session::held).
    void update(SurfaceLights& lights, const PadTouches& pads, uint32_t held, Clock::time_point now);
    // Turns off only the lights the page set, once it has closed.
    void close(SurfaceLights& lights);
private:
    bool showing = false;
    Clock::time_point next_frame;
    std::array<uint16_t, surface_pad_lights> strikes_seen {};
    std::array<Clock::time_point, surface_pad_lights> struck {}, last_held {};
    std::array<float, surface_pad_lights> force {}; // Last force shown, kept for the release fade.
};

// Snapshot of the pad touch page for one frame. The trace keeps one sample
// every 75 ms, oldest first: about five seconds.
struct PadsView {
    static constexpr unsigned trace_length = 68;
    std::array<uint8_t, 16> velocity {}, pressure {}, peak {}; // Latest strike, pressure now, highest this touch.
    std::array<uint8_t, trace_length> trace {};   // Highest pressure on any pad during each sample.
    std::array<uint8_t, trace_length> strikes {}; // Velocity of a strike during each sample, 0 for none.
};

// Screen thread: the pressure history, sampled every 75 ms whichever page is
// open so it is current when the pad page opens. Called every screen frame;
// frames between samples leave it alone.
class PadTrace {
public:
    // `velocity` is the last strike's velocity (Session::velocity).
    void advance(PadTouches& pads, int velocity, std::chrono::steady_clock::time_point now);
    PadsView view(const PadTouches& pads) const;
private:
    std::array<uint8_t, PadsView::trace_length> trace {}, strike_marks {};
    unsigned strikes_seen = 0;
    std::chrono::steady_clock::time_point next_sample {};
};
