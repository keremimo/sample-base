// SPDX-License-Identifier: AGPL-3.0-only
#include "drivers/audio.h"
#include "drivers/platform.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <pthread.h>
#include <sstream>
#include <stdexcept>
#include <cstdlib>

namespace {
void check(int error, const char* action) {
    if (error < 0) throw std::runtime_error(std::string(action) + ": " + snd_strerror(error));
}
float block_peak(const AudioBlock& block) {
    double peak = 0;
    for (auto sample : block) peak = std::max(peak, std::abs(sample / 2147483648.0));
    return float(peak);
}
}
void AudioDevice::configure(snd_pcm_t* pcm, bool capture) {
    snd_pcm_hw_params_t* hw; snd_pcm_hw_params_alloca(&hw);
    check(snd_pcm_hw_params_any(pcm, hw), "Read PCM capabilities");
    check(snd_pcm_hw_params_set_access(pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED), "Set interleaved audio");
    check(snd_pcm_hw_params_set_format(pcm, hw, SND_PCM_FORMAT_S32_LE), "Set S32 audio");
    check(snd_pcm_hw_params_set_channels(pcm, hw, audio_channels), "Set stereo audio");
    check(snd_pcm_hw_params_set_rate(pcm, hw, audio_rate, 0), "Set 44100 Hz");
    check(snd_pcm_hw_params_set_period_size(pcm, hw, audio_frames, 0), "Set 128-frame period");
    snd_pcm_uframes_t buffer = audio_frames * (audio_queued_periods + 1);
    check(snd_pcm_hw_params_set_buffer_size_near(pcm, hw, &buffer), "Set PCM buffer");
    if (buffer < audio_frames * 2) throw std::runtime_error("PCM buffer is too small");
    check(snd_pcm_hw_params(pcm, hw), "Apply PCM format");
    snd_pcm_sw_params_t* sw; snd_pcm_sw_params_alloca(&sw);
    check(snd_pcm_sw_params_current(pcm, sw), "Read PCM software settings");
    check(snd_pcm_sw_params_set_start_threshold(pcm, sw, capture ? 1 : buffer - audio_frames), "Set PCM prefill");
    check(snd_pcm_sw_params_set_avail_min(pcm, sw, audio_frames), "Set PCM wakeup");
    check(snd_pcm_sw_params(pcm, sw), "Apply PCM software settings");
    check(snd_pcm_prepare(pcm), "Prepare PCM");
}
AudioDevice::AudioDevice(const std::string& out, const std::string& in) {
    check(snd_pcm_open(&output, out.c_str(), SND_PCM_STREAM_PLAYBACK, SND_PCM_NONBLOCK), "Open playback");
    try {
        configure(output, false);
        if (in != "off" && snd_pcm_open(&input, in.c_str(), SND_PCM_STREAM_CAPTURE, SND_PCM_NONBLOCK) >= 0) {
            try { configure(input, true); }
            catch (const std::exception& error) { std::cerr << "Capture unavailable: " << error.what() << '\n'; snd_pcm_close(input); input = nullptr; }
        }
    } catch (...) { snd_pcm_close(output); output = nullptr; throw; }
}
AudioDevice::~AudioDevice() {
    if (input) { snd_pcm_drop(input); snd_pcm_close(input); }
    if (output) { snd_pcm_drop(output); snd_pcm_close(output); }
}
int AudioDevice::recover(snd_pcm_t* pcm, int error, bool capture) {
    // Avoid snd_pcm_recover's potentially unbounded suspend/resume loop.
    if (error == -EINTR) return 0;
    if (error != -EPIPE && error != -ESTRPIPE) return error;
    int result = snd_pcm_prepare(pcm);
    if (result >= 0 && capture) result = snd_pcm_start(pcm);
    return result;
}
void AudioDevice::run(std::atomic<bool>& running, std::atomic<bool>& failed, AudioStats& stats, Render render, void* context) {
    sched_param priority {}; priority.sched_priority = 30;
    int scheduling = pthread_setschedparam(pthread_self(), SCHED_FIFO, &priority);
    if (scheduling) std::cerr << "Audio priority unavailable: " << scheduling << '\n';
    try {
        if (input && snd_pcm_start(input) < 0) { snd_pcm_close(input); input = nullptr; }
        stats.capture_active.store(input != nullptr);
        AudioBlock captured {}, incoming {}, outgoing {};
        unsigned collected = 0;
        while (running.load()) {
            incoming.fill(0);
            if (input) for (unsigned attempt = 0; attempt < 8; ++attempt) {
                auto count = snd_pcm_readi(input, captured.data() + collected * audio_channels, audio_frames - collected);
                if (count == -EAGAIN || count == 0) break;
                if (count < 0) {
                    ++stats.capture_xruns; collected = 0;
                    if (recover(input, int(count), true) < 0) { snd_pcm_close(input); input = nullptr; stats.capture_active.store(false); }
                    break;
                }
                collected += unsigned(count); stats.capture_frames.fetch_add(uint64_t(count));
                if (collected == audio_frames) { incoming = captured; collected = 0; break; }
            }
            stats.input_peak.store(block_peak(incoming));
            outgoing.fill(0); render(context, incoming, outgoing);
            float peak = block_peak(outgoing); stats.peak.store(peak);
            if (peak > 0.0001f) ++stats.sounding_blocks;
            unsigned offset = 0;
            while (offset < audio_frames && running.load()) {
                auto count = snd_pcm_writei(output, outgoing.data() + offset * audio_channels, audio_frames - offset);
                if (count == -EAGAIN || count == 0) {
                    int ready = snd_pcm_wait(output, 100);
                    if (ready < 0) { ++stats.xruns; check(recover(output, ready, false), "Recover PCM wait"); }
                } else if (count < 0) {
                    ++stats.xruns; check(recover(output, int(count), false), "Recover playback");
                } else { offset += unsigned(count); stats.frames.fetch_add(uint64_t(count)); }
            }
        }
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; failed.store(true); running.store(false); }
    stats.capture_active.store(false);
}
std::string AudioDevice::devices_json() {
    void** hints = nullptr;
    if (snd_device_name_hint(-1, "pcm", &hints) < 0) return "[]";
    std::ostringstream out; out << '['; bool first = true;
    for (unsigned n = 0; hints[n] && n < 256; ++n) {
        char* name = snd_device_name_get_hint(hints[n], "NAME");
        char* direction = snd_device_name_get_hint(hints[n], "IOID");
        char* description = snd_device_name_get_hint(hints[n], "DESC");
        if (name) {
            if (!first) out << ',';
            first = false;
            out << "{\"name\":" << json_string(name) << ",\"direction\":" << json_string(direction ? direction : "duplex")
                << ",\"description\":" << json_string(description ? description : "") << '}';
        }
        free(name); free(direction); free(description);
    }
    snd_device_name_free_hint(hints);
    return out.str() + ']';
}
