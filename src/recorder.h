// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

// Input recording showcase. The sources follow the AC50 device tree: the
// AK4558 capture pair carries either the two TRS line inputs (L and R) or the
// built-in microphone, chosen by the "TRS" and "Builtin Microphone" DAPM pin
// switches, never both. USB-C audio played by a computer arrives on the
// kernel's UAC2 gadget capture PCM (see usb_audio.h). Everything here is
// portable and allocation-free after construction, so the audio thread can
// use it directly.
enum class InputSource : uint8_t { Off, Line, Mic, Usb, Count };
enum class InputChannels : uint8_t { Stereo, Left, Right, Count };
constexpr std::array<const char*, size_t(InputSource::Count)> input_source_names {{"off", "line", "mic", "usb"}};
constexpr std::array<const char*, size_t(InputSource::Count)> input_source_labels {{"Off", "Line in", "Mic", "USB-C"}};
constexpr std::array<const char*, size_t(InputChannels::Count)> input_channel_names {{"stereo", "left", "right"}};
constexpr std::array<const char*, size_t(InputChannels::Count)> input_channel_labels {{"L + R", "L", "R"}};

// Line, Mic, USB-C, Off, then Line again.
inline InputSource next_input_source(InputSource source) {
    return InputSource((unsigned(source) + 1) % unsigned(InputSource::Count));
}
inline InputChannels next_input_channels(InputChannels channels) {
    return InputChannels((unsigned(channels) + 1) % unsigned(InputChannels::Count));
}
// The microphone is one DAPM widget feeding both ADC inputs, so it is always
// recorded as a single mono channel and ignores the channel choice.
inline bool input_has_channels(InputSource source) { return source == InputSource::Line || source == InputSource::Usb; }
// A recording ends when its source or channel choice changes.
inline unsigned input_route(InputSource source, InputChannels channels) {
    return unsigned(source) << 4 | (input_has_channels(source) ? unsigned(channels) : 0u);
}

// Turns `count` interleaved stereo frames of the selected source into the
// frames that are recorded. One chosen channel is copied to both sides so a
// single line input or USB channel plays back centred.
inline void select_input_channels(InputSource source, InputChannels channels, float* frames, unsigned count) {
    for (unsigned n = 0; n < count; ++n) {
        float& left = frames[2 * n];
        float& right = frames[2 * n + 1];
        if (source == InputSource::Off || source >= InputSource::Count) left = right = 0;
        else if (source == InputSource::Mic) left = right = 0.5f * (left + right);
        else if (channels == InputChannels::Left) right = left;
        else if (channels == InputChannels::Right) left = right;
    }
}

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

// One take of up to `capacity` stereo frames, preallocated. Any thread may
// request record/play/stop; only the audio thread calls process(). The screen
// reads the published length, position and overview without locks.
class Recorder {
public:
    enum class State : uint8_t { Idle, Recording, Playing };
    static constexpr unsigned overview_frames = 512; // Frames per waveform overview peak.
    static constexpr std::array<const char*, 3> state_names {{"idle", "recording", "playing"}};

    explicit Recorder(unsigned capacity_frames)
        : capacity_(std::max(1u, capacity_frames)), samples(size_t(capacity_) * 2),
          peaks(std::make_unique<std::atomic<uint8_t>[]>(overview_columns())) {
        for (unsigned n = 0; n < overview_columns(); ++n) peaks[n].store(0, std::memory_order_relaxed);
    }
    Recorder(const Recorder&) = delete;
    Recorder& operator=(const Recorder&) = delete;

    void record() { requests.fetch_or(record_bit, std::memory_order_acq_rel); } // Starts or finishes a take.
    void play() { requests.fetch_or(play_bit, std::memory_order_acq_rel); }     // From the start; ends recording.
    void stop() { requests.fetch_or(stop_bit, std::memory_order_acq_rel); }

