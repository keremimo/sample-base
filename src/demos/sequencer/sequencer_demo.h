// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include "runtime/app.h"
#include "surface/controls.h"
#include "surface/leds.h"
#include "demos/demo_state.h"
#include "demos/sequencer/sequencer.h"
#include <array>
#include <chrono>
#include <cstdint>
#include <string>

// Step sequencer: SEQ opens a drum machine of eight synthesized drums, one
// track each, sixteen steps long (sequencer.h). In step mode the pads are the
// selected track's steps; in drum mode pads 1-8 play the drums and pads 9-16
// mute them. Play and Stop run it, Seq Record writes live hits in, and Tap
// Tempo, the wheel, Minus and Plus set the tempo. It keeps playing on the
// other pages; Stop stops it anywhere.

// MIDI thread: the sequencer's pads and controls. It keeps Shift and the
// tap times, which only the MIDI thread uses.
class SequencerInput {
public:
    using Clock = std::chrono::steady_clock;
    // Every surface event, before the buttons are routed. Tracks Shift; on
    // the SEQ page the wheel sets the tempo and pads go to the sequencer.
    // Returns true when a pad's note-on was used, so it does not also play
    // the synth.
    bool surface(DemoState& state, const SurfaceEvent& event);
    // A button pressed on the SEQ page. Returns false for buttons the page
    // leaves to the others, including Stop.
    bool button(DemoState& state, SurfaceButton button, Clock::time_point now);
private:
    bool shift = false;
    seq::TapTempo taps;
};

// MIDI thread, any page: Seq Record opens the SEQ page armed, and on it arms
// or disarms. Arming turns the pads to drums, since steps cannot be played.
void sequencer_record_button(DemoState& state);

// MIDI thread: a light frame every 20 ms while the page is open.
class SequencerLights {
public:
    using Clock = std::chrono::steady_clock;
    void update(SurfaceLights& lights, const DemoState& state, Clock::time_point now);
    // Turns off the lights the page set, once it has closed. Returns true if
    // it was showing, so the recorder's lights know to send themselves again.
    bool close(SurfaceLights& lights);
    bool running() const { return showing; }
private:
    bool showing = false;
    Clock::time_point next_frame;
    std::array<uint16_t, seq::tracks> triggers_seen {};
    std::array<Clock::time_point, seq::tracks> hit {}; // When each drum last sounded.
};

// Service thread: `seq` alone returns JSON; `seq play|stop|clear|reset`,
// `seq tempo 40-240`, `seq step A-H 1-16 0-127` and `seq mute A-H on|off`
// change it. Nothing for any other action.
std::string sequencer_command(DemoState& state, const std::string& action, const std::string& argument);
// The `status` fields for the sequencer.
std::string sequencer_status(const DemoState& state);
// Tempo in tenths of a BPM as text, "92.5".
std::string tempo_text(unsigned tempo);

// Snapshot of the SEQ page for one frame.
struct SeqView {
    std::array<std::array<uint8_t, seq::steps>, seq::tracks> steps {};
    uint8_t muted = 0, hits = 0; // A bit per track; `hits` sounded since the last frame.
    unsigned tempo = seq::default_tempo;
    int step = -1; // The playing step, -1 when stopped.
    uint8_t track = 0;
    bool drums = false, armed = false;
};

// Screen thread: the SEQ page's view. Remembers the drum hits it has shown.
class SeqFeed {
public:
    // `shown` is false while another page is open; the view is then empty.
    SeqView frame(const DemoState& state, bool shown);
private:
    std::array<uint16_t, seq::tracks> triggers_seen {};
};
