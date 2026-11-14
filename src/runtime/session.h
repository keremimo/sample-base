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
//   screen   run_app(): up to 60 frames a second, as fast as the panel takes
//            them; 10 a second once the screen stands still
// The screen thread's counters (Display::Stats, plus the last draw).
struct ScreenStats {
    std::atomic<uint64_t> presented {0}, pushed {0}, pixels {0}, clips {0};
    std::atomic<uint32_t> last_push_us {0}, max_push_us {0}, full_push_us {0}, draw_us {0};
};

// Every field is atomic or guarded by a lock. Comments name the writer.
struct Session {
    std::atomic<bool> running {true}, failed {false}, screenshot {false};
    AudioStats audio; // Audio thread.
    // The screen thread sets it when App::audio_format() changes; the audio
    // thread closes the device, opens the new format and clears it.
    std::atomic<bool> audio_reopen {false};
    ScreenStats screen; // Screen thread.

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

    // SD card access (drivers/card_share.h), written by the service thread:
    // a CardShareState, whether a computer is connected and whether it has
    // ejected the drive, and a count that moves each time the card comes back.
    // While the state is not Home nothing may read or write the card.
    std::atomic<uint8_t> card_share {0};
    std::atomic<bool> card_connected {false}, card_ejected {false};
    std::atomic<uint64_t> card_returns {0};

    // Screen Mirror (drivers/screen_mirror.h), written by the screen thread:
    // a MirrorState, the picture size and rate offered, whether it is JPEG, whether a
    // JPEG size fell back to 640 x 480 YUYV (no encoder), whether a change the
    // app asked for waits (a computer shows the camera) and the pictures sent.
    struct Mirror {
        std::atomic<uint8_t> state {0};
        std::atomic<uint16_t> width {0}, height {0}, fps {0};
        std::atomic<bool> mjpeg {false}, fallback {false}, held {false};
        std::atomic<uint64_t> frames {0};
    } mirror;
    std::mutex prompt_lock;
    std::string prompt;
};
