// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include "drivers/usb_audio.h"
#include "dsp/recorder.h"
#include "surface/midi.h"
#include "demos/page.h"
#include "demos/pad_touch/pad_touch.h"
#include "demos/sequencer/sequencer.h"
#include "demos/synth/synth.h"
#include <atomic>
#include <cstdint>

constexpr unsigned sample_rate = Synth::sample_rate, block_frames = Synth::block_size;

// What the demos' parts share across the runtime's threads (see
// runtime/session.h for which thread is which). Every field is atomic, a
// lock-free queue or thread-safe by design. Comments name the writer where
// only one may write.
struct DemoState {
    MidiQueue<> queue; // Notes for the synth. The MIDI thread is its only producer.
    std::atomic<unsigned> overruns {0}; // Synth queue overflows; the audio thread counts them.
    std::atomic<float> gain {1}, monitor {0}; // Output level and input monitoring, 0..1.

    // Input recording showcase. `source` changes only on the service thread,
    // after the matching hardware route is in place; `channels` and the page
    // are plain choices any thread may set.
    static constexpr unsigned recording_seconds = 60;
    Recorder recorder {recording_seconds * sample_rate};
    UsbAudioInput usb;
    std::atomic<uint8_t> source {uint8_t(InputSource::Off)}, channels {uint8_t(InputChannels::Stereo)};
    std::atomic<int> source_request {-1}; // MIDI thread asks, service thread applies.
    std::atomic<float> input_left {0}, input_right {0}; // Decaying peaks of the selected source.

    // Pad velocity and pressure showcase: every pad's latest strike and pressure.
    PadTouches pads;

    // Step sequencer: the pattern, tempo, transport and live drum hits.
    seq::Shared seq;

    std::atomic<Page> page {Page::Synth};
    std::atomic<unsigned> light_position {0}; // Light show position in ms; the MIDI thread writes it.
    std::atomic<int> scene_request {-1};      // Any thread asks; the MIDI thread jumps the light show.
};
