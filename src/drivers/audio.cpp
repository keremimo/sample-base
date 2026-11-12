// SPDX-License-Identifier: AGPL-3.0-only
#include "drivers/audio.h"
#include "drivers/codec_power.h"
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
// The ASoC codec behind an open playback PCM (drivers/codec_power.h); none for USB, "null" and the like.
CodecLink codec_of(snd_pcm_t* pcm) {
    snd_pcm_info_t* info; snd_pcm_info_alloca(&info);
    if (snd_pcm_info(pcm, info) < 0) return {};
    const char* id = snd_pcm_info_get_id(info);
    return codec_link(snd_pcm_info_get_card(info), id ? id : "");
}
// Ends a stopped playback PCM's hardware setup with its codec powered down at once, behind the board's
// output mute, rather than pmdown_time later: whatever opens it next sets its clocks while it is off.
void free_quietly(snd_pcm_t* pcm) {
    ImmediatePowerDown now(codec_of(pcm));
    snd_pcm_hw_free(pcm);
}
}
bool AudioFormat::valid() const {
    return std::find(audio_rates.begin(), audio_rates.end(), rate) != audio_rates.end()
        && periods >= audio_min_periods && periods <= audio_max_periods;
}
void AudioDevice::configure(snd_pcm_t* pcm, bool capture, const AudioFormat& format) {
    snd_pcm_hw_params_t* hw; snd_pcm_hw_params_alloca(&hw);
    check(snd_pcm_hw_params_any(pcm, hw), "Read PCM capabilities");
    check(snd_pcm_hw_params_set_access(pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED), "Set interleaved audio");
    check(snd_pcm_hw_params_set_format(pcm, hw, SND_PCM_FORMAT_S32_LE), "Set S32 audio");
    check(snd_pcm_hw_params_set_channels(pcm, hw, audio_channels), "Set stereo audio");
    check(snd_pcm_hw_params_set_rate(pcm, hw, format.rate, 0), ("Set " + std::to_string(format.rate) + " Hz").c_str());
    check(snd_pcm_hw_params_set_period_size(pcm, hw, audio_frames, 0), "Set 128-frame period");
    snd_pcm_uframes_t buffer = audio_frames * (format.periods + 1);
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
AudioDevice::AudioDevice(const std::string& out, const std::string& in, AudioFormat format, SpeakerHold* speaker) : opened(format) {
    if (!format.valid()) throw std::runtime_error("Unsupported audio format " + std::to_string(format.rate) + " Hz, "
                                                  + std::to_string(format.periods) + " periods");
    check(snd_pcm_open(&output, out.c_str(), SND_PCM_STREAM_PLAYBACK, SND_PCM_NONBLOCK), "Open playback");
    try {
        // A stream another program ended moments ago leaves the codec powered at that program's rate (the
        // stock app's 44.1 kHz) for pmdown_time: a new clock now clicks in the jacks. Taking the stream over
        // at that rate cancels the kernel's delayed power-down without touching the clock; freeing it then
        // powers the codec down, and this format's clock is set while it is off (HARDWARE.md, "Opening and
        // closing without a click").
        CodecLink codec = codec_of(output);
        AudioFormat running {codec_clock_rate(codec), format.periods};
        bool powered = codec_powered(codec);
        // Unless the codec already runs at this rate, preparing below powers it up, and the speaker's
        // amplifier would come on before the codec's outputs settle: off until they have.
        if (speaker && codec.found() && !(powered && running.rate == format.rate)) speaker->hold(codec.card);
        if (running.rate != format.rate && running.valid() && powered) {
            try { configure(output, false, running); } catch (const std::exception&) {}
            free_quietly(output);
        }
        configure(output, false, format);
        if (speaker) speaker->powered_up();
        if (in != "off" && snd_pcm_open(&input, in.c_str(), SND_PCM_STREAM_CAPTURE, SND_PCM_NONBLOCK) >= 0) {
            try { configure(input, true, format); }
            catch (const std::exception& error) { std::cerr << "Capture unavailable: " << error.what() << '\n'; snd_pcm_close(input); input = nullptr; }
        }
    } catch (...) {
        if (speaker) speaker->powered_up();
        free_quietly(output); snd_pcm_close(output); output = nullptr; throw;
    }
}
// The codec powers down as the device closes, not pmdown_time later: the next device, at any rate, or the
// next program, starts it from off behind the board's output mute, which opens a second after it starts.
AudioDevice::~AudioDevice() {
    if (input) { snd_pcm_drop(input); snd_pcm_close(input); }
    if (output) { snd_pcm_drop(output); free_quietly(output); snd_pcm_close(output); }
}
int AudioDevice::recover(snd_pcm_t* pcm, int error, bool capture) {
    // Avoid snd_pcm_recover's potentially unbounded suspend/resume loop.
    if (error == -EINTR) return 0;
    if (error != -EPIPE && error != -ESTRPIPE) return error;
    int result = snd_pcm_prepare(pcm);
    if (result >= 0 && capture) result = snd_pcm_start(pcm);
    return result;
}
void AudioDevice::run(std::atomic<bool>& running, std::atomic<bool>& failed, AudioStats& stats, Render render, void* context,
                      const std::atomic<bool>* reopen) {
    stats.rate.store(opened.rate); stats.periods.store(opened.periods);
    auto going = [&] { return running.load() && !(reopen && reopen->load()); };
    sched_param priority {}; priority.sched_priority = 30;
    int scheduling = pthread_setschedparam(pthread_self(), SCHED_FIFO, &priority);
    if (scheduling) std::cerr << "Audio priority unavailable: " << scheduling << '\n';
    try {
        if (input && snd_pcm_start(input) < 0) { snd_pcm_close(input); input = nullptr; }
        stats.capture_active.store(input != nullptr);
        AudioBlock captured {}, incoming {}, outgoing {};
        unsigned collected = 0;
        while (going()) {
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
            while (offset < audio_frames && going()) {
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
