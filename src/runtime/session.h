// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include "drivers/audio.h"
#include "drivers/jacks.h"
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>

// What the runtime's threads share. An app reads it through App::session().
// Four threads run at once:
//   audio    AudioDevice::run(), calling App::render()
//   MIDI     Controller::run_midi(): surface and external MIDI, lights
//   service  Controller::run(): control commands and slow hardware requests
//   screen   run_app(): a frame every 75 ms
// Every field is atomic or guarded by a lock. Comments name the writer.
struct Session {
    std::atomic<bool> running {true}, failed {false}, screenshot {false};
    AudioStats audio; // Audio thread.

    // Surface diagnostics, written by the MIDI thread.
    std::atomic<uint32_t> held {0};                // Pads held now; bit 0 is pad 1.
    std::atomic<int> last_pad {-1}, velocity {0};  // Last pad struck, 0-15, and its velocity.
    std::atomic<uint32_t> last_midi {0};           // Status, key and value of the last surface message.
    std::atomic<uint32_t> last_input {0};          // pack_last_input(); the service thread writes the power key.
    std::atomic<unsigned> hardware_events {0}, external_events {0}, synthetic_events {0}, clocks {0};
    std::atomic<unsigned> midi_send_errors {0};

    std::atomic<bool> push_exit {true}; // Wheel Push returns to the stock app.

    // Headphones and line out: 1 plugged, 0 empty, -1 unknown. The service
    // thread writes each before App::jack() hears of the change.
    std::atomic<int> jacks[jack_count] {{-1}, {-1}};
    int plugged(Jack jack) const { return jacks[unsigned(jack)].load(); }

    // A remote tool such as control calibration owns the screen while
    // `prompting`: the runtime draws the prompt instead of the app, and apps
    // should leave button presses alone. The service thread writes `prompt`.
    std::atomic<bool> prompting {false};
    std::mutex prompt_lock;
    std::string prompt;
};
