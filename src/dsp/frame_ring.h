// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>

// Moving audio between clock domains: PCM decoding, rate conversion and a
// lock-free ring from one producer thread to the audio thread. Portable and
// allocation-free; drivers/usb_audio uses all three.

// Signed little-endian PCM sample of 2, 3 or 4 bytes as -1..1.
inline float pcm_sample(const uint8_t* bytes, unsigned width) {
    if (width == 2) return float(int16_t(uint16_t(bytes[0] | bytes[1] << 8))) / 32768.0f;
    if (width == 3) {
        uint32_t value = uint32_t(bytes[0]) << 8 | uint32_t(bytes[1]) << 16 | uint32_t(bytes[2]) << 24;
        return float(int32_t(value) >> 8) / 8388608.0f;
    }
    uint32_t value = uint32_t(bytes[0]) | uint32_t(bytes[1]) << 8 | uint32_t(bytes[2]) << 16 | uint32_t(bytes[3]) << 24;
    return float(double(int32_t(value)) / 2147483648.0);
}

// Linear-interpolating rate converter for stereo frames. A computer chooses
// the USB rate; the AK4558 runs at 44.1 kHz. Output lags input by one frame.
class LinearResampler {
public:
    void reset(unsigned input_rate, unsigned output_rate) {
        step = input_rate && output_rate ? double(input_rate) / output_rate : 1.0;
        phase = 0; primed = false;
    }
    template <class Emit> void feed(float left, float right, Emit&& emit) {
        if (!primed) { last_left = left; last_right = right; primed = true; return; }
        while (phase < 1.0) {
            float t = float(phase);
            emit(last_left + (left - last_left) * t, last_right + (right - last_right) * t);
            phase += step;
        }
        phase -= 1.0;
        last_left = left; last_right = right;
    }
private:
    double step = 1.0, phase = 0;
    float last_left = 0, last_right = 0;
    bool primed = false;
};

// Stereo float frames from one producer thread to the audio thread. The
// consumer waits until `prime` frames are buffered before it starts, and again
// after it runs dry, so a clock-domain hand-off such as USB costs one short
// silence rather than a stream of clicks. A full ring drops the newest frames.
template <unsigned Capacity> class FrameRing {
    static_assert(Capacity >= 2 && (Capacity & (Capacity - 1)) == 0, "Capacity must be a power of two");
public:
    unsigned push(const float* frames, unsigned count) {
        auto w = write.load(std::memory_order_relaxed), r = read.load(std::memory_order_acquire);
        unsigned room = Capacity - (w - r), n = std::min(count, room);
        for (unsigned i = 0; i < n; ++i) {
            unsigned slot = (w + i) & (Capacity - 1);
            data[2 * slot] = frames[2 * i]; data[2 * slot + 1] = frames[2 * i + 1];
        }
        write.store(w + n, std::memory_order_release);
        if (n < count) overflows.fetch_add(1, std::memory_order_relaxed);
        return n;
    }
    // Writes `count` frames, real or silent; true when they were real.
    bool pull(float* frames, unsigned count, unsigned prime) {
        if (flush_requested.exchange(false, std::memory_order_acq_rel)) {
            read.store(write.load(std::memory_order_acquire), std::memory_order_release); primed = false;
        }
        auto r = read.load(std::memory_order_relaxed), w = write.load(std::memory_order_acquire);
        unsigned available = w - r;
        if (!primed && available >= std::max(prime, count)) primed = true;
        if (!primed || available < count) {
            if (primed) { underruns.fetch_add(1, std::memory_order_relaxed); primed = false; }
            std::fill(frames, frames + 2 * count, 0.0f);
            return false;
        }
        for (unsigned i = 0; i < count; ++i) {
            unsigned slot = (r + i) & (Capacity - 1);
            frames[2 * i] = data[2 * slot]; frames[2 * i + 1] = data[2 * slot + 1];
        }
        read.store(r + count, std::memory_order_release);
        return true;
    }
    // Any thread: the consumer discards everything buffered on its next pull.
    void flush() { flush_requested.store(true, std::memory_order_release); }
    unsigned buffered() const { return write.load(std::memory_order_acquire) - read.load(std::memory_order_acquire); }
    std::atomic<unsigned> overflows {0}, underruns {0};
private:
    std::array<float, Capacity * 2> data {};
    std::atomic<unsigned> read {0}, write {0};
    std::atomic<bool> flush_requested {false};
    bool primed = false; // Consumer only.
};
