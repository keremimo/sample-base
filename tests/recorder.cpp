// SPDX-License-Identifier: AGPL-3.0-only
#include "recorder.h"
#include <cassert>
#include <cmath>
#include <vector>

static bool near(float a, float b) { return std::abs(a - b) < 1e-6f; }

static void channel_selection() {
    auto routed = [](InputSource source, InputChannels channels) {
        std::array<float, 4> frames {0.25f, -0.5f, 1.0f, 0.0f};
        select_input_channels(source, channels, frames.data(), 2);
        return frames;
    };
    auto off = routed(InputSource::Off, InputChannels::Stereo);
    for (float sample : off) assert(sample == 0);
    auto stereo = routed(InputSource::Line, InputChannels::Stereo);
    assert(stereo[0] == 0.25f && stereo[1] == -0.5f && stereo[2] == 1.0f && stereo[3] == 0.0f);
    auto left = routed(InputSource::Line, InputChannels::Left);
    assert(left[0] == 0.25f && left[1] == 0.25f && left[2] == 1.0f && left[3] == 1.0f);
    auto right = routed(InputSource::Usb, InputChannels::Right);
    assert(right[0] == -0.5f && right[1] == -0.5f && right[2] == 0.0f && right[3] == 0.0f);
    // The microphone is mono whatever the channel choice.
    auto mic = routed(InputSource::Mic, InputChannels::Left);
    assert(near(mic[0], -0.125f) && near(mic[1], -0.125f) && near(mic[2], 0.5f) && near(mic[3], 0.5f));
    assert(input_route(InputSource::Mic, InputChannels::Left) == input_route(InputSource::Mic, InputChannels::Right));
    assert(input_route(InputSource::Line, InputChannels::Left) != input_route(InputSource::Line, InputChannels::Right));
    assert(input_route(InputSource::Line, InputChannels::Left) != input_route(InputSource::Usb, InputChannels::Left));
    assert(next_input_source(InputSource::Off) == InputSource::Line && next_input_source(InputSource::Line) == InputSource::Mic &&
           next_input_source(InputSource::Mic) == InputSource::Usb && next_input_source(InputSource::Usb) == InputSource::Off);
    assert(next_input_channels(InputChannels::Right) == InputChannels::Stereo);
}

static void sample_formats() {
    const uint8_t s16[] {0x00, 0x80, 0xff, 0x7f, 0x00, 0x40};
    assert(pcm_sample(s16, 2) == -1.0f && near(pcm_sample(s16 + 2, 2), 32767 / 32768.0f) && pcm_sample(s16 + 4, 2) == 0.5f);
    const uint8_t s24[] {0x00, 0x00, 0x80, 0xff, 0xff, 0xff, 0x00, 0x00, 0x40};
    assert(pcm_sample(s24, 3) == -1.0f && near(pcm_sample(s24 + 3, 3), -1 / 8388608.0f) && pcm_sample(s24 + 6, 3) == 0.5f);
    const uint8_t s32[] {0x00, 0x00, 0x00, 0x80, 0x00, 0x00, 0x00, 0xc0};
    assert(pcm_sample(s32, 4) == -1.0f && pcm_sample(s32 + 4, 4) == -0.5f);
}

static void resampling() {
    LinearResampler same; same.reset(44100, 44100);
    std::vector<float> out;
    for (int n = 0; n < 10; ++n) same.feed(float(n), float(-n), [&](float l, float r) { out.push_back(l); out.push_back(r); });
    assert(out.size() == 18); // One frame of latency.
    for (unsigned n = 0; n < 9; ++n) assert(out[2 * n] == float(n) && out[2 * n + 1] == -float(n));

    LinearResampler down; down.reset(48000, 44100);
    out.clear();
    for (int n = 0; n < 48000; ++n) down.feed(n / 48000.0f, 0.0f, [&](float l, float r) { out.push_back(l); out.push_back(r); });
    assert(out.size() / 2 >= 44098 && out.size() / 2 <= 44101);
    // A ramp stays a ramp: each output advances by 48000/44100 input frames.
    for (unsigned n = 1; n < 2000; ++n) assert(std::abs((out[2 * n] - out[2 * n - 2]) * 48000.0f - 48000.0f / 44100.0f) < 1e-3f);

    LinearResampler up; up.reset(32000, 44100);
    unsigned count = 0;
    for (int n = 0; n < 32000; ++n) up.feed(0.5f, 0.5f, [&](float l, float r) { assert(l == 0.5f && r == 0.5f); ++count; });
    assert(count >= 44098 && count <= 44101);
}

