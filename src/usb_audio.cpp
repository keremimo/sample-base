// SPDX-License-Identifier: AGPL-3.0-only
#include "usb_audio.h"
#include "platform.h"
#include <alsa/asoundlib.h>
#include <pthread.h>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <iostream>
#include <sstream>
#include <utility>
#include <vector>

namespace {
struct Capture {
    snd_pcm_t* pcm = nullptr;
    unsigned rate = 0, channels = 0, width = 0;
    snd_pcm_uframes_t period = 0;
};
// The gadget offers exactly one format; another PCM may offer several.
bool open_capture(const std::string& name, long host_rate, Capture& capture) {
    if (snd_pcm_open(&capture.pcm, name.c_str(), SND_PCM_STREAM_CAPTURE, SND_PCM_NONBLOCK) < 0) { capture.pcm = nullptr; return false; }
    snd_pcm_hw_params_t* hw; snd_pcm_hw_params_alloca(&hw);
    snd_pcm_sw_params_t* sw; snd_pcm_sw_params_alloca(&sw);
    const std::pair<snd_pcm_format_t, unsigned> formats[] {{SND_PCM_FORMAT_S32_LE, 4}, {SND_PCM_FORMAT_S24_3LE, 3}, {SND_PCM_FORMAT_S16_LE, 2}};
    bool ok = snd_pcm_hw_params_any(capture.pcm, hw) >= 0 &&
              snd_pcm_hw_params_set_access(capture.pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED) >= 0;
    capture.width = 0;
    for (auto& format : formats)
        if (ok && !capture.width && snd_pcm_hw_params_test_format(capture.pcm, hw, format.first) == 0 &&
            snd_pcm_hw_params_set_format(capture.pcm, hw, format.first) >= 0) capture.width = format.second;
    unsigned channels = 0, rate = host_rate > 0 ? unsigned(host_rate) : UsbAudioInput::output_rate;
    ok = ok && capture.width && snd_pcm_hw_params_get_channels_min(hw, &channels) >= 0;
    if (ok && channels < 2 && snd_pcm_hw_params_test_channels(capture.pcm, hw, 2) == 0) channels = 2;
    ok = ok && channels >= 1 && channels <= 32 && snd_pcm_hw_params_set_channels(capture.pcm, hw, channels) >= 0 &&
         snd_pcm_hw_params_set_rate_near(capture.pcm, hw, &rate, nullptr) >= 0;
    snd_pcm_uframes_t period = 256, buffer = 0;
    ok = ok && snd_pcm_hw_params_set_period_size_near(capture.pcm, hw, &period, nullptr) >= 0;
    buffer = period * 8;
    ok = ok && snd_pcm_hw_params_set_buffer_size_near(capture.pcm, hw, &buffer) >= 0 &&
         snd_pcm_hw_params(capture.pcm, hw) >= 0 && snd_pcm_hw_params_get_period_size(hw, &period, nullptr) >= 0 &&
         snd_pcm_sw_params_current(capture.pcm, sw) >= 0 && snd_pcm_sw_params_set_avail_min(capture.pcm, sw, period) >= 0 &&
         snd_pcm_sw_params(capture.pcm, sw) >= 0 && snd_pcm_prepare(capture.pcm) >= 0 && snd_pcm_start(capture.pcm) >= 0;
    if (!ok || !period || rate < 8000 || rate > 384000) { snd_pcm_close(capture.pcm); capture.pcm = nullptr; return false; }
    capture.rate = rate; capture.channels = channels; capture.period = period;
    return true;
}
}

UsbAudioInput::Device UsbAudioInput::find_gadget() {
    snd_ctl_card_info_t* info; snd_ctl_card_info_alloca(&info);
    int card = -1;
    for (unsigned n = 0; n < 32 && snd_card_next(&card) >= 0 && card >= 0; ++n) {
        snd_ctl_t* ctl = nullptr;
        if (snd_ctl_open(&ctl, ("hw:" + std::to_string(card)).c_str(), SND_CTL_NONBLOCK) < 0) continue;
        bool gadget = snd_ctl_card_info(ctl, info) >= 0 && !strcmp(snd_ctl_card_info_get_driver(info), "UAC2_Gadget");
        std::string id = gadget ? snd_ctl_card_info_get_id(info) : "";
        snd_ctl_close(ctl);
        if (gadget && !id.empty()) return {"hw:CARD=" + id + ",DEV=0", "hw:CARD=" + id};
    }
    return {};
}

