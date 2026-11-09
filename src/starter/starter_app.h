// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include "runtime/app.h"
#include <array>
#include <atomic>

// The smallest complete app, to copy when starting your own (docs/APPS.md).
// Each pad plays a sine at its own pitch, as loud as it was struck; a held
// pad lights up; the screen names the last pad. Run it with --app starter.
class StarterApp : public App {
public:
    void render(const AudioBlock& input, AudioBlock& output) override;  // audio thread
    void surface(const SurfaceEvent& event) override;                   // MIDI thread
    void lights(SurfaceLights& lights, Clock::time_point now) override; // MIDI thread
    void draw(juce::Graphics& g) override;                              // screen thread

private:
    // How loud each pad should sound, 0..1. The MIDI thread sets it and the
    // audio thread reads it: an atomic is all the two threads share.
    std::array<std::atomic<float>, 16> loudness {};

    // Audio thread only.
    struct Voice { double phase = 0; float level = 0; };
    std::array<Voice, 16> voices {};

    juce::Typeface::Ptr font; // Screen thread; loaded on the first frame.
};