static void ring() {
    FrameRing<16> frames;
    std::array<float, 2 * 8> block {};
    std::array<float, 2 * 4> out {};
    for (unsigned n = 0; n < 8; ++n) { block[2 * n] = float(n); block[2 * n + 1] = -float(n); }
    assert(frames.push(block.data(), 4) == 4);
    assert(!frames.pull(out.data(), 4, 6) && out[0] == 0); // Not primed yet.
    assert(frames.push(block.data() + 8, 4) == 4);
    assert(frames.pull(out.data(), 4, 6) && out[0] == 0 && out[1] == 0 && out[6] == 3 && out[7] == -3);
    assert(frames.pull(out.data(), 4, 6) && out[0] == 4 && out[7] == -7);
    assert(!frames.pull(out.data(), 4, 6) && frames.underruns == 1); // Ran dry: silent and re-primes.
    assert(frames.push(block.data(), 8) == 8 && frames.push(block.data(), 8) == 8);
    assert(frames.push(block.data(), 1) == 0 && frames.overflows == 1); // Full ring drops the newest.
    frames.flush();
    assert(!frames.pull(out.data(), 4, 4) && frames.buffered() == 0);
}

static void recorder() {
    constexpr unsigned block = 128;
    Recorder take(1000);
    assert(take.overview_columns() == 2 && take.state() == Recorder::State::Idle && take.length() == 0);
    std::array<float, block * 2> input {}, output {};
    for (unsigned n = 0; n < block; ++n) { input[2 * n] = 0.5f; input[2 * n + 1] = -0.25f; }
    unsigned line = input_route(InputSource::Line, InputChannels::Stereo);

    take.record();
    take.process(input.data(), output.data(), block, line, false);
    assert(take.state() == Recorder::State::Idle && take.refused() == 1 && take.takes() == 0);

    take.record();
    for (unsigned n = 0; n < 3; ++n) take.process(input.data(), output.data(), block, line, true);
    assert(take.state() == Recorder::State::Recording && take.length() == 3 * block && take.takes() == 1 && take.route() == line);
    for (float sample : output) assert(sample == 0); // Recording adds nothing to the output.
    assert(take.overview_peak(0, take.length()) == 128);
    take.record(); // Second press finishes the take.
    take.process(input.data(), output.data(), block, line, true);
    assert(take.state() == Recorder::State::Idle && take.length() == 3 * block);

    take.play();
    take.process(input.data(), output.data(), block, line, true);
    assert(take.state() == Recorder::State::Playing && take.position() == block);
    assert(output[0] == 0.5f && output[1] == -0.25f && output[2 * block - 1] == -0.25f);
    output.fill(0.125f);
    take.process(input.data(), output.data(), block, line, true);
    assert(output[0] == 0.625f); // Playback is added to what is already there.
    output.fill(0);
    take.process(input.data(), output.data(), block, line, true);
    assert(take.state() == Recorder::State::Idle && take.position() == 0); // Finished after the last frame.

    take.play();
    take.process(input.data(), output.data(), block, line, true);
    take.stop();
    output.fill(0);
    take.process(input.data(), output.data(), block, line, true);
    assert(take.state() == Recorder::State::Idle && output[0] == 0);

    // A new take replaces the old one, and a route change ends it.
    take.record();
    take.process(input.data(), output.data(), block, input_route(InputSource::Usb, InputChannels::Left), true);
    assert(take.length() == block && take.takes() == 2);
    take.process(input.data(), output.data(), block, input_route(InputSource::Usb, InputChannels::Right), true);
    assert(take.state() == Recorder::State::Idle && take.length() == block);

    // Recording stops by itself when the buffer is full.
    take.record();
    for (unsigned n = 0; n < 10; ++n) take.process(input.data(), output.data(), block, line, true);
    assert(take.state() == Recorder::State::Idle && take.length() == 1000);
    assert(take.overview_peak(512, 1000) == 128);

    // Play while recording ends the take and plays it from the start.
    take.record();
    for (unsigned n = 0; n < 2; ++n) take.process(input.data(), output.data(), block, line, true);
    take.play();
    output.fill(0);
    take.process(input.data(), output.data(), block, line, true);
    assert(take.state() == Recorder::State::Playing && take.length() == 2 * block && output[0] == 0.5f);
    assert(take.overview_peak(512, 1000) == 0); // The new take's overview has no stale peaks.

    // Play with nothing recorded does nothing.
    Recorder empty(256);
    empty.play();
    empty.process(input.data(), output.data(), block, line, true);
    assert(empty.state() == Recorder::State::Idle);
}

int main() {
    channel_selection();
    sample_formats();
    resampling();
    ring();
    recorder();
}
