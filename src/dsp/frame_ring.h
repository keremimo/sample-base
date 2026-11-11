// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>

// Moving audio between clock domains: PCM encoding and decoding, rate
// conversion, drift correction and a lock-free ring from one producer thread
// to one consumer thread. Portable and allocation-free; drivers/usb_audio
// uses them in both directions.

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

// Linear-interpolating rate converter for frames of `Channels` samples. Cheap
// enough for an audio thread, but it dulls the top octave by up to
// cos(pi f / rate) as its read position moves between frames, and converting
// between 44.1 and 48 kHz it folds images back into the audible band.
// SincResampler below does neither. Output lags input by one frame.
template <unsigned Channels> class FrameResampler {
public:
    void reset(unsigned input_rate, unsigned output_rate) {
        base = step = input_rate && output_rate ? double(input_rate) / output_rate : 1.0;
        phase = 0; primed = false;
    }
    // Scales the conversion by `factor` from the next frame on, without a
    // jump: above 1 it reads input slightly faster. See drift_trim().
    void trim(double factor) { step = base * factor; }
    // `emit` gets each converted frame as a pointer to `Channels` samples.
    template <class Emit> void feed(const float* frame, Emit&& emit) {
        if (!primed) { std::copy(frame, frame + Channels, last.begin()); primed = true; return; }
        while (phase < 1.0) {
            float t = float(phase);
            for (unsigned c = 0; c < Channels; ++c) mixed[c] = last[c] + (frame[c] - last[c]) * t;
            emit(static_cast<const float*>(mixed.data()));
            phase += step;
        }
        phase -= 1.0;
        std::copy(frame, frame + Channels, last.begin());
    }
private:
    double base = 1.0, step = 1.0, phase = 0;
    std::array<float, Channels> last {}, mixed {};
    bool primed = false;
};

// The stereo converter, fed and emitting left and right.
class LinearResampler : public FrameResampler<2> {
public:
    using FrameResampler<2>::feed;
    template <class Emit> void feed(float left, float right, Emit&& emit) {
        const float frame[2] {left, right};
        feed(frame, [&](const float* out) { emit(out[0], out[1]); });
    }
};

// The kernel SincResampler interpolates: sin(pi u) / (pi u) under a Kaiser
// window that closes `zero_crossings` crossings from the centre, tabulated
// from u = 0 at `steps` points a crossing, each with the slope to the next,
// and zero for one crossing beyond. One table, built on first use, serves
// every converter and every rate.
class SincKernel {
public:
    static constexpr unsigned zero_crossings = 32, steps = 256;
    struct Point { float value, slope; };
    static const Point* table() {
        static const SincKernel kernel;
        return kernel.points.data();
    }

private:
    SincKernel() {
        constexpr double pi = 3.14159265358979323846, beta = 9.2;
        auto bessel_i0 = [](double x) {
            double sum = 1, term = 1;
            for (int k = 1; k < 100 && term > 1e-17 * sum; ++k) { term *= x * x / (4.0 * k * k); sum += term; }
            return sum;
        };
        const double unity = bessel_i0(beta);
        for (unsigned n = 0; n < points.size(); ++n) {
            double u = double(n) / steps, edge = u / zero_crossings;
            if (edge >= 1) break;
            double sinc = n % steps ? std::sin(pi * u) / (pi * u) : n ? 0.0 : 1.0; // Exact zeros: equal rates pass bit for bit.
            points[n].value = float(sinc * bessel_i0(beta * std::sqrt(1 - edge * edge)) / unity);
        }
        for (unsigned n = 0; n + 1 < points.size(); ++n) points[n].slope = points[n + 1].value - points[n].value;
    }
    std::array<Point, (zero_crossings + 1) * steps + 1> points {};
};

