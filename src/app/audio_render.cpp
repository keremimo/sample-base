// SPDX-License-Identifier: AGPL-3.0-only
#include "app/audio_render.h"
#include <algorithm>
#include <array>
#include <cmath>

namespace {
constexpr double full_scale = 2147483648.0; // S32 sample to -1..1.

// Plays every queued note. After a queue overflow the backlog is dropped and
// all sound stops, rather than playing stale notes late.
void play_queued_midi(RenderContext& context) {
    auto& state = context.state;
    MidiEvent event;
    if (state.queue.take_overflow()) {
        for (unsigned n = 0; n < 256 && state.queue.pop(event); ++n) {}
        context.synth.midi({0xb0,120,0}); ++state.overruns;
    }
    for (unsigned n = 0; n < 256 && state.queue.pop(event); ++n) context.synth.midi(event);
}

// Records the selected source and adds take playback to `playback`.
void record_input(RenderContext& context, const AudioBlock& input, std::array<float, block_frames * 2>& playback) {
    auto& state = context.state;
    auto source = InputSource(state.source.load(std::memory_order_relaxed));
    auto channels = InputChannels(state.channels.load(std::memory_order_relaxed));
    std::array<float, block_frames * 2> recorded {};
    if (source == InputSource::Usb) state.usb.pull(recorded.data(), block_frames);
    else if (source != InputSource::Off) for (unsigned i = 0; i < recorded.size(); ++i) recorded[i] = float(input[i] / full_scale);
    // About 20 dB per 300 ms of fall, so the 75 ms screen refresh sees peaks.
    context.meter_left *= 0.978f; context.meter_right *= 0.978f;
    for (unsigned n = 0; n < block_frames; ++n) {
        context.meter_left = std::max(context.meter_left, std::abs(recorded[2 * n]));
        context.meter_right = std::max(context.meter_right, std::abs(recorded[2 * n + 1]));
    }
    state.input_left.store(context.meter_left, std::memory_order_relaxed);
    state.input_right.store(context.meter_right, std::memory_order_relaxed);
    select_input_channels(source, channels, recorded.data(), block_frames);
    state.recorder.process(recorded.data(), playback.data(), block_frames, input_route(source, channels), source != InputSource::Off);
}
} // namespace

void render_audio(void* pointer, const AudioBlock& input, AudioBlock& output) {
    auto& context = *static_cast<RenderContext*>(pointer);
    auto& state = context.state;
    play_queued_midi(context);
    context.synth.render(output.data());
    std::array<float, block_frames * 2> playback {};
    record_input(context, input, playback);

    float gain = state.gain.load(), monitor = state.monitor.load();
    for (unsigned i = 0; i < output.size(); ++i) {
        double sample = (output[i] / full_scale + playback[i] + input[i] / full_scale * monitor) * gain;
        output[i] = int32_t(std::clamp(sample, -0.99, 0.99) * 2147483647.0);
    }
}
