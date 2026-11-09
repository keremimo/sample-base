// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <alsa/asoundlib.h>
#include <array>
#include <atomic>
#include <cstdint>
#include <string>

constexpr unsigned audio_rate = 44100, audio_frames = 128, audio_channels = 2;
using AudioBlock = std::array<int32_t, audio_frames * audio_channels>;
struct AudioStats {
    std::atomic<uint64_t> frames {0}, capture_frames {0}, sounding_blocks {0};
    std::atomic<unsigned> xruns {0}, capture_xruns {0};
    std::atomic<bool> capture_active {false};
    std::atomic<float> peak {0}, input_peak {0};
};

// Callback receives exactly 128 stereo S32 frames, with zero input when
// capture is unavailable. Callback must not allocate, block or access files.
class AudioDevice {
public:
    using Render = void (*)(void*, const AudioBlock&, AudioBlock&);
    AudioDevice(const std::string& output = "hw:I2S,0", const std::string& input = "hw:I2S,0");
    ~AudioDevice();
    AudioDevice(const AudioDevice&) = delete;
    AudioDevice& operator=(const AudioDevice&) = delete;
    void run(std::atomic<bool>& running, std::atomic<bool>& failed, AudioStats& stats, Render render, void* context);
    static std::string devices_json();
private:
    static void configure(snd_pcm_t* pcm, bool capture);
    static int recover(snd_pcm_t* pcm, int error, bool capture);
    snd_pcm_t *output = nullptr, *input = nullptr;
};
