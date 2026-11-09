// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include "audio.h"
#include "midi.h"
#include "recorder.h"
#include "usb_audio.h"
#include "app/page.h"
#include "app/synth.h"
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>

constexpr unsigned sample_rate = Synth::sample_rate, block_frames = Synth::block_size;

// Everything the app's threads share. Four threads run at once:
//   audio    render_audio() (audio_render.h): synth, recorder and mix
//   MIDI     Controller::run_midi() (controller.h): MIDI in and out, lights
//   service  Controller::run(): control commands, slow hardware polling
//   screen   run_application() (application.h): a frame every 75 ms
// Every field is an atomic, a lock-free queue or guarded by a lock, so any
// thread may read it. Comments name the writer where only one may write.
struct AppState {
    MidiQueue<> queue; // Notes for the synth. The MIDI thread is its only producer.
    AudioStats audio;  // Written by the audio thread.
    std::atomic<bool> running {true}, failed {false}, screenshot {false};

    // Surface and MIDI diagnostics for `status` and the screen.
    std::atomic<uint32_t> held {0}, last_midi {0}, last_input {0};
    std::atomic<int> last_pad {-1}, velocity {0};
    std::atomic<unsigned> hardware_events {0}, external_events {0}, synthetic_events {0}, overruns {0}, clocks {0};
    std::atomic<unsigned> midi_send_errors {0};

    std::atomic<float> gain {1}, monitor {0}; // Output level and input monitoring, 0..1.
    std::atomic<bool> push_exit {true};       // Wheel Push returns to the stock app.

    // A remote tool (such as control calibration) owns the screen while
    // `prompting`; the recorder buttons rest. The service thread writes
    // `prompt` and the screen thread copies it.
    std::atomic<bool> prompting {false};
    std::mutex prompt_lock;
    std::string prompt;

    // Input recording showcase. `source` changes only on the service thread,
    // after the matching hardware route is in place; `channels` and the page
    // are plain choices any thread may set.
    static constexpr unsigned recording_seconds = 60;
    Recorder recorder {recording_seconds * sample_rate};
    UsbAudioInput usb;
    std::atomic<uint8_t> source {uint8_t(InputSource::Off)}, channels {uint8_t(InputChannels::Stereo)};
    std::atomic<int> source_request {-1}; // MIDI thread asks, service thread applies.
    std::atomic<float> input_left {0}, input_right {0}; // Decaying peaks of the selected source.

    std::atomic<Page> page {Page::Synth};
    std::atomic<unsigned> light_position {0}; // Light show position in ms; the MIDI thread writes it.
    std::atomic<int> scene_request {-1};      // Any thread asks; the MIDI thread jumps the light show.
};
