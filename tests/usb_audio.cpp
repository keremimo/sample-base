// SPDX-License-Identifier: AGPL-3.0-only
// A fake UAC2 gadget card in front of ALSA's null PCM, both ways: discovery
// by driver name, waiting while the host is idle, opening at the host's rate,
// sample conversion to and from the device's rate and reopening when the host
// changes rate or stops, and an 18-channel gadget carrying the main mix and
// stems. Then the input's drift trim against a host clock running fast or
// slow, and the converter's flatness and alias rejection.
#include "drivers/usb_audio.h"
#include <alsa/asoundlib.h>
#include <algorithm>
#include <cassert>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

static std::atomic<bool> gadget {false};
static std::atomic<long> rate {0}, playback_rate {0};
static std::atomic<unsigned> pcm_opens {0}, pcm_closes {0}, reads {0};
static std::atomic<unsigned> playback_opens {0}, playback_closes {0}, writes {0}, signal_frames {0}, wrong_frames {0};
static std::atomic<unsigned> gadget_channels {0}, stem_frames {0}; // 0: the PCM's own channel count
static std::mutex name_lock;
static std::string opened, opened_playback;
static int fake_ctl = 0;
// Paced capture: the test hands the capture thread the host's frames and
// waits until it has converted and queued them, so the drift test runs both
// clocks itself, as fast as the machine allows and the same every time.
static std::atomic<bool> paced {false};
static std::mutex pace_lock;
static std::condition_variable pace_change;
static unsigned pace_frames = 0; // Handed over, not yet read
static bool pace_idle = false;   // The capture thread is back for more: all it read is in the ring

