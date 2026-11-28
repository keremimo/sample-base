// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "dsp/frame_ring.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

// Audio between the MPC and a computer over USB-C. The kernel's UAC2 gadget
// function (drivers/usb/gadget/function/f_uac2.c and u_audio.c) registers an
// ALSA card whose driver is "UAC2_Gadget" with one "UAC2 PCM" device. Its
// capture stream is what the computer plays; its playback stream is what the
// computer records. Each direction's sample size (S16_LE, S24_3LE or S32_LE)
// and channel count are fixed by the gadget configuration, its rate is
// whatever the computer chose, and the PCM-interface controls "Capture Rate"
// and "Playback Rate" read 0 while the computer is not streaming that way.
// On firmware 1.3.0.12 both directions are stereo S32_LE; widen_uac2_playback()
// (drivers/usb_gadget.h) gives the computer-bound side room for stems.
//
// The computer's USB clock and the AK4558's drift apart, so each
// direction runs its own thread with a FrameRing to the audio thread. The
// input asks the computer, through the gadget's feedback ("Capture Pitch
// 1000000"), to send at the AK4558's pace, and the output sizes the gadget's
// packets ("Playback Pitch 1000000") to the same pace, which keeps each ring
// a few milliseconds deep; without those controls, or with a computer that
// ignores the feedback, a SincResampler that drift_trim() nudges keeps the
// ring near `prime_frames`. Neither opens anything until the computer
// streams.

struct Uac2Gadget { std::string pcm, control; };
// The first UAC2 gadget card, or empty names without one.
Uac2Gadget find_uac2_gadget();
// A "Capture Rate" or "Playback Rate" control: the computer's current rate,
// 0 while it is not streaming, or -1 without the control.
long uac2_rate(const std::string& control, const char* name);

enum class UsbStreamState : uint8_t { Stopped, Missing, Waiting, Running, Failed, Count };
constexpr std::array<const char*, size_t(UsbStreamState::Count)> usb_stream_state_names {{"stopped", "missing", "waiting", "running", "failed"}};

// One direction's counters, as json() reports them. Any thread may read
// them: every field comes from an atomic. `trim_ppm` is the direction's drift
// trim: how much faster than the two rates' ratio its converter reads input,
// in parts per million. `locked` while the computer's frames follow the
// AK4558 through the gadget's pitch controls, `pitch_ppm` how much faster
// than its nominal rate the computer is asked to send (input) or the gadget
// sends (output), `target` the fill the ring is held at (and primes to), and
// `lowest` the least the ring held as frames came in (input) or went out
// (output) since the stream settled, 2 seconds after it opened: the margin
// left before it would run dry.
struct UsbStreamStats {
    UsbStreamState state = UsbStreamState::Stopped;
    unsigned rate = 0, channels = 0, bits = 0, buffered = 0, overflows = 0, underruns = 0, xruns = 0, opens = 0;
    int trim_ppm = 0;
    bool locked = false;
    int pitch_ppm = 0;
    unsigned target = 0, lowest = 0;
};

// What a computer plays to the MPC. start() opens nothing on the calling
// thread: a separate thread finds the gadget, waits for the computer, reads
// its stream a `period_frames` period at a time as stereo float at the audio
// device's rate, and hands it to the audio thread through a ring.
//
// With the gadget's feedback control the computer is locked to the AK4558:
// after every 250 ms of its audio, PitchLock turns the ring's average fill
// meanwhile into the pitch the computer is asked to send at, holding the ring
// at locked_target(), one audio block, one period and 2 ms. At equal rates
// (the gadget follows the device's rate) the frames pass untouched; at
// others a resampler converts them at a fixed ratio. A computer that ignores
// the feedback leaves the ring drifting off its target: after a second more
// than a third of the target away, the input unlocks and trims as below.
//
// Without the control the resampler converts every frame, and drift_trim()
// nudges it by the ring's average fill so the ring settles near
// `prime_frames` instead of running dry or filling up as the two clocks
// drift apart. Either way, should the ring run dry it re-primes to its
// target. Nothing is opened unless an application selects USB input.
class UsbAudioInput {
public:
    static constexpr unsigned ring_frames = 8192, prime_frames = 1024, period_frames = 64;
    // The ring's fill while locked: the audio thread's 128-frame block, one
    // period and 2 ms to spare (280 frames at 44.1 kHz, 288 at 48, 384 at 96).
    static constexpr unsigned locked_target(unsigned rate) { return 128 + period_frames + rate / 500; }

    UsbAudioInput() = default;
    ~UsbAudioInput() { stop(); }
    UsbAudioInput(const UsbAudioInput&) = delete;
    UsbAudioInput& operator=(const UsbAudioInput&) = delete;

    // "auto" finds the UAC2 gadget card; any other value is an ALSA PCM name
    // whose rate is negotiated directly. Called from one control thread.
    void start(const std::string& device);
    void stop();
    // Audio thread: `count` stereo frames, silent until enough are buffered.
    bool pull(float* frames, unsigned count) { return ring.pull(frames, count, fill_target.load(std::memory_order_relaxed)); }
    // Any thread: the audio device's rate, which pull() delivers (44.1 kHz
    // until told). A change reopens the stream within 250 ms.
    void set_device_rate(unsigned rate) { device_rate.store(rate, std::memory_order_relaxed); }

