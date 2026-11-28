// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <thread>

// USB-C in straight to the jacks, as an audio interface's direct monitoring
// does: the USB-C input thread (UsbAudioInput) adds each period it reads into
// the AK4558's playback buffer a little ahead of where the codec's DMA reads,
// on top of what the app rendered there. An app otherwise takes USB-C in a
// block at a time and the output queues it behind two or more periods; a
// computer's instrument played from the MPC then comes back about 11 ms after
// it arrives, here about 3 ms. Dry: nothing the app does to its own mix
// touches it, and an app that plays USB-C in through its mix too should leave
// it out of that while this is on (set()).
//
// The audio thread (AudioDevice::run) lends the playback buffer while the
// codec output runs (mmap access) and says after each block how far it has
// written and where the DMA was (at()); the input thread places each period
// from that, on the AK4558's rate. Its writes stay `ahead` frames in front of
// the DMA: they follow the computer's pace, which the gadget's feedback locks
// to the AK4558 on average but lets wander by tens of frames over seconds.
// More than `nudge` frames off `ahead`, a period repeats or leaves out one
// frame (nudges()); out of [min_ahead, max_ahead] they start again at
// `ahead` (a slip: a click). One process-wide instance, as there is one codec
// output: direct_monitor().
class DirectMonitor {
public:
    static constexpr unsigned min_ahead = 32, ahead = 96, max_ahead = 192, nudge = 16;

    // Any thread: whether USB-C in plays direct now, and at what gain each
    // side (the app's main volume and its own level). add() eases to it over
    // about 5 ms, and from it to silence when turned off, so neither clicks;
    // silent and off, it leaves the buffer alone.
    void set(bool on, float left, float right) {
        gain_l.store(left, std::memory_order_relaxed);
        gain_r.store(right, std::memory_order_relaxed);
        on_.store(on, std::memory_order_relaxed);
    }
    void set(bool on, float gain) { set(on, gain, gain); }
    bool on() const { return on_.load(std::memory_order_relaxed); }

    // Audio thread: the codec's playback buffer, interleaved stereo S32,
    // `frames` long, at `rate`, until detach().
    void attach(int32_t* buffer, unsigned frames, unsigned rate) {
        frames_ = frames; rate_ = rate;
        restart();
        buffer_.store(buffer, std::memory_order_seq_cst);
    }
    // Audio thread: the stream was prepared again (an underrun), so its
    // positions start from 0; nothing is placed until the next at().
    void restart() {
        seq_.fetch_add(1, std::memory_order_acq_rel);
        valid_.store(false, std::memory_order_relaxed);
        epoch_.fetch_add(1, std::memory_order_relaxed);
        seq_.fetch_add(1, std::memory_order_release);
    }
    // Audio thread, after each block: `written` frames committed since the
    // stream was prepared, and `played` the frames the DMA had read by `ns`
    // (CLOCK_MONOTONIC); `running` false while the stream waits to start.
    void at(uint64_t written, uint64_t played, int64_t ns, bool running) {
        seq_.fetch_add(1, std::memory_order_acq_rel);
        written_.store(written, std::memory_order_relaxed);
        played_.store(played, std::memory_order_relaxed);
        ns_.store(ns, std::memory_order_relaxed);
        valid_.store(running, std::memory_order_relaxed);
        seq_.fetch_add(1, std::memory_order_release);
    }
    // Audio thread: before the buffer goes. Waits out a write in progress.
    void detach() {
        buffer_.store(nullptr, std::memory_order_seq_cst);
        while (busy_.load(std::memory_order_seq_cst)) std::this_thread::sleep_for(std::chrono::microseconds(50));
    }

