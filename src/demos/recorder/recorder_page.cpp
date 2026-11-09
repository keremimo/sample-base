// SPDX-License-Identifier: AGPL-3.0-only
#include "demos/demo_screen.h"
#include "dsp/recorder.h"
#include <algorithm>
#include <cmath>

namespace {
juce::String clock_time(unsigned frames, unsigned rate) {
    unsigned tenths = unsigned(uint64_t(frames) * 10 / (rate ? rate : 1));
    return juce::String(tenths / 600) + ":" + juce::String((tenths / 10) % 60).paddedLeft('0', 2) + "." + juce::String(tenths % 10);
}
// -48 dBFS to 0 dBFS across the meter.
float meter_position(float peak) {
    if (peak <= 0.0f) return 0.0f;
    return juce::jlimit(0.0f, 1.0f, (20.0f * std::log10(peak) + 48.0f) / 48.0f);
}
} // namespace

// The recorder page: input and channel soft keys, transport state and time,
// L/R input meters and the take's waveform with the playhead.
void draw_recorder_page(juce::Graphics& g, const Ui& ui, const DemoView& state) {
    using namespace colours;
    auto label = [&](const juce::String& value, int x, int y, int w, int h, float size, juce::Colour colour,
                     juce::Justification justification = juce::Justification::centredLeft) {
        ui.label(g, value, x, y, w, h, size, colour, justification);
    };
    const auto& view = state.recorder;
    auto source = InputSource(view.source < uint8_t(InputSource::Count) ? view.source : 0);
    auto channels = InputChannels(view.channels < uint8_t(InputChannels::Count) ? view.channels : 0);
    auto mode = Recorder::State(view.state <= 2 ? view.state : 0);
    bool recording = mode == Recorder::State::Recording, playing = mode == Recorder::State::Playing;

    // Soft keys, drawn under the three buttons above the screen.
    const juce::String channel_value = source == InputSource::Off ? "-" : source == InputSource::Mic ? "Mono" : input_channel_labels[size_t(channels)];
    const std::array<std::pair<juce::String, juce::String>, 3> keys {{
        {"INPUT", input_source_labels[size_t(source)]}, {"CHANNEL", channel_value}, {"PAGE", "Synth"}}};
    for (unsigned n = 0; n < keys.size(); ++n) {
        bool live = (n == 0 && source != InputSource::Off) || (n == 1 && input_has_channels(source));
        ui.soft_key(g, int(n), keys[n].first, keys[n].second, n == 2 || live ? text : muted, live);
    }

    // Transport state and time.
    juce::Colour badge_colour = recording ? juce::Colour(0xff4a2226) : playing ? juce::Colour(0xff263b34) : panel;
    g.setColour(badge_colour); g.fillRoundedRectangle(12, 58, 66, 22, 5);
    if (recording) { g.setColour(red); g.fillEllipse(20, 64, 10, 10); }
    if (playing) {
        juce::Path triangle; triangle.addTriangle(20, 63, 20, 75, 30, 69);
        g.setColour(mint); g.fillPath(triangle);
    }
    juce::String word = recording ? "REC" : playing ? "PLAY" : view.frames ? "READY" : "EMPTY";
    label(word, recording || playing ? 34 : 12, 58, recording || playing ? 42 : 66, 22, 12,
          recording ? red : playing ? mint : muted, recording || playing ? juce::Justification::centredLeft : juce::Justification::centred);
    label(clock_time(playing ? view.position : view.frames, view.rate), 88, 52, 130, 34, 28, text);
    label(recording ? "of " + clock_time(view.capacity, view.rate) : playing ? "/ " + clock_time(view.frames, view.rate) : "max " + clock_time(view.capacity, view.rate),
          212, 60, 96, 20, 12, muted, juce::Justification::centredRight);

    // Input meters before channel selection; dim bars are not recorded.
    for (unsigned side = 0; side < 2; ++side) {
        int x = side ? 168 : 12;
        bool used = source != InputSource::Off &&
            (!input_has_channels(source) || channels == InputChannels::Stereo || unsigned(channels) == side + 1);
        float peak = side ? view.right : view.left;
        label(side ? "R" : "L", x, 90, 12, 14, 11, used ? text : muted);
        g.setColour(track); g.fillRoundedRectangle(float(x + 14), 95, 126, 5, 2.5f);
        float width = 126 * meter_position(peak);
        if (source != InputSource::Off && width > 0) {
            g.setColour(peak >= 0.99f ? red : used ? mint : juce::Colour(0xff4f5d69));
            g.fillRoundedRectangle(float(x + 14), 95, std::max(width, 3.0f), 5, 2.5f);
        }
    }

    // Waveform of the take; the scale grows while recording.
    g.setColour(juce::Colour(0xff151c23)); g.fillRoundedRectangle(12, 110, 296, 76, 5);
    g.setColour(track); g.fillRect(12, 148, 296, 1);
    if (view.frames || recording) {
        unsigned scale = std::max(view.scale, 1u);
        unsigned filled = unsigned(uint64_t(view.frames) * RecorderView::columns / scale);
        for (unsigned column = 0; column < RecorderView::columns && column <= filled; ++column) {
            float height = std::max(1.0f, 36.0f * std::sqrt(view.waveform[column] / 255.0f));
            bool heard = playing && uint64_t(column) * scale < uint64_t(view.position) * RecorderView::columns;
            g.setColour(recording ? red.withAlpha(0.85f) : heard ? mint : mint.withAlpha(0.55f));
            g.fillRect(float(12 + column), 148.5f - height, 1.0f, height * 2);
        }
        if (playing) {
            float x = 12 + 296.0f * float(view.position) / float(scale);
            g.setColour(text); g.fillRect(x, 112.0f, 1.0f, 72.0f);
        }
    } else {
        label(source == InputSource::Off ? "Choose an input" : "Press SAMPLE RECORD", 12, 110, 296, 76, 15, muted, juce::Justification::centred);
    }

    label(juce::String::fromUTF8(view.detail.c_str()), 12, 188, 296, 16, 12, view.hint ? amber : muted);
    ui.footer(g, "Rec  Play  Stop   Sample: synth", state.last_input);
}