    UsbStreamState state() const { return UsbStreamState(current.load(std::memory_order_acquire)); }
    unsigned rate() const { return stream_rate.load(std::memory_order_relaxed); }
    unsigned channels() const { return stream_channels.load(std::memory_order_relaxed); }
    unsigned bits() const { return stream_width.load(std::memory_order_relaxed) * 8; }
    UsbStreamStats stats() const;
    std::string json() const;

private:
    void run(std::string device);
    void set_state(UsbStreamState state) { current.store(uint8_t(state), std::memory_order_release); }
    FrameRing<ring_frames> ring;
    std::thread thread;
    std::atomic<bool> running {false}, locked {false};
    std::atomic<uint8_t> current {uint8_t(UsbStreamState::Stopped)};
    std::atomic<unsigned> stream_rate {0}, stream_channels {0}, stream_width {0}, xruns {0}, opens {0};
    std::atomic<unsigned> fill_target {prime_frames}, lowest {0};
    std::atomic<int> trim_ppm {0}, pitch_ppm {0};
    std::atomic<unsigned> device_rate {44100};
    mutable std::mutex device_lock; // Guards `device_name`; never taken by the audio thread.
    std::string device_name;
};

// One block of stems for the USB-C channels after the main mix: one mono
// float channel per track, -1..1, written by an app's render().
constexpr unsigned usb_stem_count = 16, usb_stem_frames = 128;
struct UsbStems { float track[usb_stem_count][usb_stem_frames]; };

// What the computer records from the MPC: whatever the audio thread hands to
// push(), which the runtime does with every block an app renders, so the
// computer hears what the jacks play, after the app's own volume, unless the
// app gives a mix of its own (App::usb_mix()). A separate thread finds the
// gadget, waits for the computer to start recording, then writes it.
// Silence goes out while the ring primes. push() drops everything while
// nothing is streaming.
//
// With the gadget's "Playback Pitch 1000000" control the gadget sends at the
// AK4558's pace: after every 250 ms of the computer's audio, PitchLock turns
// the ring's average fill meanwhile into the pitch the gadget's packets are
// sized by, holding the ring at locked_target(), one audio block, one period
// and 2 ms. At equal rates the frames pass untouched; at others a resampler
// converts them at a fixed ratio. Should the ring stray off its target
// anyway, after a second more than a third of it away the output unlocks and
// trims as below.
//
// Without the control the resampler converts every frame, and drift_trim()
// keeps the ring near `prime_frames` by nudging it, so a long recording
// neither runs dry nor overflows.
//
// The gadget's channels are the main mix (1 and 2), then each stem in
// order (3 to 18), then silence; a mono gadget gets the main mix summed.
// With fewer than 18 channels the stems that do not fit are left out.
class UsbAudioOutput {
public:
    static constexpr unsigned ring_frames = 8192, prime_frames = 1024;
    static constexpr unsigned channels = 2 + usb_stem_count; // What the ring carries
    // The ring's fill while locked: the audio thread's 128-frame block, the
    // gadget's period and 2 ms to spare (280 frames at 48 kHz with the
    // 18-channel gadget's 56-frame period).
    static constexpr unsigned locked_target(unsigned rate, unsigned period) { return 128 + period + rate / 500; }

    UsbAudioOutput() = default;
    ~UsbAudioOutput() { stop(); }
    UsbAudioOutput(const UsbAudioOutput&) = delete;
    UsbAudioOutput& operator=(const UsbAudioOutput&) = delete;

    // "auto" finds the UAC2 gadget card; any other value is an ALSA PCM name,
    // opened at the device's rate when it has no rate control. Called from one control
    // thread; start() again restarts.
    void start(const std::string& device);
    void stop();
    // The device start() was last given, or empty before the first start().
    std::string requested() const;
    // Audio thread: `count` interleaved stereo S32 frames, and that block's
    // stems (silent when null; then `count` may be any length, otherwise at
    // most usb_stem_frames). Never blocks.
    void push(const int32_t* frames, unsigned count, const UsbStems* stems = nullptr);
    // Any thread: the audio device's rate, what push() is given (44.1 kHz
    // until told). The runtime sets it at each open; a change reopens the
    // stream within 250 ms.
    void set_device_rate(unsigned rate) { device_rate.store(rate, std::memory_order_relaxed); }

    UsbStreamState state() const { return UsbStreamState(current.load(std::memory_order_acquire)); }
    unsigned rate() const { return stream_rate.load(std::memory_order_relaxed); }
    // How many stems the open stream carries: its channels after the first two.
    unsigned stems() const { return std::min(std::max(stream_channels.load(std::memory_order_relaxed), 2u) - 2, usb_stem_count); }
    UsbStreamStats stats() const;
    std::string json() const;

private:
    using Ring = FrameRing<ring_frames, channels>; // 590 KB: on the heap, not the caller's stack
    void run(std::string device);
    void set_state(UsbStreamState state) { current.store(uint8_t(state), std::memory_order_release); }
    std::unique_ptr<Ring> owned_ring = std::make_unique<Ring>();
    Ring& ring = *owned_ring;
    std::array<float, usb_stem_frames * channels> block {}; // Audio thread: push()'s staging
    std::thread thread;
    std::atomic<bool> running {false}, accepting {false}, locked {false};
    std::atomic<uint8_t> current {uint8_t(UsbStreamState::Stopped)};
    std::atomic<unsigned> stream_rate {0}, stream_channels {0}, stream_width {0}, xruns {0}, opens {0};
    std::atomic<unsigned> fill_target {prime_frames}, lowest {0};
    std::atomic<int> trim_ppm {0}, pitch_ppm {0};
    std::atomic<unsigned> device_rate {44100};
    mutable std::mutex device_lock; // Guards the names; never taken by the audio thread.
    std::string device_name, requested_name;
};
