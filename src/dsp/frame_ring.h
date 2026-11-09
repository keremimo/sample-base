// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>

// Moving audio between clock domains: PCM encoding and decoding, rate
// conversion, drift correction and a lock-free ring from one producer thread
// to one consumer thread. Portable and allocation-free; drivers/usb_audio
// uses all of them, in both directions.

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

// The reverse of pcm_sample(): -1..1 as a signed little-endian sample of 2, 3
// or 4 bytes, rounded to the nearest step. Anything louder clips.
inline void pcm_store(float sample, uint8_t* bytes, unsigned width) {
    double scale = double(1u << (8 * width - 1));
    double value = std::clamp(double(sample) * scale + (sample < 0 ? -0.5 : 0.5), -scale, scale - 1);
    uint32_t bits = uint32_t(int32_t(value));
    for (unsigned n = 0; n < width; ++n) bytes[n] = uint8_t(bits >> (8 * n));
}

// Linear-interpolating rate converter for stereo frames. A computer chooses
// the USB rate; the AK4558 runs at 44.1 kHz. Output lags input by one frame.
class LinearResampler {
public:
    void reset(unsigned input_rate, unsigned output_rate) {
        base = step = input_rate && output_rate ? double(input_rate) / output_rate : 1.0;
        phase = 0; primed = false;
    }
    // Scales the conversion by `factor` from the next frame on, without a
    // jump: above 1 it reads input slightly faster. See drift_trim().
    void trim(double factor) { step = base * factor; }
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
    double base = 1.0, step = 1.0, phase = 0;
    float last_left = 0, last_right = 0;
    bool primed = false;
};

// Two crystals never agree exactly: the AK4558's 44.1 kHz and a computer's USB
// clock drift apart by tens of parts per million. When a FrameRing joins them,
// this turns the ring's average fill into a LinearResampler::trim() factor for
// its consumer: a ring filling above `target` is read faster, one draining
// below it slower. Proportional only, so a steady drift settles with the fill
// offset by drift / gain, 50 frames for 100 ppm, with a time constant of about
// 11 seconds at 44.1 kHz. The factor stays within ±0.2 %.
inline double drift_trim(double average_fill, double target) {
    constexpr double gain = 2e-6, limit = 2e-3;
    return 1.0 + std::clamp((average_fill - target) * gain, -limit, limit);
}

// Stereo float frames from one producer thread to one consumer thread, either
// way round the audio thread. The consumer waits until `prime` frames are
// buffered before it starts, and again after it runs dry, so a clock-domain
// hand-off such as USB costs one short silence rather than a stream of clicks.
// A full ring drops the newest frames.
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
