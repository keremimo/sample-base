// SPDX-License-Identifier: AGPL-3.0-only
#include "screen.h"
#include "pads.h"
#include "recorder.h"
#include <cmath>
#include <stdexcept>

namespace {
const juce::Colour background(0xff10151b), text(0xfff0f1eb), muted(0xff98a7b5), mint(0xff8de0bd), red(0xffff6b6b),
    amber(0xfff0c674), panel(0xff1b232b), track(0xff29333e), rule(0xff30404b);
void draw_label(juce::Graphics& g, const juce::Typeface::Ptr& typeface, const juce::String& value, int x, int y, int w, int h,
           float size, juce::Colour colour, juce::Justification justification = juce::Justification::centredLeft) {
    g.setColour(colour);
    g.setFont(juce::Font(juce::FontOptions(typeface).withHeight(size)));
    g.drawText(value, x, y, w, h, justification, false);
}
juce::String clock_time(unsigned frames, unsigned rate) {
    unsigned tenths = unsigned(uint64_t(frames) * 10 / (rate ? rate : 1));
    return juce::String(tenths / 600) + ":" + juce::String((tenths / 10) % 60).paddedLeft('0', 2) + "." + juce::String(tenths % 10);
}
// -48 dBFS to 0 dBFS across the meter.
float meter_position(float peak) {
    if (peak <= 0.0f) return 0.0f;
    return juce::jlimit(0.0f, 1.0f, (20.0f * std::log10(peak) + 48.0f) / 48.0f);
}
}

Screen::Screen(const juce::String& font_path) {
    juce::MemoryBlock data;
    if (!juce::File(font_path).loadFileAsData(data)) throw std::runtime_error("Cannot read screen font");
    typeface = juce::Typeface::createSystemTypefaceFor(data.getData(), data.getSize());
    if (!typeface) throw std::runtime_error("Cannot load screen font");
}

juce::Image Screen::draw(const ScreenState& state) {
    juce::Graphics g(image);
    g.fillAll(background);
    auto label = [&](juce::String value, int x, int y, int w, int h, float size, juce::Colour colour,
                     juce::Justification justification = juce::Justification::centredLeft) {
        draw_label(g, typeface, value, x, y, w, h, size, colour, justification);
    };
    if (!state.prompt.empty()) {
        juce::StringArray lines;
        lines.addTokens(juce::String::fromUTF8(state.prompt.c_str()), "|", "");
        while (lines.size() < 5) lines.add({});
        g.setColour(juce::Colour(0xff263b34)); g.fillRoundedRectangle(12, 12, 296, 24, 5);
        label(lines[0], 22, 12, 280, 24, 14, mint);
        label(lines[1], 12, 48, 296, 44, 36, text);
        label(lines[2], 12, 96, 296, 26, 18, text);
        g.setColour(panel); g.fillRoundedRectangle(12, 136, 296, 52, 5);
        label(lines[3], 22, 136, 280, 52, 16, mint);
        g.setColour(rule); g.fillRect(12, 205, 296, 1);
        label(lines[4], 12, 210, 296, 20, 12, muted);
        return image;
    }
    if (state.recorder_page) {
        draw_recorder(g, state);
        return image;
    }
    label("sample-base", 12, 7, 220, 27, 25, text);
    g.setColour(juce::Colour(0xff263b34)); g.fillRoundedRectangle(245, 13, 63, 19, 5);
    label("PLAY", 260, 13, 45, 19, 12, mint);
    label("Built-in synth", 12, 38, 130, 18, 13, muted);
    label("Beep / Boop", 12, 56, 190, 34, 32, text);
    label(state.last_pad >= 0 ? "Pad " + juce::String(state.last_pad + 1) + "  /  " + juce::String(state.velocity) : "Tap a pad to play", 12, 94, 210, 18, 13, muted);
    g.setColour(track); g.fillRoundedRectangle(237, 67, 71, 6, 3);
    g.setColour(mint); g.fillRoundedRectangle(237, 67, juce::jlimit(1.0f, 71.0f, state.peak * 142), 6, 3);
    label("C major", 237, 83, 80, 18, 12, muted);
    for (int pad = 0; pad < 16; ++pad) {
        int x = 12 + (pad % 4) * 77, y = 123 + (3 - pad / 4) * 19;
        bool held = (state.held & (1u << pad)) != 0;
        g.setColour(held ? mint : juce::Colour(0xff26323d));
        g.fillRoundedRectangle(float(x), float(y), 65, 16, 3);
        label(pad_note_labels[pad], x + 7, y, 57, 16, 11, held ? background : text);
    }
    g.setColour(rule); g.fillRect(12, 205, 296, 1);
    label("Push: stock app   Sample: recorder", 12, 210, 190, 20, 12, text);
    if (!state.last_input.empty())
        label(juce::String::fromUTF8(state.last_input.c_str()), 196, 210, 112, 20, 12, mint, juce::Justification::centredRight);
    if (state.xruns) label("Audio recoveries: " + juce::String(state.xruns), 150, 39, 160, 16, 11, muted);
    return image;
}

void Screen::draw_recorder(juce::Graphics& g, const ScreenState& state) {
    auto label = [&](juce::String value, int x, int y, int w, int h, float size, juce::Colour colour,
                     juce::Justification justification = juce::Justification::centredLeft) {
        draw_label(g, typeface, value, x, y, w, h, size, colour, justification);
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
        int x = 8 + int(n) * 104;
        g.setColour(panel); g.fillRoundedRectangle(float(x), 6, 96, 38, 5);
        bool live = (n == 0 && source != InputSource::Off) || (n == 1 && input_has_channels(source));
        if (live) { g.setColour(mint); g.fillRoundedRectangle(float(x), 6, 3, 38, 1.5f); }
        label(keys[n].first, x + 10, 8, 82, 14, 10, muted);
        label(keys[n].second, x + 10, 22, 82, 20, 17, n == 2 || live ? text : muted);
    }

    // Transport state and time.
    juce::Colour badge = recording ? juce::Colour(0xff4a2226) : playing ? juce::Colour(0xff263b34) : panel;
    g.setColour(badge); g.fillRoundedRectangle(12, 58, 66, 22, 5);
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
    g.setColour(rule); g.fillRect(12, 205, 296, 1);
    label("Rec  Play  Stop   Sample: synth", 12, 210, 190, 20, 12, text);
    if (!state.last_input.empty())
        label(juce::String::fromUTF8(state.last_input.c_str()), 196, 210, 112, 20, 12, mint, juce::Justification::centredRight);
}
