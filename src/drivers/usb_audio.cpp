// SPDX-License-Identifier: AGPL-3.0-only
#include "drivers/usb_audio.h"
#include "drivers/platform.h"
#include <alsa/asoundlib.h>
#include <pthread.h>
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstring>
#include <sstream>
#include <utility>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;

struct Stream {
    snd_pcm_t* pcm = nullptr;
    unsigned rate = 0, channels = 0, width = 0;
    snd_pcm_uframes_t period = 0;
};

// Opens one direction of the gadget, or any PCM, at the computer's rate, or
// at `fallback` when there is no rate control. The gadget offers exactly one
// format and channel count; another PCM may offer several, and gets S32, then
// S24 in three bytes, then S16, with two channels where it allows them.
// Capture starts at once with an eight-period buffer. Playback has four
// periods and starts by itself once the first full buffer is written.
bool open_stream(const std::string& name, snd_pcm_stream_t direction, long host_rate, unsigned fallback, Stream& stream) {
    if (snd_pcm_open(&stream.pcm, name.c_str(), direction, SND_PCM_NONBLOCK) < 0) { stream.pcm = nullptr; return false; }
    bool capture = direction == SND_PCM_STREAM_CAPTURE;
    snd_pcm_hw_params_t* hw; snd_pcm_hw_params_alloca(&hw);
    snd_pcm_sw_params_t* sw; snd_pcm_sw_params_alloca(&sw);
    const std::pair<snd_pcm_format_t, unsigned> formats[] {{SND_PCM_FORMAT_S32_LE, 4}, {SND_PCM_FORMAT_S24_3LE, 3}, {SND_PCM_FORMAT_S16_LE, 2}};
    bool ok = snd_pcm_hw_params_any(stream.pcm, hw) >= 0 &&
              snd_pcm_hw_params_set_access(stream.pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED) >= 0;
    stream.width = 0;
    for (auto& format : formats)
        if (ok && !stream.width && snd_pcm_hw_params_test_format(stream.pcm, hw, format.first) == 0 &&
            snd_pcm_hw_params_set_format(stream.pcm, hw, format.first) >= 0) stream.width = format.second;
    unsigned channels = 0, rate = host_rate > 0 ? unsigned(host_rate) : fallback;
    ok = ok && stream.width && snd_pcm_hw_params_get_channels_min(hw, &channels) >= 0;
    if (ok && channels < 2 && snd_pcm_hw_params_test_channels(stream.pcm, hw, 2) == 0) channels = 2;
    ok = ok && channels >= 1 && channels <= 32 && snd_pcm_hw_params_set_channels(stream.pcm, hw, channels) >= 0 &&
         snd_pcm_hw_params_set_rate_near(stream.pcm, hw, &rate, nullptr) >= 0;
    snd_pcm_uframes_t period = 256, buffer = 0;
    ok = ok && snd_pcm_hw_params_set_period_size_near(stream.pcm, hw, &period, nullptr) >= 0;
    buffer = period * (capture ? 8 : 4);
    ok = ok && snd_pcm_hw_params_set_buffer_size_near(stream.pcm, hw, &buffer) >= 0 &&
         snd_pcm_hw_params(stream.pcm, hw) >= 0 && snd_pcm_hw_params_get_period_size(hw, &period, nullptr) >= 0 &&
         snd_pcm_hw_params_get_buffer_size(hw, &buffer) >= 0 &&
         snd_pcm_sw_params_current(stream.pcm, sw) >= 0 && snd_pcm_sw_params_set_avail_min(stream.pcm, sw, period) >= 0 &&
         (capture || snd_pcm_sw_params_set_start_threshold(stream.pcm, sw, buffer) >= 0) &&
         snd_pcm_sw_params(stream.pcm, sw) >= 0 && snd_pcm_prepare(stream.pcm) >= 0 && (!capture || snd_pcm_start(stream.pcm) >= 0);
    if (!ok || !period || rate < 8000 || rate > 384000) { snd_pcm_close(stream.pcm); stream.pcm = nullptr; return false; }
    stream.rate = rate; stream.channels = channels; stream.period = period;
    return true;
}

void raise_priority() {
    sched_param priority {}; priority.sched_priority = 20;
    pthread_setschedparam(pthread_self(), SCHED_FIFO, &priority); // Best effort; ordinary priority also works.
}

// The gadget card, or a PCM named on the command line, and its rate now.
struct Target { std::string pcm; long rate = -1; };
Target find_target(const std::string& requested, const char* control) {
    if (requested != "auto") return {requested, -1};
    Uac2Gadget gadget = find_uac2_gadget();
    return {gadget.pcm, gadget.pcm.empty() ? -1 : uac2_rate(gadget.control, control)};
}
} // namespace