extern "C" int __wrap_snd_card_next(int* card) { *card = *card < 0 && gadget ? 0 : -1; return 0; }
extern "C" int __wrap_snd_ctl_open(snd_ctl_t** ctl, const char* name, int) {
    std::string text = name;
    if (!gadget || (text != "hw:0" && text != "hw:CARD=UAC2Gadget")) return -ENOENT;
    *ctl = reinterpret_cast<snd_ctl_t*>(&fake_ctl); return 0;
}
extern "C" int __wrap_snd_ctl_close(snd_ctl_t*) { return 0; }
extern "C" int __wrap_snd_ctl_card_info(snd_ctl_t*, snd_ctl_card_info_t*) { return 0; }
extern "C" const char* __wrap_snd_ctl_card_info_get_driver(const snd_ctl_card_info_t*) { return "UAC2_Gadget"; }
extern "C" const char* __wrap_snd_ctl_card_info_get_id(const snd_ctl_card_info_t*) { return "UAC2Gadget"; }
extern "C" int __wrap_snd_ctl_elem_read(snd_ctl_t*, snd_ctl_elem_value_t* value) {
    if (snd_ctl_elem_value_get_interface(value) != SND_CTL_ELEM_IFACE_PCM) return -ENOENT;
    const char* name = snd_ctl_elem_value_get_name(value);
    if (!strcmp(name, "Capture Rate")) snd_ctl_elem_value_set_integer(value, 0, rate);
    else if (!strcmp(name, "Playback Rate")) snd_ctl_elem_value_set_integer(value, 0, playback_rate);
    else return -ENOENT;
    return 0;
}
extern "C" int __real_snd_pcm_open(snd_pcm_t**, const char*, snd_pcm_stream_t, int);
extern "C" int __wrap_snd_pcm_open(snd_pcm_t** pcm, const char* name, snd_pcm_stream_t stream, int mode) {
    std::lock_guard<std::mutex> lock(name_lock);
    if (stream == SND_PCM_STREAM_CAPTURE) { opened = name; ++pcm_opens; }
    else { opened_playback = name; ++playback_opens; }
    return __real_snd_pcm_open(pcm, "null", stream, mode);
}
extern "C" int __real_snd_pcm_close(snd_pcm_t*);
extern "C" int __wrap_snd_pcm_close(snd_pcm_t* pcm) {
    ++(snd_pcm_stream(pcm) == SND_PCM_STREAM_CAPTURE ? pcm_closes : playback_closes);
    return __real_snd_pcm_close(pcm);
}
extern "C" int __real_snd_pcm_hw_params_get_channels_min(const snd_pcm_hw_params_t*, unsigned*);
extern "C" int __wrap_snd_pcm_hw_params_get_channels_min(const snd_pcm_hw_params_t* params, unsigned* value) {
    int result = __real_snd_pcm_hw_params_get_channels_min(params, value);
    if (result >= 0 && gadget_channels) *value = gadget_channels; // The gadget offers exactly its mask's channels.
    return result;
}
extern "C" int __wrap_snd_pcm_wait(snd_pcm_t*, int) {
    if (!paced) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    return 1;
}
// Host audio: left 0.5, right -0.5, as S32_LE (the first format offered).
extern "C" snd_pcm_sframes_t __wrap_snd_pcm_readi(snd_pcm_t*, void* buffer, snd_pcm_uframes_t frames) {
    ++reads;
    if (paced) {
        std::unique_lock<std::mutex> lock(pace_lock);
        pace_idle = true; pace_change.notify_all();
        if (!pace_change.wait_for(lock, std::chrono::milliseconds(10), [] { return pace_frames > 0; })) return -EAGAIN;
        pace_idle = false;
        frames = std::min<snd_pcm_uframes_t>(frames, pace_frames);
        pace_frames -= unsigned(frames);
    }
    auto* samples = static_cast<int32_t*>(buffer);
    for (snd_pcm_uframes_t n = 0; n < frames; ++n) { samples[2 * n] = 0x40000000; samples[2 * n + 1] = int32_t(0xc0000000u); }
    return snd_pcm_sframes_t(frames);
}
// What the host records. The MPC plays left 0.5, right -0.5: every frame is
// silence while the ring primes, that signal (to float precision), or the
// converter's band-limited step between the two, which rings a little past
// both. With 18 channels, stem s plays (s + 1) / 32 after them.
static bool near_s32(int32_t sample, double value) { return std::abs(sample - value * 2147483648.0) < 1024; }
extern "C" snd_pcm_sframes_t __wrap_snd_pcm_writei(snd_pcm_t*, const void* buffer, snd_pcm_uframes_t frames) {
    ++writes;
    unsigned stride = gadget_channels ? gadget_channels.load() : 2;
    auto* samples = static_cast<const int32_t*>(buffer);
    for (snd_pcm_uframes_t n = 0; n < frames; ++n) {
        const int32_t* frame = samples + stride * n;
        int32_t left = frame[0], right = frame[1];
        if (near_s32(left, 0.5) && near_s32(right, -0.5)) {
            ++signal_frames;
            bool stems = stride > 2;
            for (unsigned s = 0; s + 2 < stride; ++s) stems = stems && near_s32(frame[2 + s], (s + 1) / 32.0);
            if (stems) ++stem_frames;
        }
        else if (left < -0x10000000 || left > 0x58000000 || std::abs(int64_t(left) + right) > 1) ++wrong_frames;
    }
    return snd_pcm_sframes_t(frames);
}

