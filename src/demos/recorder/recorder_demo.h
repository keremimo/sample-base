// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include "drivers/audio.h"
#include "runtime/app.h"
#include "surface/controls.h"
#include "surface/leds.h"
#include "demos/demo_state.h"
#include <array>
#include <chrono>
#include <cstdint>
#include <string>

// Input recording showcase. Sample opens its page. The left button above the
// screen steps the input through Line in, Mic, USB-C and Off, the middle one
// picks L + R, L or R, and Sample Record, Play and Stop drive one take of up
// to 60 seconds (dsp/recorder.h). Inputs stay untouched until one is chosen.

// MIDI thread: the recorder's buttons. Returns false for any other button.
bool recorder_button(DemoState& state, SurfaceButton button);

// Audio thread: records the selected input and adds take playback to
// `playback`. Keeps decaying input meters between blocks.
class RecorderAudio {
public:
    void process(DemoState& state, const AudioBlock& input, std::array<float, block_frames * 2>& playback);
private:
    float meter_left = 0, meter_right = 0;
};

// MIDI thread: the Sample, Sample Record, Play and Stop lights follow the
// recorder. A light is only requested when it should change, so a manual
// `led` command stays until then.
class RecorderLights {
public:
    void update(SurfaceLights& lights, const DemoState& state);
    // After every light went off: send the transport lights again.
    void forget() { shown.fill(0xff); }
private:
    std::array<uint8_t, 4> shown {{0xff, 0xff, 0xff, 0xff}}; // Last requested, 0xff for none.
};

// Service thread: applies an input the MIDI thread asked for.
void apply_source_request(DemoState& state, Hardware& hardware, const std::string& usb_device);
// Service thread: the input, source, channels, transport and recorder
// commands; nothing for any other action.
std::string recorder_command(DemoState& state, Hardware& hardware, const std::string& usb_device,
                             const std::string& action, const std::string& argument);
// The `status` fields for the recorder.
std::string recorder_status(const DemoState& state);

// Snapshot of the recorder page for one frame.
struct RecorderView {
    static constexpr unsigned columns = 296; // Waveform width in pixels.
    uint8_t source = 0, channels = 0, state = 0; // InputSource, InputChannels, Recorder::State
    unsigned frames = 0, position = 0, capacity = 0, rate = 44100;
    unsigned scale = 0; // Frames across the waveform; at least `frames`.
    float left = 0, right = 0; // Input peaks before channel selection.
    std::string detail; // Source status, or a hint after a refused action.
    bool hint = false;
    std::array<uint8_t, columns> waveform {};
};

// Screen thread: the recorder page's view. A refused take (no input chosen)
// shows a hint for a few frames, which is why this keeps state.
class RecorderFeed {
public:
    // `shown` is false while another page is open; the view is then empty.
    RecorderView frame(const DemoState& state, bool capture_active, bool shown);
private:
    unsigned refusals_seen = 0;
    std::chrono::steady_clock::time_point hint_until = std::chrono::steady_clock::time_point::min();
};