Uac2Gadget find_uac2_gadget() {
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

long uac2_rate(const std::string& control, const char* name) {
    snd_ctl_t* ctl = nullptr;
    if (control.empty() || snd_ctl_open(&ctl, control.c_str(), SND_CTL_NONBLOCK) < 0) return -1;
    snd_ctl_elem_value_t* value; snd_ctl_elem_value_alloca(&value);
    snd_ctl_elem_value_set_interface(value, SND_CTL_ELEM_IFACE_PCM);
    snd_ctl_elem_value_set_name(value, name);
    long rate = snd_ctl_elem_read(ctl, value) < 0 ? -1 : snd_ctl_elem_value_get_integer(value, 0);
    snd_ctl_close(ctl);
    return rate;
}

// --- Input: the computer plays, the MPC hears ---

void UsbAudioInput::start(const std::string& device) {
    stop();
    ring.flush();
    running.store(true);
    set_state(UsbStreamState::Waiting);
    thread = std::thread([this, device] { run(device); });
}

void UsbAudioInput::stop() {
    running.store(false);
    if (thread.joinable()) thread.join();
    set_state(UsbStreamState::Stopped);
    stream_rate.store(0); stream_channels.store(0); stream_width.store(0); trim_ppm.store(0);
}

void UsbAudioInput::run(std::string requested) {
    raise_priority();
    Stream capture;
    SincResampler<2> resampler;
    std::vector<uint8_t> raw;
    std::vector<float> converted;
    long open_rate = 0;
    unsigned open_device = 0;
    // The ring's fill after each push since the last trim, and the computer's frames meanwhile.
    double fill_sum = 0; unsigned fill_count = 0, trim_frames = 0;
    auto close = [&] {
        if (capture.pcm) { snd_pcm_drop(capture.pcm); snd_pcm_close(capture.pcm); capture.pcm = nullptr; }
        stream_rate.store(0); stream_channels.store(0); stream_width.store(0); trim_ppm.store(0);
    };
    auto next_check = Clock::now();
    while (running.load()) {
        auto now = Clock::now();
        if (now >= next_check) {
            next_check = now + std::chrono::milliseconds(250);
            Target target = find_target(requested, "Capture Rate");
            { std::lock_guard<std::mutex> lock(device_lock); device_name = target.pcm; }
            if (target.pcm.empty() || target.rate == 0) {
                close(); set_state(target.pcm.empty() ? UsbStreamState::Missing : UsbStreamState::Waiting);
            } else if (capture.pcm && ((target.rate > 0 && target.rate != open_rate) || device_rate.load() != open_device)) {
                close(); // The computer or the audio device changed rate; reopen at the new one.
            }
            if (!capture.pcm && !target.pcm.empty() && target.rate != 0) {
                unsigned output_rate = device_rate.load();
                if (open_stream(target.pcm, SND_PCM_STREAM_CAPTURE, target.rate, output_rate, capture)) {
                    open_rate = target.rate; open_device = output_rate; ++opens;
                    raw.assign(capture.period * capture.channels * capture.width, 0);
                    resampler.reset(capture.rate, output_rate);
                    fill_sum = 0; fill_count = 0; trim_frames = 0;
                    ring.flush();
                    stream_rate.store(capture.rate); stream_channels.store(capture.channels); stream_width.store(capture.width);
                    set_state(UsbStreamState::Running);
                } else set_state(UsbStreamState::Failed);
            }
        }
        if (!capture.pcm) { std::this_thread::sleep_for(std::chrono::milliseconds(20)); continue; }
        int ready = snd_pcm_wait(capture.pcm, 50);
        snd_pcm_sframes_t count = ready < 0 ? ready : snd_pcm_readi(capture.pcm, raw.data(), capture.period);
        if (count == -EAGAIN || count == 0) continue;
        if (count < 0) {
            ++xruns;
            if ((count != -EPIPE && count != -ESTRPIPE) || snd_pcm_prepare(capture.pcm) < 0 || snd_pcm_start(capture.pcm) < 0) {
                close(); set_state(UsbStreamState::Failed); // Retried at the next check.
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
        // Every 250 ms of the computer's audio, counted in its frames, the
        // ring's average fill meanwhile trims the conversion.
        fill_sum += ring.buffered(); ++fill_count; trim_frames += unsigned(count);
        if (trim_frames >= capture.rate / 4) {
            double factor = drift_trim(fill_sum / fill_count, prime_frames);
            resampler.trim(factor);
            trim_ppm.store(int(std::lround((factor - 1.0) * 1e6)));
            fill_sum = 0; fill_count = 0; trim_frames = 0;
        }
    }
    close();
}

UsbStreamStats UsbAudioInput::stats() const {
    return {state(), rate(), channels(), bits(), ring.buffered(), ring.overflows.load(), ring.underruns.load(), xruns.load(),
            opens.load(), trim_ppm.load()};
}

std::string UsbAudioInput::json() const {
    std::string device;
    { std::lock_guard<std::mutex> lock(device_lock); device = device_name; }
    auto now = stats();
    std::ostringstream out;
    out << "{\"state\":" << json_string(usb_stream_state_names[size_t(now.state)]) << ",\"device\":" << json_string(device)
        << ",\"rate\":" << now.rate << ",\"channels\":" << now.channels << ",\"bits\":" << now.bits
        << ",\"buffered\":" << now.buffered << ",\"trim_ppm\":" << now.trim_ppm << ",\"overflows\":" << now.overflows
        << ",\"underruns\":" << now.underruns << ",\"xruns\":" << now.xruns << ",\"opens\":" << now.opens << '}';
    return out.str();
}

// --- Output: the MPC plays, the computer records ---

void UsbAudioOutput::start(const std::string& device) {
    stop();
    { std::lock_guard<std::mutex> lock(device_lock); requested_name = device; }
    running.store(true);
    set_state(UsbStreamState::Waiting);
    thread = std::thread([this, device] { run(device); });
}

void UsbAudioOutput::stop() {
    running.store(false);
    if (thread.joinable()) thread.join();
    accepting.store(false);
    set_state(UsbStreamState::Stopped);
    stream_rate.store(0); stream_channels.store(0); stream_width.store(0); trim_ppm.store(0);
}

std::string UsbAudioOutput::requested() const {
    std::lock_guard<std::mutex> lock(device_lock);
    return requested_name;
}

void UsbAudioOutput::push(const int32_t* frames, unsigned count, const UsbStems* stems) {
    if (!accepting.load(std::memory_order_acquire)) return;
    if (stems) count = std::min(count, usb_stem_frames);
    for (unsigned done = 0; done < count;) {
        unsigned n = std::min(count - done, usb_stem_frames);
        for (unsigned i = 0; i < n; ++i) {
            float* frame = block.data() + channels * i;
            frame[0] = float(double(frames[2 * (done + i)]) / 2147483648.0);
            frame[1] = float(double(frames[2 * (done + i) + 1]) / 2147483648.0);
            for (unsigned s = 0; s < usb_stem_count; ++s) frame[2 + s] = stems ? stems->track[s][done + i] : 0.0f;
        }
        ring.push(block.data(), n);
        done += n;
    }
}

void UsbAudioOutput::run(std::string requested) {
    raise_priority();
    constexpr unsigned chunk_frames = 32;
    Stream playback;
    SincResampler<channels> resampler; // 38 KB of history: fine on this thread's stack
    std::vector<uint8_t> raw;
    std::vector<float> staged; // Converted frames not yet encoded, `channels` samples each.
    std::array<float, channels * chunk_frames> chunk;
    snd_pcm_uframes_t pending = 0, offset = 0; // Encoded frames in `raw` still to write, and where they start.
    long open_rate = 0;
    unsigned open_device = 0;
    double fill_sum = 0; unsigned fill_count = 0;
    auto close = [&] {
        accepting.store(false, std::memory_order_release);
        ring.flush(); ring.pull(chunk.data(), 1, prime_frames); // Applies the flush: nothing is left buffered.
        if (playback.pcm) { snd_pcm_drop(playback.pcm); snd_pcm_close(playback.pcm); playback.pcm = nullptr; }
        stream_rate.store(0); stream_channels.store(0); stream_width.store(0); trim_ppm.store(0);
    };
    // One period from the ring: converted to the computer's rate, then encoded.
    auto render_period = [&] {
        while (staged.size() < channels * playback.period) {
            ring.pull(chunk.data(), chunk_frames, prime_frames); // Silence while priming.
            for (unsigned n = 0; n < chunk_frames; ++n)
                resampler.feed(chunk.data() + channels * n, [&](const float* frame) { staged.insert(staged.end(), frame, frame + channels); });
        }
        unsigned carried = std::min(playback.channels, channels);
        for (snd_pcm_uframes_t frame = 0; frame < playback.period; ++frame) {
            const float* in = staged.data() + channels * frame;
            uint8_t* bytes = raw.data() + size_t(frame) * playback.channels * playback.width;
            if (playback.channels == 1) { pcm_store(0.5f * (in[0] + in[1]), bytes, playback.width); continue; }
            for (unsigned channel = 0; channel < playback.channels; ++channel)
                pcm_store(channel < carried ? in[channel] : 0.0f, bytes + channel * playback.width, playback.width);
        }
        staged.erase(staged.begin(), staged.begin() + channels * playback.period);
        pending = playback.period; offset = 0;
    };
    // An underrun or suspend restarts the stream; anything else closes it until the next check.
    auto recover = [&](snd_pcm_sframes_t error) {
        ++xruns; pending = 0;
        if ((error != -EPIPE && error != -ESTRPIPE) || snd_pcm_prepare(playback.pcm) < 0) { close(); set_state(UsbStreamState::Failed); }
    };
    auto next_check = Clock::now();
    while (running.load()) {
        auto now = Clock::now();
        if (now >= next_check) {
            next_check = now + std::chrono::milliseconds(250);
            Target target = find_target(requested, "Playback Rate");
            { std::lock_guard<std::mutex> lock(device_lock); device_name = target.pcm; }
            if (target.pcm.empty() || target.rate == 0) {
                close(); set_state(target.pcm.empty() ? UsbStreamState::Missing : UsbStreamState::Waiting);
            } else if (playback.pcm && ((target.rate > 0 && target.rate != open_rate) || device_rate.load() != open_device)) {
                close(); // The computer or the audio device changed rate; reopen at the new one.
            }
            if (!playback.pcm && !target.pcm.empty() && target.rate != 0) {
                unsigned input_rate = device_rate.load();
                if (open_stream(target.pcm, SND_PCM_STREAM_PLAYBACK, target.rate, input_rate, playback)) {
                    open_rate = target.rate; open_device = input_rate; ++opens;
                    raw.assign(playback.period * playback.channels * playback.width, 0);
                    staged.clear(); staged.reserve(channels * (playback.period + chunk_frames * 2));
                    pending = 0; fill_sum = 0; fill_count = 0;
                    resampler.reset(input_rate, playback.rate);
                    ring.flush();
                    stream_rate.store(playback.rate); stream_channels.store(playback.channels); stream_width.store(playback.width);
                    accepting.store(true, std::memory_order_release);
                    set_state(UsbStreamState::Running);
                } else set_state(UsbStreamState::Failed);
            } else if (playback.pcm && fill_count) {
                double factor = drift_trim(fill_sum / fill_count, prime_frames);
                resampler.trim(factor);
                trim_ppm.store(int(std::lround((factor - 1.0) * 1e6)));
                fill_sum = 0; fill_count = 0;
            }
        }
        if (!playback.pcm) { std::this_thread::sleep_for(std::chrono::milliseconds(20)); continue; }
        int ready = snd_pcm_wait(playback.pcm, 50);
        // A period lasts a few milliseconds, so a 50 ms wait means the computer
        // stopped recording: check at once rather than letting the ring overflow.
        if (ready == 0) next_check = Clock::now();
        snd_pcm_sframes_t room = ready < 0 ? ready : snd_pcm_avail_update(playback.pcm);
        if (room < 0) { recover(room); continue; }
        while (room > 0 && playback.pcm) {
            if (!pending) render_period();
            auto written = snd_pcm_writei(playback.pcm, raw.data() + offset * playback.channels * playback.width,
                                          std::min<snd_pcm_uframes_t>(pending, snd_pcm_uframes_t(room)));
            if (written == -EAGAIN || written == 0) break;
            if (written < 0) { recover(written); break; }
            offset += snd_pcm_uframes_t(written); pending -= snd_pcm_uframes_t(written); room -= written;
            fill_sum += ring.buffered(); ++fill_count;
        }
    }
    close();
}

UsbStreamStats UsbAudioOutput::stats() const {
    return {state(), rate(), stream_channels.load(), stream_width.load() * 8, ring.buffered(), ring.overflows.load(),
            ring.underruns.load(), xruns.load(), opens.load(), trim_ppm.load()};
}

std::string UsbAudioOutput::json() const {
    std::string device, wanted;
    { std::lock_guard<std::mutex> lock(device_lock); device = device_name; wanted = requested_name; }
    auto now = stats();
    std::ostringstream out;
    out << "{\"state\":" << json_string(usb_stream_state_names[size_t(now.state)]) << ",\"requested\":" << json_string(wanted)
        << ",\"device\":" << json_string(device) << ",\"rate\":" << now.rate << ",\"channels\":" << now.channels
        << ",\"stems\":" << stems() << ",\"bits\":" << now.bits << ",\"buffered\":" << now.buffered << ",\"trim_ppm\":" << now.trim_ppm
        << ",\"overflows\":" << now.overflows << ",\"underruns\":" << now.underruns
        << ",\"xruns\":" << now.xruns << ",\"opens\":" << now.opens << '}';
    return out.str();
}