    // USB-C input thread: `count` stereo float frames just read, at the
    // audio device's rate.
    void add(const float* frames, unsigned count) {
        const bool on = on_.load(std::memory_order_relaxed);
        if (!on && eased_l == 0 && eased_r == 0) { placed = false; return; }
        busy_.store(true, std::memory_order_seq_cst);
        int32_t* buffer = buffer_.load(std::memory_order_seq_cst);
        uint64_t written = 0, played = 0; int64_t ns = 0; bool valid = false; unsigned epoch = 0;
        if (buffer) {
            for (unsigned tries = 0; tries < 8; ++tries) {
                unsigned before = seq_.load(std::memory_order_acquire);
                if (before & 1) continue;
                written = written_.load(std::memory_order_relaxed); played = played_.load(std::memory_order_relaxed);
                ns = ns_.load(std::memory_order_relaxed); valid = valid_.load(std::memory_order_relaxed);
                epoch = epoch_.load(std::memory_order_relaxed);
                std::atomic_thread_fence(std::memory_order_acquire);
                if (seq_.load(std::memory_order_relaxed) == before) break;
                valid = false;
            }
        }
        if (!buffer || !valid) { placed = false; eased_l = eased_r = 0; busy_.store(false, std::memory_order_release); return; }
        if (epoch != epoch_seen) { placed = false; epoch_seen = epoch; }
        const int64_t now = int64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
        const uint64_t dma = played + uint64_t(std::max<int64_t>(0, now - ns) * int64_t(rate_) / 1000000000);
        if (!placed || cursor < dma + min_ahead || cursor > dma + max_ahead) {
            if (placed) slips_.fetch_add(1, std::memory_order_relaxed);
            cursor = dma + ahead; placed = true;
        }
        const unsigned lead = unsigned(cursor - dma);
        lead_.store(lead, std::memory_order_relaxed);
        // Wandered off `ahead`: one frame more (the first again) or one fewer (the first left out) this period.
        const bool repeat = lead < ahead - nudge && count > 1, skip = lead > ahead + nudge && count > 1;
        if (repeat || skip) nudges_.fetch_add(1, std::memory_order_relaxed);
        const unsigned out = count + (repeat ? 1 : 0) - (skip ? 1 : 0);
        const float aim_l = on ? gain_l.load(std::memory_order_relaxed) : 0.0f, aim_r = on ? gain_r.load(std::memory_order_relaxed) : 0.0f;
        const float ease = 1.0f - std::exp(-1.0f / (0.005f * float(rate_))); // a 5 ms time constant
        for (unsigned f = 0; f < out; ++f) {
            uint64_t at = cursor + f;
            if (at >= written) { short_.fetch_add(1, std::memory_order_relaxed); break; } // the app has not written it yet
            const unsigned from = repeat ? (f ? f - 1 : 0) : f + (skip ? 1 : 0);
            eased_l += (aim_l - eased_l) * ease; eased_r += (aim_r - eased_r) * ease;
            int32_t* frame = buffer + size_t(at % frames_) * 2;
            frame[0] = mix(frame[0], frames[2 * from] * eased_l * 2147483648.0f);
            frame[1] = mix(frame[1], frames[2 * from + 1] * eased_r * 2147483648.0f);
        }
        if (!on && eased_l < 1e-5f && eased_r < 1e-5f) eased_l = eased_r = 0; // faded out: done until on again
        cursor += out;
        added_.fetch_add(count, std::memory_order_relaxed);
        busy_.store(false, std::memory_order_release);
    }

    // Any thread: frames placed, slips (the computer's pace out of reach,
    // started again), nudges (a frame repeated or left out), periods cut short by the app's writing, and how far
    // ahead of the DMA the last period went, in frames.
    uint64_t added() const { return added_.load(std::memory_order_relaxed); }
    unsigned slips() const { return slips_.load(std::memory_order_relaxed); }
    unsigned nudges() const { return nudges_.load(std::memory_order_relaxed); }
    unsigned short_writes() const { return short_.load(std::memory_order_relaxed); }
    unsigned lead() const { return lead_.load(std::memory_order_relaxed); }
    bool attached() const { return buffer_.load(std::memory_order_relaxed) != nullptr; }

private:
    static int32_t mix(int32_t sample, float add) {
        int64_t sum = int64_t(sample) + int64_t(add);
        return int32_t(std::clamp<int64_t>(sum, INT32_MIN, INT32_MAX));
    }
    std::atomic<int32_t*> buffer_ {nullptr};
    unsigned frames_ = 0, rate_ = 48000; // set before buffer_ is published
    std::atomic<bool> on_ {false}, busy_ {false}, valid_ {false};
    std::atomic<float> gain_l {0.0f}, gain_r {0.0f};
    std::atomic<unsigned> seq_ {0}, epoch_ {0};
    std::atomic<uint64_t> written_ {0}, played_ {0};
    std::atomic<int64_t> ns_ {0};
    std::atomic<uint64_t> added_ {0};
    std::atomic<unsigned> slips_ {0}, nudges_ {0}, short_ {0}, lead_ {0};
    // The input thread's own.
    uint64_t cursor = 0;
    bool placed = false;
    float eased_l = 0, eased_r = 0; // the gains as the last frame played them
    unsigned epoch_seen = ~0u;
};

// The one codec output's.
inline DirectMonitor& direct_monitor() {
    static DirectMonitor monitor;
    return monitor;
}
