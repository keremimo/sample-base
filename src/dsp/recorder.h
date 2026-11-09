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

// Input recording. The sources follow the AC50 device tree: the
// AK4558 capture pair carries either the two TRS line inputs (L and R) or the
// built-in microphone, chosen by the "TRS" and "Builtin Microphone" DAPM pin
// switches, never both. USB-C audio played by a computer arrives on the
// kernel's UAC2 gadget capture PCM (see drivers/usb_audio.h). Everything here is
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
