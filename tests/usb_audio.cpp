// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// A fake UAC2 gadget card in front of ALSA's null PCM, both ways: discovery
// by driver name, waiting while the host is idle, opening at the host's rate,
// sample conversion to and from the device's rate and reopening when the host
// changes rate or stops, and an 18-channel gadget carrying the main mix and
// stems. Then the input against a host clock running fast or slow: locked
// through the gadget's feedback, trimmed without it, and unlocked when the
// host ignores it. Last, the converter's flatness and alias rejection.
#include <cerrno>
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
// The gadget's feedback control, when it has one (asynchronous mode): what
// was last written, and how many writes.
static std::atomic<bool> pitch_control {false};
static std::atomic<long> pitch {1000000}, first_pitch {0};
static std::atomic<unsigned> pitch_writes {0};
// Its playback pitch control the same way: the size of the packets it sends.
static std::atomic<bool> playback_pitch_control {false};
static std::atomic<long> playback_pitch {1000000}, first_playback_pitch {0};
static std::atomic<unsigned> playback_pitch_writes {0};
// Paced playback: the test hands the playback thread the room the gadget
// has sent on to the host and waits until it has filled it, so the drift
// test runs both clocks itself.
static std::atomic<bool> paced_out {false};
static unsigned out_room = 0; // Under pace_lock: frames the playback thread may write
static bool out_idle = false; // The playback thread is back waiting: all it wrote is counted

