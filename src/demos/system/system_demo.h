// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include "runtime/app.h"
#include "runtime/session.h"
#include "surface/controls.h"
#include "surface/leds.h"
#include "demos/demo_state.h"
#include "demos/system/system.h"
#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

// System dashboard: Knob FX opens a page of the MPC's live state, from what
// the base already reads: CPU load and memory, temperatures, supply voltage,
// the power key and backlight; the audio engine and USB-C audio both ways;
// MIDI ports and the jacks; mounted storage. The buttons above the screen
// and the wheel step through four panels (system.h). Nothing is read while
// the page is closed.

// Service thread: reads the hardware every half second while the page is
// open (storage every two seconds) and publishes it to DemoState::system.
// Each opening starts the CPU meter and its history afresh.
class SystemReader {
public:
    using Clock = std::chrono::steady_clock;
    void service(DemoState& state, Hardware& hardware, Clock::time_point now);
private:
    sysinfo::CpuMeter cpu;
    sysinfo::History history;
    std::vector<sysinfo::Mount> storage;
    bool open = false;
    unsigned reads = 0;
    Clock::time_point next_read, next_storage;
};

// MIDI thread, on the System page: the left and middle buttons above the
// screen step back and forward through the panels, and so does the wheel.
// Returns true when the press or turn was used.
bool system_button(DemoState& state, SurfaceButton button);
bool system_wheel(DemoState& state, const SurfaceEvent& event);

// MIDI thread: Knob FX lights while the page is open.
class SystemLights {
public:
    void update(SurfaceLights& lights);
    // Turns Knob FX off once the page has closed.
    void close(SurfaceLights& lights);
private:
    bool showing = false;
};

// Service thread: `system` alone returns the latest readings as JSON;
// `system next|previous|health|audio|ports|storage` opens the page at that
// panel. Nothing for any other action.
std::string system_command(DemoState& state, const std::string& argument);

// Snapshot of the System page for one frame: the panel, the latest
// readings and the audio engine's own counters, read every frame.
struct SystemView {
    sysinfo::Panel panel = sysinfo::Panel::Health;
    sysinfo::Readings readings;
    uint64_t frames = 0;
    unsigned xruns = 0, capture_xruns = 0;
    bool capture_active = false;
    float peak = 0, input_peak = 0;
};

// Screen thread. `shown` is false while another page is open; the view is
// then empty and nothing is copied.
SystemView system_view(const DemoState& state, const Session& session, bool shown);