long UsbAudioInput::host_rate(const std::string& control) {
    snd_ctl_t* ctl = nullptr;
    if (control.empty() || snd_ctl_open(&ctl, control.c_str(), SND_CTL_NONBLOCK) < 0) return -1;
    snd_ctl_elem_value_t* value; snd_ctl_elem_value_alloca(&value);
    snd_ctl_elem_value_set_interface(value, SND_CTL_ELEM_IFACE_PCM);
    snd_ctl_elem_value_set_name(value, "Capture Rate");
    long rate = snd_ctl_elem_read(ctl, value) < 0 ? -1 : snd_ctl_elem_value_get_integer(value, 0);
    snd_ctl_close(ctl);
    return rate;
}

void UsbAudioInput::start(const std::string& device) {
    stop();
    ring.flush();
    running.store(true);
    set_state(UsbInputState::Waiting);
    thread = std::thread([this, device] { run(device); });
}

void UsbAudioInput::stop() {
    running.store(false);
    if (thread.joinable()) thread.join();
    set_state(UsbInputState::Stopped);
    stream_rate.store(0); stream_channels.store(0); stream_width.store(0);
}

void UsbAudioInput::run(std::string requested) {
    sched_param priority {}; priority.sched_priority = 20;
    pthread_setschedparam(pthread_self(), SCHED_FIFO, &priority); // Best effort; ordinary priority also works.
    Capture capture;
    LinearResampler resampler;
    std::vector<uint8_t> raw;
    std::vector<float> converted;
    long open_rate = 0;
    auto close = [&] {
        if (capture.pcm) { snd_pcm_drop(capture.pcm); snd_pcm_close(capture.pcm); capture.pcm = nullptr; }
        stream_rate.store(0); stream_channels.store(0); stream_width.store(0);
    };
    auto next_check = std::chrono::steady_clock::now();
    while (running.load()) {
        auto now = std::chrono::steady_clock::now();
        if (now >= next_check) {
            next_check = now + std::chrono::milliseconds(250);
            Device device = requested == "auto" ? find_gadget() : Device {requested, {}};
            { std::lock_guard<std::mutex> lock(device_lock); device_name = device.pcm; }
            long rate = device.pcm.empty() ? -1 : host_rate(device.control);
            if (device.pcm.empty() || rate == 0) {
                close(); set_state(device.pcm.empty() ? UsbInputState::Missing : UsbInputState::Waiting);
            } else if (capture.pcm && rate > 0 && rate != open_rate) {
                close(); // The host changed rate; reopen at the new one.
            }
            if (!capture.pcm && !device.pcm.empty() && rate != 0) {
                if (open_capture(device.pcm, rate, capture)) {
                    open_rate = rate; ++opens;
                    raw.assign(capture.period * capture.channels * capture.width, 0);
                    resampler.reset(capture.rate, output_rate);
                    ring.flush();
                    stream_rate.store(capture.rate); stream_channels.store(capture.channels); stream_width.store(capture.width);
                    set_state(UsbInputState::Running);
                } else set_state(UsbInputState::Failed);
            }
        }
        if (!capture.pcm) { std::this_thread::sleep_for(std::chrono::milliseconds(20)); continue; }
        int ready = snd_pcm_wait(capture.pcm, 50);
        snd_pcm_sframes_t count = ready < 0 ? ready : snd_pcm_readi(capture.pcm, raw.data(), capture.period);
        if (count == -EAGAIN || count == 0) continue;
        if (count < 0) {
            ++xruns;
            if ((count != -EPIPE && count != -ESTRPIPE) || snd_pcm_prepare(capture.pcm) < 0 || snd_pcm_start(capture.pcm) < 0) {
                close(); set_state(UsbInputState::Failed); // Retried at the next check.
            }
            continue;
        }
        converted.clear();
        unsigned frame_bytes = capture.channels * capture.width;
        for (snd_pcm_sframes_t frame = 0; frame < count; ++frame) {
            const uint8_t* bytes = raw.data() + size_t(frame) * frame_bytes;
            float left = pcm_sample(bytes, capture.width);
            float right = capture.channels > 1 ? pcm_sample(bytes + capture.width, capture.width) : left;
            resampler.feed(left, right, [&](float l, float r) { converted.push_back(l); converted.push_back(r); });
        }
        ring.push(converted.data(), unsigned(converted.size() / 2));
    }
    close();
}

std::string UsbAudioInput::json() const {
    std::string device;
    { std::lock_guard<std::mutex> lock(device_lock); device = device_name; }
    std::ostringstream out;
    out << "{\"state\":" << json_string(usb_input_state_names[size_t(state())]) << ",\"device\":" << json_string(device)
        << ",\"rate\":" << rate() << ",\"channels\":" << channels() << ",\"bits\":" << bits()
        << ",\"buffered\":" << ring.buffered() << ",\"overflows\":" << ring.overflows.load()
        << ",\"underruns\":" << ring.underruns.load() << ",\"xruns\":" << xruns.load() << ",\"opens\":" << opens.load() << '}';
    return out.str();
}
