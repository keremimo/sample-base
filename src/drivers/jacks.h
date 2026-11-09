// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <alsa/asoundlib.h>
#include <array>
#include <cstdint>
#include <functional>
#include <string>

// The two jacks the AC50 can tell are plugged. Its device tree gives the I2S
// card `simple-audio-card,hp-det-gpio` (GPIO3_B0) and `out-det-gpio`
// (GPIO3_B1); the kernel debounces each for 150 ms, powers the matching DAPM
// pin and reports it as a read-only boolean card control. Line in, the sync
// jacks and the MIDI jacks have no detection switch.
enum class Jack : uint8_t { Headphones, LineOut };
constexpr unsigned jack_count = 2;
struct JackInfo { const char* key; const char* label; const char* control; };
constexpr std::array<JackInfo, jack_count> jack_info {{
    {"headphones", "Headphones", "Headphones Jack"},
    {"line_out", "Line out", "Line Out Jack"},
}};

// A jack's state as an app hears it. `initial` marks the first state known
// after the control appears (at startup, or after the card came back), not a
// plug or unplug. The timestamp is CLOCK_MONOTONIC when sample-base read the
// control: about 150 ms after the contact moved, because of the debounce.
struct JackEvent {
    Jack jack = Jack::Headphones;
    bool plugged = false;
    bool initial = false;
    uint64_t timestamp_ns = 0;
};

// Watches the jack controls with ALSA control events on its own handle, so
// the mixer's handle never queues events and nothing is re-read while the
// jacks stay put. One thread only (the service thread). The card is
// injectable for tests.
class JackDetect {
public:
    using Report = std::function<void(const JackEvent&)>;
    explicit JackDetect(std::string card = "hw:I2S");
    ~JackDetect();
    JackDetect(const JackDetect&) = delete;
    JackDetect& operator=(const JackDetect&) = delete;
    // Non-blocking. Reads pending control events and reports every jack whose
    // state differs from the last one reported, in Jack order; returns how
    // many. A missing or lost card is reopened at most once a second.
    unsigned poll(const Report& report);
    bool available() const { return ctl != nullptr; }
    // 1 plugged, 0 empty, -1 unknown (no card or no such control).
    int state(Jack jack) const { return jacks[unsigned(jack)].reported; }
    // Plugs and unplugs seen since startup; initial states do not count.
    uint64_t changes(Jack jack) const { return jacks[unsigned(jack)].changes; }
    std::string json() const;
private:
    struct State { int value = -1, reported = -1; uint64_t changes = 0, timestamp_ns = 0; };
    void open_card();
    void close_card();
    void read_values();
    std::string card;
    snd_ctl_t* ctl = nullptr;
    bool events = false; // Subscribed; without events the values are re-read once a second.
    std::array<State, jack_count> jacks {};
    uint64_t retry_ns = 0;
};
