// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
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
