// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <alsa/asoundlib.h>
#include <array>
#include <atomic>
#include <cstdint>
#include <string>

// The default format: 44.1 kHz with three 128-frame periods queued. An app
// may ask for another (App::audio_format()); blocks stay 128 frames at any rate.
constexpr unsigned audio_rate = 44100, audio_frames = 128, audio_channels = 2;
// Playback starts with this many periods queued and keeps about that many:
// a block rendered now is heard this many frames later.
constexpr unsigned audio_queued_periods = 3;
using AudioBlock = std::array<int32_t, audio_frames * audio_channels>;

// The rates the AK4558 path takes (the codec's normal and double speed with
// the card's fixed 512 fs master clock) and how many periods may be queued.
// More periods ride out a slow block (no crackle) but are heard later.
constexpr std::array<unsigned, 3> audio_rates {{44100, 48000, 96000}};
constexpr unsigned audio_min_periods = 2, audio_max_periods = 8;
struct AudioFormat {
    unsigned rate = audio_rate, periods = audio_queued_periods;
    unsigned latency_frames() const { return audio_frames * periods; }
    double latency_ms() const { return 1000.0 * latency_frames() / rate; }
    bool valid() const;
    bool operator==(const AudioFormat& other) const { return rate == other.rate && periods == other.periods; }
    bool operator!=(const AudioFormat& other) const { return !(*this == other); }
};

struct AudioStats {
    std::atomic<uint64_t> frames {0}, capture_frames {0}, sounding_blocks {0};
    std::atomic<unsigned> xruns {0}, capture_xruns {0};
    std::atomic<bool> capture_active {false};
    std::atomic<float> peak {0}, input_peak {0};
    // The format open now, written before each run(); reopens counts the format changes.
    std::atomic<unsigned> rate {audio_rate}, periods {audio_queued_periods}, reopens {0};
};

// Callback receives exactly 128 stereo S32 frames, with zero input when
// capture is unavailable. Callback must not allocate, block or access files.
// The constructor throws when the playback PCM refuses `format`.
class AudioDevice {
public:
    using Render = void (*)(void*, const AudioBlock&, AudioBlock&);
    AudioDevice(const std::string& output = "hw:I2S,0", const std::string& input = "hw:I2S,0", AudioFormat format = {});
    ~AudioDevice();
    AudioDevice(const AudioDevice&) = delete;
    AudioDevice& operator=(const AudioDevice&) = delete;
    // Until `running` goes false, or `reopen` true (the caller then closes
    // this device and opens another format). Writes the format into `stats`.
    void run(std::atomic<bool>& running, std::atomic<bool>& failed, AudioStats& stats, Render render, void* context,
             const std::atomic<bool>* reopen = nullptr);
    const AudioFormat& format() const { return opened; }
    static std::string devices_json();
private:
    static void configure(snd_pcm_t* pcm, bool capture, const AudioFormat& format);
    AudioFormat opened;
    static int recover(snd_pcm_t* pcm, int error, bool capture);
    snd_pcm_t *output = nullptr, *input = nullptr;
};