template <class Condition> static void wait_for(Condition condition) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!condition()) {
        assert(std::chrono::steady_clock::now() < deadline);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

static void input() {
    UsbAudioInput usb;
    std::array<float, 256> frames {};
    assert(usb.state() == UsbStreamState::Stopped && !usb.pull(frames.data(), 128));

    usb.start("auto");
    wait_for([&] { return usb.state() == UsbStreamState::Missing; });
    usb.stop();
    assert(usb.state() == UsbStreamState::Stopped && pcm_opens == 0);

    gadget = true;
    usb.start("auto");
    wait_for([&] { return usb.state() == UsbStreamState::Waiting && usb.json().find("hw:CARD=UAC2Gadget,DEV=0") != std::string::npos; });
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    assert(pcm_opens == 0); // Nothing is opened while the host is idle.

    rate = 48000;
    wait_for([&] { return usb.state() == UsbStreamState::Running; });
    assert(usb.rate() == 48000 && usb.channels() == 2 && usb.bits() == 32 && pcm_opens == 1 && playback_opens == 0);
    auto input = usb.stats();
    assert(input.state == UsbStreamState::Running && input.rate == 48000 && input.channels == 2 && input.bits == 32);
    assert(input.opens == 1 && usb.json().find("\"trim_ppm\":") != std::string::npos);
    { std::lock_guard<std::mutex> lock(name_lock); assert(opened == "hw:CARD=UAC2Gadget,DEV=0"); }
    wait_for([&] { return usb.pull(frames.data(), 128); });
    for (unsigned n = 0; n < 128; ++n) assert(std::abs(frames[2 * n] - 0.5f) < 1e-6f && std::abs(frames[2 * n + 1] + 0.5f) < 1e-6f);

    rate = 44100; // The host switched rate: reopen at the new one.
    wait_for([&] { return usb.rate() == 44100; });
    assert(pcm_opens == 2 && pcm_closes == 1);

    rate = 0; // The host stopped streaming.
    wait_for([&] { return usb.state() == UsbStreamState::Waiting; });
    assert(pcm_closes == 2 && usb.rate() == 0);

    rate = 44100;
    wait_for([&] { return usb.state() == UsbStreamState::Running; });
    usb.stop();
    assert(usb.state() == UsbStreamState::Stopped && pcm_opens == 3 && pcm_closes == 3 && reads > 0);
    rate = 0;
}

static void output() {
    UsbAudioOutput usb;
    std::array<int32_t, 2 * 128> block;
    for (unsigned n = 0; n < 128; ++n) { block[2 * n] = 0x40000000; block[2 * n + 1] = int32_t(0xc0000000u); }
    assert(usb.state() == UsbStreamState::Stopped && usb.requested().empty());

    gadget = false;
    usb.start("auto");
    wait_for([&] { return usb.state() == UsbStreamState::Missing; });
    assert(usb.requested() == "auto");

    gadget = true;
    wait_for([&] { return usb.state() == UsbStreamState::Waiting; });
    for (unsigned n = 0; n < 16; ++n) usb.push(block.data(), 128); // Nobody is recording: dropped.
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    assert(playback_opens == 0 && usb.json().find("\"buffered\":0,") != std::string::npos);

    playback_rate = 48000; // The host starts recording.
    wait_for([&] { return usb.state() == UsbStreamState::Running; });
    assert(usb.rate() == 48000 && playback_opens == 1 && pcm_opens == 3);
    { std::lock_guard<std::mutex> lock(name_lock); assert(opened_playback == "hw:CARD=UAC2Gadget,DEV=0"); }
    auto json = usb.json();
    assert(json.find("\"channels\":2,\"stems\":0,\"bits\":32,") != std::string::npos && json.find("\"trim_ppm\":") != std::string::npos);
    auto output = usb.stats(); // What the system page shows: the same counters as json().
    assert(output.state == UsbStreamState::Running && output.rate == 48000 && output.channels == 2 && output.bits == 32);
    assert(output.opens == 1);
    wait_for([&] { usb.push(block.data(), 128); return signal_frames > 0; });
    assert(writes > 0 && wrong_frames == 0);

    playback_rate = 44100; // A new rate: reopen, converting 1:1.
    wait_for([&] { return usb.rate() == 44100; });
    assert(playback_opens == 2 && playback_closes == 1);

    playback_rate = 0; // The host stopped recording: closed with nothing left buffered.
    wait_for([&] { return usb.state() == UsbStreamState::Waiting; });
    assert(playback_closes == 2 && usb.rate() == 0 && usb.json().find("\"buffered\":0,") != std::string::npos);

    playback_rate = 48000;
    wait_for([&] { return usb.state() == UsbStreamState::Running; });
    usb.stop();
    assert(usb.state() == UsbStreamState::Stopped && playback_opens == 3 && playback_closes == 3 && usb.requested() == "auto");
    auto stopped = usb.json();
    usb.push(block.data(), 128); // Stopped: dropped.
    assert(usb.json() == stopped);

    // A named PCM without a rate control opens at 44.1 kHz straight away.
    gadget = false;
    usb.start("plughw:Other");
    wait_for([&] { return usb.state() == UsbStreamState::Running; });
    assert(usb.rate() == 44100 && usb.requested() == "plughw:Other");
    { std::lock_guard<std::mutex> lock(name_lock); assert(opened_playback == "plughw:Other"); }
    usb.stop();
    assert(playback_closes == 4 && wrong_frames == 0);

    // The gadget widened for stems: main mix on 1-2, track s on 3 + s.
    UsbStems stems {};
    for (unsigned s = 0; s < usb_stem_count; ++s)
        for (auto& sample : stems.track[s]) sample = float(s + 1) / 32;
    gadget = true; gadget_channels = 18; playback_rate = 44100;
    usb.start("auto");
    wait_for([&] { return usb.state() == UsbStreamState::Running; });
    assert(usb.stats().channels == 18 && usb.stems() == 16 && usb.json().find("\"channels\":18,\"stems\":16,") != std::string::npos);
    wait_for([&] { usb.push(block.data(), 128, &stems); return stem_frames > 0; });
    usb.stop();
    assert(wrong_frames == 0);
    gadget_channels = 0; playback_rate = 0;
}

// Hands the capture thread `frames` of host audio and waits until they are
// converted and in the ring.
static void deliver(unsigned frames) {
    std::unique_lock<std::mutex> lock(pace_lock);
    pace_frames += frames; pace_idle = false;
    pace_change.notify_all();
    pace_change.wait(lock, [] { return pace_frames == 0 && pace_idle; });
}

// Two minutes of a host whose clock runs `ppm` fast (slow below 0) against
// the device's 44.1 kHz, a period at a time, read by an audio thread taking
// 128 frames as each falls due. Untrimmed, +1500 ppm fills the ring by 66
// frames a second until it overflows after about 105 s, and -1000 ppm drains
// it in 17 s and then re-primes every 17 s. Trimmed, the ring never runs dry
// or overflows, and over the second minute the trim averages the drift and
// the fill stays near 1,024 + ppm / 2 frames (as the capture thread pushes;
// a period less once the audio thread has taken its share). Each 250 ms trim
// wanders by up to ±150 ppm, as the audio thread's 128-frame reads beat
// against the pushes: well under a cent, and the average is what matters.
static void input_drift(double ppm) {
    UsbAudioInput usb;
    gadget = true; rate = 44100; paced = true;
    usb.start("auto");
    wait_for([&] { return usb.state() == UsbStreamState::Running; });
    const double speed = 1 + ppm * 1e-6;
    std::array<float, 2 * 128> block {};
    double owed = 0, trims = 0; // Device frames due to the audio thread; the second minute's trims added up
    unsigned low = ~0u, high = 0, samples = 0;
    for (double host = 0; host < 120 * 44100 * speed; host += 256) {
        deliver(256);
        owed += 256 / speed;
        for (; owed >= 128; owed -= 128) usb.pull(block.data(), 128);
        if (host > 60 * 44100 * speed) {
            auto now = usb.stats();
            low = std::min(low, now.buffered); high = std::max(high, now.buffered);
            trims += now.trim_ppm; ++samples;
            assert(std::abs(now.trim_ppm - ppm) < 150);
        }
    }
    auto settled = usb.stats();
    usb.stop();
    paced = false; rate = 0;
    double trim = trims / samples, target = 1024 + ppm / 2;
    std::printf("drift %+.0f ppm: trim %+.1f ppm on average, fill %u..%u (target %.0f), overflows %u, underruns %u\n", ppm,
                trim, low, high, target, settled.overflows, settled.underruns);
    assert(settled.overflows == 0 && settled.underruns == 0);
    assert(std::abs(trim - ppm) < 10);
    assert(low > target - 450 && high < target + 150);
}

// A least-squares fit of a cos + b sin + c, `step` radians a sample: the
// tone's amplitude and the rms of everything else.
struct Tone { double amplitude, residual; };
static Tone fit_tone(const float* samples, size_t count, double step) {
    double m[3][4] {};
    for (size_t n = 0; n < count; ++n) {
        const double basis[3] {std::cos(step * n), std::sin(step * n), 1.0};
        for (unsigned r = 0; r < 3; ++r) {
            for (unsigned c = 0; c < 3; ++c) m[r][c] += basis[r] * basis[c];
            m[r][3] += basis[r] * samples[n];
        }
    }
    for (unsigned i = 0; i < 3; ++i)
        for (unsigned k = i + 1; k < 3; ++k) {
            double f = m[k][i] / m[i][i];
            for (unsigned j = i; j < 4; ++j) m[k][j] -= f * m[i][j];
        }
    double v[3];
    for (int i = 2; i >= 0; --i) {
        v[i] = m[i][3];
        for (int j = i + 1; j < 3; ++j) v[i] -= m[i][j] * v[j];
        v[i] /= m[i][i];
    }
    double error = 0;
    for (size_t n = 0; n < count; ++n) {
        double left = samples[n] - (v[0] * std::cos(step * n) + v[1] * std::sin(step * n) + v[2]);
        error += left * left;
    }
    return {std::hypot(v[0], v[1]), std::sqrt(error / count)};
}

static double db(double ratio) { return 20 * std::log10(ratio); }
constexpr double pi = 3.14159265358979323846;

// Equal rates trimmed by 50 ppm, as the drift trim leaves them: each output
// frame falls a little further between two input frames, sweeping through
// every position each 0.45 s. A linear converter makes 15 kHz swing between
// -0.4 and -6.3 dB, and 20 kHz down to -16 dB; this one holds both flat.
static void flatness() {
    for (double hz : {15000.0, 20000.0}) {
        SincResampler<2> usb; usb.reset(44100, 44100); usb.trim(1 + 50e-6);
        std::vector<float> out;
        for (unsigned n = 0; n < 44100 * 33 / 10; ++n) {
            float sample = float(0.5 * std::sin(2 * pi * hz * n / 44100));
            usb.feed(sample, -sample, [&](float left, float right) { assert(left == -right); out.push_back(left); });
        }
        double low = 0, high = -100;
        for (size_t start = 4410; start + 2205 <= out.size(); start += 2205) { // 3 s in 50 ms blocks
            double level = db(fit_tone(out.data() + start, 2205, 2 * pi * hz * (1 + 50e-6) / 44100).amplitude / 0.5);
            low = std::min(low, level); high = std::max(high, level);
        }
        std::printf("flatness %.0f Hz: %+.4f..%+.4f dB\n", hz, low, high);
        assert(low > -0.01 && high < 0.01);
    }
}

// 48 kHz from a computer down to 44.1 kHz, and 44.1 up to 48: whatever the
// conversion folds or images back next to a tone is at least 90 dB under it.
// A linear converter leaves 10 kHz at 48 -> 44.1 an alias 24 dB down at
// 6.1 kHz, and 18 kHz one 13 dB down at 14.1 kHz.
static void alias_rejection() {
    const unsigned pairs[][2] {{48000, 44100}, {44100, 48000}};
    for (auto& rates : pairs) {
        double worst = -200, worst_hz = 0;
        for (double hz : {1000.0, 6100.0, 10000.0, 14100.0, 18000.0, 19200.0, 20000.0}) {
            SincResampler<2> usb; usb.reset(rates[0], rates[1]);
            std::vector<float> out;
            for (unsigned n = 0; n < rates[0] * 3 / 4; ++n) {
                float sample = float(0.5 * std::sin(2 * pi * hz * n / rates[0]));
                usb.feed(sample, sample, [&](float left, float) { out.push_back(left); });
            }
            Tone tone = fit_tone(out.data() + rates[1] / 10, rates[1] / 2, 2 * pi * hz / rates[1]);
            double aliases = db(tone.residual / (0.5 / std::sqrt(2.0)));
            assert(std::abs(db(tone.amplitude / 0.5)) < 0.01 && aliases < -90);
            if (aliases > worst) { worst = aliases; worst_hz = hz; }
        }
        std::printf("aliases %u -> %u: %.1f dB at worst (%.0f Hz)\n", rates[0], rates[1], worst, worst_hz);
    }
}

int main() {
    std::setvbuf(stdout, nullptr, _IOLBF, 1024); // Each measurement shows even if an assertion then fails.
    input();
    output();
    input_drift(1500);
    input_drift(-1000);
    flatness();
    alias_rejection();
}
