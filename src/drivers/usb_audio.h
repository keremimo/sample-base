// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include "dsp/frame_ring.h"
#include <atomic>
#include <mutex>
#include <string>
#include <thread>

// Audio that a computer plays to the MPC over USB-C. The kernel's UAC2 gadget
// function (drivers/usb/gadget/function/f_uac2.c and u_audio.c) registers an
// ALSA card whose driver is "UAC2_Gadget" with one "UAC2 PCM" device. Its
// capture stream is the host's playback. The sample size (S16_LE, S24_3LE or
// S32_LE) and channel count are fixed by the gadget configuration, the rate is
// whatever the host chose, and the PCM-interface control "Capture Rate" reads 0
// while the host is not streaming.
//
// start() opens nothing on the calling thread: a separate thread finds the
// gadget, waits for the host, converts the stream to 44.1 kHz stereo float and
// hands it to the audio thread through a FrameRing. The host and the AK4558
// have independent clocks, so the ring absorbs drift and re-primes when it
// runs dry. stop() closes the PCM. Nothing is opened unless an application
// selects USB input.
enum class UsbInputState : uint8_t { Stopped, Missing, Waiting, Running, Failed, Count };
constexpr std::array<const char*, size_t(UsbInputState::Count)> usb_input_state_names {{"stopped", "missing", "waiting", "running", "failed"}};

class UsbAudioInput {
public:
    static constexpr unsigned ring_frames = 8192, prime_frames = 1024, output_rate = 44100;
    struct Device { std::string pcm, control; };

    UsbAudioInput() = default;
    ~UsbAudioInput() { stop(); }
    UsbAudioInput(const UsbAudioInput&) = delete;
    UsbAudioInput& operator=(const UsbAudioInput&) = delete;

    // "auto" finds the UAC2 gadget card; any other value is an ALSA PCM name
    // whose rate is negotiated directly. Called from one control thread.
    void start(const std::string& device);
    void stop();
    // Audio thread: `count` stereo frames, silent until enough are buffered.
    bool pull(float* frames, unsigned count) { return ring.pull(frames, count, prime_frames); }

    UsbInputState state() const { return UsbInputState(current.load(std::memory_order_acquire)); }
    unsigned rate() const { return stream_rate.load(std::memory_order_relaxed); }
    unsigned channels() const { return stream_channels.load(std::memory_order_relaxed); }
    unsigned bits() const { return stream_width.load(std::memory_order_relaxed) * 8; }
    std::string json() const;
    static Device find_gadget();
    // The host's current rate, 0 while it is not streaming, or -1 without the control.
    static long host_rate(const std::string& control);

private:
    void run(std::string device);
    void set_state(UsbInputState state) { current.store(uint8_t(state), std::memory_order_release); }
    FrameRing<ring_frames> ring;
    std::thread thread;
    std::atomic<bool> running {false};
    std::atomic<uint8_t> current {uint8_t(UsbInputState::Stopped)};
    std::atomic<unsigned> stream_rate {0}, stream_channels {0}, stream_width {0}, xruns {0}, opens {0};
    mutable std::mutex device_lock; // Guards `device_name`; never taken by the audio thread.
    std::string device_name;
};
