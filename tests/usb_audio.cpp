// SPDX-License-Identifier: AGPL-3.0-only
// A fake UAC2 gadget card in front of ALSA's null PCM, both ways: discovery
// by driver name, waiting while the host is idle, opening at the host's rate,
// sample conversion to and from 44.1 kHz and reopening when the host changes
// rate or stops.
#include "drivers/usb_audio.h"
#include <alsa/asoundlib.h>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstring>
#include <string>
#include <thread>

static std::atomic<bool> gadget {false};
static std::atomic<long> rate {0}, playback_rate {0};
static std::atomic<unsigned> pcm_opens {0}, pcm_closes {0}, reads {0};
static std::atomic<unsigned> playback_opens {0}, playback_closes {0}, writes {0}, signal_frames {0}, wrong_frames {0};
static std::mutex name_lock;
static std::string opened, opened_playback;
static int fake_ctl = 0;

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
extern "C" int __wrap_snd_pcm_wait(snd_pcm_t*, int) { std::this_thread::sleep_for(std::chrono::milliseconds(1)); return 1; }
// Host audio: left 0.5, right -0.5, as S32_LE (the first format offered).
extern "C" snd_pcm_sframes_t __wrap_snd_pcm_readi(snd_pcm_t*, void* buffer, snd_pcm_uframes_t frames) {
    ++reads;
    auto* samples = static_cast<int32_t*>(buffer);
    for (snd_pcm_uframes_t n = 0; n < frames; ++n) { samples[2 * n] = 0x40000000; samples[2 * n + 1] = int32_t(0xc0000000u); }
    return snd_pcm_sframes_t(frames);
}
// What the host records. The MPC plays left 0.5, right -0.5: every frame is
// silence while the ring primes, that exact signal, or a ramp between the two.
extern "C" snd_pcm_sframes_t __wrap_snd_pcm_writei(snd_pcm_t*, const void* buffer, snd_pcm_uframes_t frames) {
    ++writes;
    auto* samples = static_cast<const int32_t*>(buffer);
    for (snd_pcm_uframes_t n = 0; n < frames; ++n) {
        int32_t left = samples[2 * n], right = samples[2 * n + 1];
        if (left == 0x40000000 && right == int32_t(0xc0000000u)) ++signal_frames;
        else if (left < 0 || left > 0x40000000 || std::abs(int64_t(left) + right) > 1) ++wrong_frames;
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
    assert(input.opens == 1 && input.trim_ppm == 0);
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
    assert(json.find("\"channels\":2,\"bits\":32,") != std::string::npos && json.find("\"trim_ppm\":") != std::string::npos);
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
}

int main() {
    input();
    output();
}
