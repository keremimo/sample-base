// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <chrono>
#include <functional>
#include <mutex>
#include <string>

// The ASoC codec behind a playback PCM, as the kernel shows it, so that the
// audio device sets the codec's clocks only while the codec is powered down
// (docs/HARDWARE.md, "Opening and closing without a click"). On the MPC that
// is the I2S card's AK4558: a new master clock while it is powered clicks in
// the jacks, and the kernel keeps it powered, with the board's output mute
// open, for pmdown_time (5 s) after a stream ends. A PCM without an ASoC
// link (USB, "null", a host build) has none, and everything here then does
// nothing.

// The AK4558's master clock in the clock tree (the device tree's
// `I2S1_MCLKOUT_TX`), read through debugfs: it keeps the rate it was last set
// to while gated. The card runs it at 512 fs (`simple-audio-card,mclk-fs`).
inline constexpr const char* codec_mclk = "/sys/kernel/debug/clk/i2s1_mclkout_tx/clk_rate";
inline constexpr unsigned codec_mclk_fs = 512;

struct CodecLink {
    std::string dir;   // the link's sysfs directory, with pmdown_time and dapm_widget; empty when there is none
    std::string clock; // the file holding the codec's master clock rate; empty with no link
    int card = -1;     // the sound card it belongs to
    bool found() const { return !dir.empty(); }
};

// `card` and `pcm_id` as snd_pcm_info() gives them for a playback PCM; the id
// starts with the link's name ("fe410000.i2s-ak4558-hifi ak4558-hifi-0").
// The link counts only when its pmdown_time can be written (root). `sys` and
// `clock` are injectable for tests.
CodecLink codec_link(int card, const std::string& pcm_id, const std::string& sys = "/sys",
                     const std::string& clock = codec_mclk);
// DAPM has the codec powered: a stream runs, or one ended less than
// pmdown_time ago (its "PM State" is not Off). False when unknown.
bool codec_powered(const CodecLink& link);
// The standard rate the codec's master clock is set for, 0 when unknown.
unsigned codec_clock_rate(const CodecLink& link);

// While one is alive, a playback stream of the link whose hardware setup ends
// (snd_pcm_hw_free(), or closing it) powers the codec down at once instead of
// pmdown_time later: DAPM shuts the board's output mute, waits for it, then
// powers the DACs down and holds the codec in reset. The link's own delay
// comes back when it goes.
class ImmediatePowerDown {
public:
    explicit ImmediatePowerDown(const CodecLink& link);
    ~ImmediatePowerDown();
    ImmediatePowerDown(const ImmediatePowerDown&) = delete;
    ImmediatePowerDown& operator=(const ImmediatePowerDown&) = delete;
private:
    std::string path, saved; // pmdown_time and what it held; empty when nothing was changed
};

// The built-in speaker's pin switch (the speaker amplifier's DAPM pin,
// `simple-audio-card,pin-switches`), and how long the codec's outputs take to
// settle after it leaves reset: the board's mute opens the jacks that long
// after the codec powers up (`unmute-delay-ms`).
inline constexpr const char* speaker_switch = "Builtin Speaker Switch";
inline constexpr std::chrono::milliseconds codec_settle {1000};

// Reads a card's speaker pin switch (1 on, 0 off, -1 when the card has none)
// and writes it; speaker_switch_io() in drivers/mixer.h on the MPC.
struct SwitchIo {
    std::function<int(int card)> read;
    std::function<bool(int card, bool on)> write;
};

// The built-in speaker while the codec powers up. Its amplifier
// (`simple-audio-amplifier`, GPIO SPKR_EN) has no mute of its own: DAPM
// enables it as soon as the DACs are up, some 20 ms after the codec leaves
// reset while its outputs still settle, and the speaker plays that as a
// click. So a stream that powers the codec up first holds the speaker: its
// pin switch goes off, and it comes back (as it was, or as the app has asked
// since) `settle` after the codec is up, as the board's mute opens for the
// jacks. The thread opening the stream holds; HardwareMixer, on the service
// thread, defers the app's changes and ends the hold. Safe from any thread.
class SpeakerHold {
public:
    using Clock = std::chrono::steady_clock;
    explicit SpeakerHold(SwitchIo io, Clock::duration settle = codec_settle);
    ~SpeakerHold(); // still holding: the switch as it was before the first hold
    SpeakerHold(const SpeakerHold&) = delete;
    SpeakerHold& operator=(const SpeakerHold&) = delete;
    // Before a stream powers up the codec on `card`: its speaker off now.
    void hold(int card);
    // Once that stream is prepared (or failed to open): the hold ends `settle` from `now`.
    void powered_up(Clock::time_point now = Clock::now());
    // While holding, a change to the switch waits for the end (true); false otherwise.
    bool defer(bool on);
    // A hold whose time has come ends, the speaker back on if that is what it waits for.
    void poll(Clock::time_point now = Clock::now());
    // The switch before the first hold (1 or 0), -1 before any: what leaving restores.
    int original() const;
    bool holding() const;
private:
    SwitchIo io;
    Clock::duration settle;
    mutable std::mutex lock;
    int held_card = -1, resume = -1, first = -1; // what the switch goes back to; its value before the first hold
    bool held = false, up = false;              // up: the codec is powered and `until` counts
    Clock::time_point until {};
};