    // `input` holds `count` routed stereo frames from select_input_channels();
    // playback is added to `output`. A route change ends a recording, and a
    // record request is refused while `can_record` is false (no input chosen).
    void process(const float* input, float* output, unsigned count, unsigned route, bool can_record) {
        unsigned wanted = requests.exchange(0, std::memory_order_acq_rel);
        if (current == State::Recording && route != take_route) current = State::Idle;
        if (wanted & stop_bit) current = State::Idle;
        if (wanted & record_bit) {
            if (current == State::Recording) current = State::Idle;
            else if (!can_record) refusals.fetch_add(1, std::memory_order_relaxed);
            else begin_take(route);
        }
        if ((wanted & play_bit) && take_length) { current = State::Playing; play_position = 0; }
        else if ((wanted & play_bit) && current == State::Recording) current = State::Idle;
        if (current == State::Recording) {
            unsigned n = std::min(count, capacity_ - take_length);
            std::memcpy(&samples[size_t(take_length) * 2], input, sizeof(float) * 2 * n);
            for (unsigned i = 0; i < n; ++i) {
                float peak = std::max(std::abs(input[2 * i]), std::abs(input[2 * i + 1]));
                auto level = uint8_t(std::min(255.0f, peak * 255.0f + 0.5f));
                auto& column = peaks[(take_length + i) / overview_frames];
                if (level > column.load(std::memory_order_relaxed)) column.store(level, std::memory_order_relaxed);
            }
            take_length += n;
            if (take_length == capacity_) current = State::Idle;
        } else if (current == State::Playing) {
            unsigned n = std::min(count, take_length - play_position);
            for (unsigned i = 0; i < 2 * n; ++i) output[i] += samples[size_t(play_position) * 2 + i];
            play_position += n;
            if (play_position >= take_length) current = State::Idle;
        }
        published_length.store(take_length, std::memory_order_release);
        published_position.store(current == State::Playing ? play_position : 0, std::memory_order_relaxed);
        published_state.store(uint8_t(current), std::memory_order_release);
    }

    State state() const { return State(published_state.load(std::memory_order_acquire)); }
    unsigned length() const { return published_length.load(std::memory_order_acquire); }
    unsigned position() const { return published_position.load(std::memory_order_relaxed); }
    unsigned capacity() const { return capacity_; }
    unsigned route() const { return published_route.load(std::memory_order_relaxed); }
    unsigned takes() const { return take_count.load(std::memory_order_relaxed); }
    unsigned refused() const { return refusals.load(std::memory_order_relaxed); }
    unsigned overview_columns() const { return (capacity_ + overview_frames - 1) / overview_frames; }
    // Peak, 0..255, of frames [first, last) of the current take.
    uint8_t overview_peak(unsigned first, unsigned last) const {
        last = std::min(last, length());
        uint8_t peak = 0;
        for (unsigned column = first / overview_frames; column * overview_frames < last && column < overview_columns(); ++column)
            peak = std::max(peak, peaks[column].load(std::memory_order_relaxed));
        return peak;
    }

private:
    static constexpr unsigned record_bit = 1, play_bit = 2, stop_bit = 4;
    void begin_take(unsigned route) {
        published_length.store(0, std::memory_order_release);
        for (unsigned n = 0; n * overview_frames < take_length; ++n) peaks[n].store(0, std::memory_order_relaxed);
        take_length = play_position = 0; take_route = route; current = State::Recording;
        published_route.store(route, std::memory_order_relaxed);
        take_count.fetch_add(1, std::memory_order_relaxed);
    }
    const unsigned capacity_;
    std::vector<float> samples;
    std::unique_ptr<std::atomic<uint8_t>[]> peaks;
    std::atomic<unsigned> requests {0}, published_length {0}, published_position {0}, published_route {0}, take_count {0}, refusals {0};
    std::atomic<uint8_t> published_state {0};
    // Audio thread only.
    State current = State::Idle;
    unsigned take_length = 0, play_position = 0, take_route = 0;
};