// Band-limited rate converter for frames of `Channels` samples, for a
// computer's USB clock against the AK4558's. Each output frame is the input
// under a windowed sinc centred on its position: everything up to 0.907 of
// the lower rate's Nyquist frequency (20 kHz at 44.1 kHz) passes flat, and
// whatever the conversion would fold or image back into that band is at
// least 90 dB down, wherever between two input frames the output falls.
// Converting down, the kernel stretches over more input frames so that the
// passband follows the output's rate. Each frame is normalised by its
// weights, so DC passes exactly. Output lags input by `half` frames (32 at
// equal rates, under a millisecond); before the first frame the input is
// taken to have held it. No allocation; reset() builds the shared kernel on
// first use.
template <unsigned Channels> class SincResampler {
public:
    // The widest kernel the history holds, in input frames each side: the
    // full stopband converting down by up to 4:1. Beyond that the band stays
    // a quarter of the input's, and what lies above the output's folds back.
    static constexpr unsigned max_half = 128;

    SincResampler() { reset(0, 0); }
    void reset(unsigned input_rate, unsigned output_rate) {
        kernel = SincKernel::table();
        base = step = input_rate && output_rate ? double(input_rate) / output_rate : 1.0;
        double band = std::max(std::min(1.0, 1.0 / base), double(SincKernel::zero_crossings) / max_half);
        half = std::min(unsigned(std::ceil(SincKernel::zero_crossings / band)), max_half);
        scale = band * SincKernel::steps;
        phase = 0; pending = half; primed = false;
    }
    // Scales the conversion by `factor` from the next frame on, without a
    // jump: above 1 it reads input slightly faster. See drift_trim().
    void trim(double factor) { step = base * factor; }
    // `emit` gets each converted frame as a pointer to `Channels` samples.
    template <class Emit> void feed(const float* frame, Emit&& emit) {
        if (!primed) {
            for (unsigned n = 0; n < 2 * history_frames; ++n) std::copy(frame, frame + Channels, history.begin() + Channels * n);
            newest = 0; primed = true;
            return;
        }
        newest = (newest + 1) & (history_frames - 1);
        std::copy(frame, frame + Channels, history.begin() + Channels * newest);
        std::copy(frame, frame + Channels, history.begin() + Channels * (newest + history_frames));
        if (pending > 1) { --pending; return; }
        pending = 0;
        const unsigned taps = 2 * half;
        const float* window = history.data() + Channels * (newest + history_frames + 1 - taps);
        while (phase < 1.0) {
            // Tap j sits (half - 1 - j + phase) input frames from the output.
            const double first = (half - 1 + phase) * scale;
            float sum = 0;
            for (unsigned j = 0; j < taps; ++j) {
                double at = std::abs(first - j * scale);
                unsigned point = unsigned(at);
                weights[j] = kernel[point].value + float(at - point) * kernel[point].slope;
                sum += weights[j];
            }
            std::array<float, Channels> mixed {};
            for (unsigned j = 0; j < taps; ++j)
                for (unsigned c = 0; c < Channels; ++c) mixed[c] += weights[j] * window[Channels * j + c];
            const float gain = 1.0f / sum;
            for (float& sample : mixed) sample *= gain;
            emit(static_cast<const float*>(mixed.data()));
            phase += step;
        }
        phase -= 1.0;
    }
    // A stereo converter fed and emitting left and right.
    template <class Emit> void feed(float left, float right, Emit&& emit) {
        static_assert(Channels == 2, "left and right are for a stereo converter");
        const float frame[2] {left, right};
        feed(frame, [&](const float* out) { emit(out[0], out[1]); });
    }

private:
    // Twice over, so that the last 2 * half frames always lie in a row.
    static constexpr unsigned history_frames = 2 * max_half;
    const SincKernel::Point* kernel = nullptr;
    double base = 1.0, step = 1.0, phase = 0, scale = 0;
    unsigned half = 0, newest = 0, pending = 0;
    bool primed = false;
    std::array<float, 2 * max_half> weights {};
    std::array<float, 2 * history_frames * Channels> history {};
};

// Two crystals never agree exactly: the AK4558's clock and a computer's USB
// clock drift apart by tens of parts per million. When a FrameRing joins them,
// this turns the ring's average fill into a trim() factor for the converter on
// either side of it: a ring filling above `target` makes the converter read
// its input faster, so that one draining the ring empties it sooner and one
// feeding it adds fewer frames; a ring draining below `target` the reverse.
// Proportional only, so a steady drift settles with the fill offset by
// drift / gain, 50 frames for 100 ppm, with a time constant of about 11
// seconds at 44.1 kHz. The factor stays within ±0.2 %.
inline double drift_trim(double average_fill, double target) {
    constexpr double gain = 2e-6, limit = 2e-3;
    return 1.0 + std::clamp((average_fill - target) * gain, -limit, limit);
}

// Float frames of `Channels` interleaved samples (stereo unless stated) from
// one producer thread to one consumer thread, either
// way round the audio thread. The consumer waits until `prime` frames are
// buffered before it starts, and again after it runs dry, so a clock-domain
// hand-off such as USB costs one short silence rather than a stream of clicks.
// A full ring drops the newest frames.
template <unsigned Capacity, unsigned Channels = 2> class FrameRing {
    static_assert(Capacity >= 2 && (Capacity & (Capacity - 1)) == 0, "Capacity must be a power of two");
public:
    unsigned push(const float* frames, unsigned count) {
        auto w = write.load(std::memory_order_relaxed), r = read.load(std::memory_order_acquire);
        unsigned room = Capacity - (w - r), n = std::min(count, room);
        for (unsigned i = 0; i < n; ++i) {
            unsigned slot = (w + i) & (Capacity - 1);
            std::copy(frames + Channels * i, frames + Channels * (i + 1), data.begin() + Channels * slot);
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
            std::fill(frames, frames + Channels * count, 0.0f);
            return false;
        }
        for (unsigned i = 0; i < count; ++i) {
            unsigned slot = (r + i) & (Capacity - 1);
            std::copy(data.begin() + Channels * slot, data.begin() + Channels * (slot + 1), frames + Channels * i);
        }
        read.store(r + count, std::memory_order_release);
        return true;
    }
    // Any thread: the consumer discards everything buffered on its next pull.
    void flush() { flush_requested.store(true, std::memory_order_release); }
    unsigned buffered() const { return write.load(std::memory_order_acquire) - read.load(std::memory_order_acquire); }
    std::atomic<unsigned> overflows {0}, underruns {0};
private:
    std::array<float, Capacity * Channels> data {};
    std::atomic<unsigned> read {0}, write {0};
    std::atomic<bool> flush_requested {false};
    bool primed = false; // Consumer only.
};