extern "C" int __wrap_snd_card_next(int* card) { *card = *card < 0 && gadget ? 0 : -1; return 0; }
// No control events on the fake card: the output thread falls back to its 250 ms checks.
extern "C" int __wrap_snd_ctl_subscribe_events(snd_ctl_t*, int) { return -ENOSYS; }
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
extern "C" int __wrap_snd_ctl_elem_write(snd_ctl_t*, snd_ctl_elem_value_t* value) {
    if (snd_ctl_elem_value_get_interface(value) != SND_CTL_ELEM_IFACE_PCM) return -ENOENT;
    const char* name = snd_ctl_elem_value_get_name(value);
    if (playback_pitch_control && !strcmp(name, "Playback Pitch 1000000")) {
        playback_pitch = snd_ctl_elem_value_get_integer(value, 0);
        if (playback_pitch_writes++ == 0) first_playback_pitch = playback_pitch.load();
        return 0;
    }
    if (!pitch_control || strcmp(name, "Capture Pitch 1000000")) return -ENOENT;
    pitch = snd_ctl_elem_value_get_integer(value, 0);
    if (pitch_writes++ == 0) first_pitch = pitch.load();
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
extern "C" int __wrap_snd_pcm_wait(snd_pcm_t* pcm, int) {
    if (paced_out && snd_pcm_stream(pcm) == SND_PCM_STREAM_PLAYBACK) {
        std::unique_lock<std::mutex> lock(pace_lock);
        out_idle = true; pace_change.notify_all();
        if (!pace_change.wait_for(lock, std::chrono::milliseconds(10), [] { return out_room > 0; })) return 0;
        out_idle = false;
        return 1;
    }
    if (!paced) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    return 1;
}
extern "C" snd_pcm_sframes_t __real_snd_pcm_avail_update(snd_pcm_t*);
extern "C" snd_pcm_sframes_t __wrap_snd_pcm_avail_update(snd_pcm_t* pcm) {
    if (!paced_out || snd_pcm_stream(pcm) != SND_PCM_STREAM_PLAYBACK) return __real_snd_pcm_avail_update(pcm);
    std::lock_guard<std::mutex> lock(pace_lock);
    return snd_pcm_sframes_t(out_room);
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
    if (paced_out) {
        std::lock_guard<std::mutex> lock(pace_lock);
        frames = std::min<snd_pcm_uframes_t>(frames, out_room);
        out_room -= unsigned(frames);
        if (!frames) return -EAGAIN;
    }
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
    assert(!input.locked && input.target == UsbAudioInput::prime_frames && usb.json().find("\"locked\":false,") != std::string::npos);
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

    // With the gadget's feedback control the input locks at once, at the
    // nominal pitch, and at the device's own rate every frame passes
    // untouched. Stopping leaves the pitch nominal for whoever streams next.
    pitch_control = true; pitch = 0; pitch_writes = 0; rate = 44100;
    usb.start("auto");
    wait_for([&] { return usb.state() == UsbStreamState::Running; });
    auto locked = usb.stats();
    assert(locked.locked && first_pitch == 1000000 && locked.target == UsbAudioInput::locked_target(44100) && locked.target == 280);
    auto json = usb.json();
    assert(json.find("\"locked\":true,") != std::string::npos && json.find("\"target\":280,") != std::string::npos);
    wait_for([&] { return usb.pull(frames.data(), 128); });
    for (unsigned n = 0; n < 128; ++n) assert(frames[2 * n] == 0.5f && frames[2 * n + 1] == -0.5f);
    pitch = 0;
    usb.stop();
    assert(pitch == 1000000 && !usb.stats().locked && pcm_opens == 4 && pcm_closes == 4);
    pitch_control = false; rate = 0;
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
    assert(usb.rate() == 48000 && playback_opens == 1 && pcm_opens == 4);
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

    // With the gadget's playback pitch control the output locks at once, at
    // the nominal pitch, its ring held at a block, a period and 2 ms; at the
    // device's rate the frames pass untouched, at another they are converted
    // at a fixed ratio. Stopping leaves the pitch nominal.
    playback_pitch_control = true; playback_pitch = 0; playback_pitch_writes = 0; stem_frames = 0;
    for (unsigned host : {44100u, 48000u}) {
        playback_rate = host;
        usb.start("auto");
        wait_for([&] { return usb.state() == UsbStreamState::Running; });
        auto locked = usb.stats();
        assert(locked.locked && first_playback_pitch == 1000000 && locked.trim_ppm == 0);
        assert(locked.target == UsbAudioOutput::locked_target(44100, 256) && locked.target == 472); // The null PCM's period
        auto locked_json = usb.json();
        assert(locked_json.find("\"locked\":true,\"pitch_ppm\":0,\"target\":472,") != std::string::npos);
        unsigned before = stem_frames;
        wait_for([&] { usb.push(block.data(), 128, &stems); return stem_frames > before; });
        playback_pitch = 0;
        usb.stop();
        assert(playback_pitch == 1000000 && !usb.stats().locked && wrong_frames == 0);
    }
    playback_pitch_control = false;
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
// the device's 44.1 kHz, `chunk` frames at a time, read by an audio thread
// taking 128 frames as each falls due. A host that `follows` the gadget's
// feedback sends at the pitch last written. What the second minute saw: the
// trim and pitch on average, and the ring's fill after each chunk's reads.
struct Drift { double trim = 0, pitch = 0; unsigned low = ~0u, high = 0; UsbStreamStats settled; };
static Drift drift(double ppm, unsigned chunk, bool follows) {
    UsbAudioInput usb;
    gadget = true; rate = 44100; paced = true; pitch = 1000000;
    usb.start("auto");
    wait_for([&] { return usb.state() == UsbStreamState::Running; });
    const double speed = 1 + ppm * 1e-6;
    std::array<float, 2 * 128> block {};
    double owed = 0, trims = 0, pitches = 0; // Device frames due to the audio thread; the second minute's sums
    unsigned samples = 0;
    Drift result;
    for (double host = 0; host < 120 * 44100 * speed; host += chunk) {
        deliver(chunk);
        owed += chunk / (speed * (follows ? pitch * 1e-6 : 1.0));
        for (; owed >= 128; owed -= 128) usb.pull(block.data(), 128);
        if (host > 60 * 44100 * speed) {
            auto now = usb.stats();
            result.low = std::min(result.low, now.buffered); result.high = std::max(result.high, now.buffered);
            trims += now.trim_ppm; pitches += now.pitch_ppm; ++samples;
            if (!now.locked) assert(std::abs(now.trim_ppm - ppm) < 150);
        }
    }
    result.settled = usb.stats();
    usb.stop();
    paced = false; rate = 0;
    result.trim = trims / samples; result.pitch = pitches / samples;
    return result;
}

// Without the feedback control the trim takes up the drift. Untrimmed, +1500
// ppm fills the ring by 66 frames a second until it overflows after about
// 105 s, and -1000 ppm drains it in 17 s and then re-primes every 17 s.
// Trimmed, the ring never runs dry or overflows, and over the second minute
// the trim averages the drift and the fill stays near 1,024 + ppm / 2 frames
// (as the capture thread pushes; a period less once the audio thread has
// taken its share). Each 250 ms trim wanders by up to ±150 ppm, as the audio
// thread's 128-frame reads beat against the pushes: well under a cent, and
// the average is what matters.
static void input_drift(double ppm) {
    auto run = drift(ppm, 256, false);
    double target = 1024 + ppm / 2;
    std::printf("drift %+.0f ppm: trim %+.1f ppm on average, fill %u..%u (target %.0f), overflows %u, underruns %u\n", ppm,
                run.trim, run.low, run.high, target, run.settled.overflows, run.settled.underruns);
    assert(!run.settled.locked && run.settled.overflows == 0 && run.settled.underruns == 0);
    assert(std::abs(run.trim - ppm) < 10);
    assert(run.low > target - 450 && run.high < target + 150);
}

// With it the host follows the pitch, a 64-frame period at a time as on the
// MPC: over the second minute the pitch averages the host's drift the other
// way (within 5 %: here the fill moves only in whole periods, so the lock
// hunts between two of them), nothing is trimmed, and the ring stays within a
// few hundred frames of empty without running dry, where trimming holds it
// near 1,024.
static void input_locked(double ppm) {
    pitch_control = true;
    auto run = drift(ppm, 64, true);
    pitch_control = false;
    double expected = (1 / (1 + ppm * 1e-6) - 1) * 1e6;
    std::printf("locked %+.0f ppm: pitch %+.1f ppm on average (%+.1f wanted), fill %u..%u (target %u), lowest %u, overflows %u, underruns %u\n",
                ppm, run.pitch, expected, run.low, run.high, run.settled.target, run.settled.lowest, run.settled.overflows,
                run.settled.underruns);
    assert(run.settled.locked && run.settled.trim_ppm == 0 && run.trim == 0);
    assert(run.settled.overflows == 0 && run.settled.underruns == 0);
    assert(std::abs(run.pitch - expected) < std::max(10.0, std::abs(ppm) / 20)); // The fixture's fill moves in 64-frame steps
    assert(run.settled.lowest > 0 && run.high < 400);
}

// A host that takes no notice of the pitch, or one too far off for the lock
// to catch before the ring strays (USB 2.0 asks a high-speed host for
// ±500 ppm): the ring drifts off its target, the input unlocks within
// seconds, the pitch goes back to nominal, and from there it is trimmed as
// without the control, never running dry.
static void input_unlocked(double ppm, bool follows) {
    pitch_control = true;
    auto run = drift(ppm, 64, follows);
    pitch_control = false;
    double target = 1024 + ppm / 2;
    std::printf("%s %+.0f ppm: unlocked, trim %+.1f ppm on average, fill %u..%u (target %.0f), overflows %u, underruns %u\n",
                follows ? "too far" : "ignored", ppm, run.trim, run.low, run.high, target, run.settled.overflows, run.settled.underruns);
    assert(!run.settled.locked && pitch == 1000000 && run.settled.target == UsbAudioInput::prime_frames);
    assert(run.settled.overflows == 0 && run.settled.underruns == 0);
    assert(std::abs(run.trim - ppm) < 10);
    assert(run.low > target - 450 && run.high < target + 150);
}

// Hands the playback thread `frames` of room, as the gadget sends them on to
// the host, and waits until it has written them.
static void send(unsigned frames) {
    std::unique_lock<std::mutex> lock(pace_lock);
    out_room += frames; out_idle = false;
    pace_change.notify_all();
    pace_change.wait(lock, [] { return out_room == 0 && out_idle; });
}

// Two minutes of a host whose clock runs `ppm` fast (slow below 0) against
// the device's 44.1 kHz, the gadget sending it 64 frames at a time, each
// sized by the pitch last written when it `follows` it; an audio thread
// pushes 128 frames as each falls due. What the second minute saw, as in
// drift().
static Drift output_drift(double ppm, bool follows) {
    UsbAudioOutput usb;
    gadget = true; gadget_channels = 18; playback_rate = 44100; paced_out = true;
    playback_pitch_control = true; playback_pitch = 1000000;
    usb.start("auto");
    wait_for([&] { return usb.state() == UsbStreamState::Running; });
    const double speed = 1 + ppm * 1e-6;
    std::array<int32_t, 2 * 128> block;
    for (unsigned n = 0; n < 128; ++n) { block[2 * n] = 0x40000000; block[2 * n + 1] = int32_t(0xc0000000u); }
    double owed = 0, trims = 0, pitches = 0; // Device frames due to the audio thread; the second minute's sums
    unsigned samples = 0, chunk = 64;
    Drift result;
    for (double host = 0; host < 120 * 44100 * speed; host += chunk) {
        owed += chunk / (speed * (follows ? playback_pitch * 1e-6 : 1.0));
        for (; owed >= 128; owed -= 128) usb.push(block.data(), 128);
        send(chunk);
        if (host > 60 * 44100 * speed) {
            auto now = usb.stats();
            result.low = std::min(result.low, now.buffered); result.high = std::max(result.high, now.buffered);
            trims += now.trim_ppm; pitches += now.pitch_ppm; ++samples;
        }
    }
    result.settled = usb.stats();
    usb.stop();
    paced_out = false; playback_pitch_control = false; gadget_channels = 0; playback_rate = 0;
    result.trim = trims / samples; result.pitch = pitches / samples;
    return result;
}

// Locked, the gadget's pitch averages the host's drift the other way (the
// gadget sends faster to a slow host), nothing is converted or trimmed, and
// the ring stays near its target, a few hundred frames, without running dry.
static void output_locked(double ppm) {
    auto run = output_drift(ppm, true);
    double expected = (1 / (1 + ppm * 1e-6) - 1) * 1e6;
    std::printf("output locked %+.0f ppm: pitch %+.1f ppm on average (%+.1f wanted), fill %u..%u (target %u), lowest %u, overflows %u, underruns %u\n",
                ppm, run.pitch, expected, run.low, run.high, run.settled.target, run.settled.lowest, run.settled.overflows,
                run.settled.underruns);
    assert(run.settled.locked && run.settled.trim_ppm == 0 && run.trim == 0);
    assert(run.settled.overflows == 0 && run.settled.underruns == 0);
    assert(std::abs(run.pitch - expected) < std::max(10.0, std::abs(ppm) / 20));
    assert(run.settled.lowest > 0 && run.high < run.settled.target + 400);
}

// A gadget whose packets take no notice of the pitch: the ring drifts off
// its target, the output unlocks within seconds, the pitch goes back to
// nominal, and from there the conversion is trimmed (by the drift the other
// way) as without the control, never running dry.
static void output_ignored(double ppm) {
    auto run = output_drift(ppm, false);
    double target = 1024 - ppm / 2;
    std::printf("output ignored %+.0f ppm: unlocked, trim %+.1f ppm on average, fill %u..%u (target %.0f), overflows %u, underruns %u\n",
                ppm, run.trim, run.low, run.high, target, run.settled.overflows, run.settled.underruns);
    assert(!run.settled.locked && playback_pitch == 1000000 && run.settled.target == UsbAudioOutput::prime_frames);
    assert(run.settled.overflows == 0 && run.settled.underruns == 0);
    assert(std::abs(run.trim + ppm) < 10);
    assert(run.low > target - 450 && run.high < target + 450);
}

// A channel exactly silent for the whole kernel is skipped and gives exact
// silence; from the first sound it is converted like any other. Here
// channel 1 gets an impulse after silence and channel 0 the same over a
// constant 0.25, which DC passes exactly, so the two differ by 0.25 in every
// output frame, the impulse's first and last taps among them.
static void silent_channels() {
    SincResampler<2> usb; usb.reset(44100, 48000);
    unsigned frames = 0, sounding = 0;
    for (unsigned n = 0; n < 2000; ++n) {
        float impulse = n == 700 || n == 1300 ? 1.0f : 0.0f;
        const float frame[2] {0.25f + impulse, impulse};
        usb.feed(frame, [&](const float* out) {
            assert(std::abs(out[0] - 0.25f - out[1]) < 1e-6f);
            if (out[1] != 0.0f) ++sounding;
            ++frames;
        });
    }
    assert(sounding > 2 * 60 && sounding < 2 * 80); // About 64 taps' worth of output frames per impulse
    std::printf("silent channels: %u of %u frames sounding\n", sounding, frames);
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
    input_locked(48);
    input_locked(-48);
    input_locked(500);
    input_locked(-500);
    input_unlocked(1000, false);
    input_unlocked(-300, false);
    input_unlocked(1500, true);
    output_locked(48);
    output_locked(-48);
    output_locked(500);
    output_locked(-500);
    output_ignored(300);
    output_ignored(-300);
    flatness();
    alias_rejection();
    silent_channels();
}
