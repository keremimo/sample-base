// SPDX-License-Identifier: AGPL-3.0-only
#include "dsp/frame_ring.h"
#include "dsp/recorder.h"
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

    // Encoding is the reverse, rounded to the nearest step and clipped.
    for (unsigned width : {2u, 3u, 4u}) {
        uint8_t bytes[4] {};
        float step = 1.0f / float(1u << (8 * width - 1));
        for (float sample : {0.0f, 0.5f, -0.5f, -1.0f, 0.123456f, -0.987654f}) {
            pcm_store(sample, bytes, width);
            assert(std::abs(pcm_sample(bytes, width) - sample) <= step);
        }
        pcm_store(-1.0f, bytes, width); assert(pcm_sample(bytes, width) == -1.0f);
        pcm_store(-3.0f, bytes, width); assert(pcm_sample(bytes, width) == -1.0f);
        pcm_store(2.0f, bytes, width); assert(near(pcm_sample(bytes, width), 1.0f - step) && bytes[width - 1] == 0x7f);
    }
    uint8_t s16_half[2]; pcm_store(0.5f, s16_half, 2); assert(s16_half[0] == 0x00 && s16_half[1] == 0x40);
    uint8_t s24_low[3]; pcm_store(-1 / 8388608.0f, s24_low, 3); assert(s24_low[0] == 0xff && s24_low[1] == 0xff && s24_low[2] == 0xff);
    uint8_t s32_quarter[4]; pcm_store(-0.25f, s32_quarter, 4);
    assert(s32_quarter[0] == 0 && s32_quarter[1] == 0 && s32_quarter[2] == 0 && s32_quarter[3] == 0xe0);
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

    // USB output runs the other way, and trim() nudges it without a jump.
    LinearResampler to_usb; to_usb.reset(44100, 48000);
    count = 0;
    for (int n = 0; n < 44100; ++n) to_usb.feed(0.25f, -0.25f, [&](float l, float r) { assert(l == 0.25f && r == -0.25f); ++count; });
    assert(count >= 47998 && count <= 48001);
    LinearResampler trimmed; trimmed.reset(44100, 48000); trimmed.trim(1.001); // Reads input 0.1 % faster.
    count = 0;
    for (int n = 0; n < 44100; ++n) trimmed.feed(0, 0, [&](float, float) { ++count; });
    assert(count >= 47950 && count <= 47954); // 48000 / 1.001
    LinearResampler ramp; ramp.reset(44100, 48000);
    std::vector<float> values;
    for (int n = 0; n < 2000; ++n) {
        if (n == 1000) ramp.trim(0.999);
        ramp.feed(float(n), 0, [&](float l, float) { values.push_back(l); });
    }
    for (size_t n = 1; n < values.size(); ++n) {
        float advance = values[n] - values[n - 1];
        assert(advance > 0.9188f * 0.998f && advance < 0.9188f * 1.002f); // 44100 / 48000, within the trim.
    }
}

static void drift() {
    assert(drift_trim(1024, 1024) == 1.0);
    assert(drift_trim(1074, 1024) > 1.0 && std::abs(drift_trim(1074, 1024) - 1.0001) < 1e-9); // 50 frames: 100 ppm.
    assert(std::abs(drift_trim(974, 1024) - 0.9999) < 1e-9);
    assert(std::abs(drift_trim(8192, 1024) - 1.002) < 1e-12 && std::abs(drift_trim(0, 1024) - 0.998) < 1e-12); // ±0.2 % at most.

    // A producer 150 ppm fast into a ring read through a trimmed resampler:
    // the fill settles 75 frames above target rather than climbing, so the
    // ring neither overflows nor runs dry.
    double fill = 1024, factor = 1.0;
    for (int quarter = 0; quarter < 4 * 120; ++quarter) {
        double produced = 44100 * 1.00015 / 4, consumed = 44100 * factor / 4;
        fill += produced - consumed;
        factor = drift_trim(fill, 1024);
    }
    assert(std::abs(fill - 1099) < 2);
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
    drift();
    ring();
    recorder();
}
