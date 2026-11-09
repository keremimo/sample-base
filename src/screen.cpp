// SPDX-License-Identifier: AGPL-3.0-only
#include "screen.h"
#include "pads.h"
#include <stdexcept>

Screen::Screen(const juce::String& font_path) {
    juce::MemoryBlock data;
    if (!juce::File(font_path).loadFileAsData(data)) throw std::runtime_error("Cannot read screen font");
    typeface = juce::Typeface::createSystemTypefaceFor(data.getData(), data.getSize());
    if (!typeface) throw std::runtime_error("Cannot load screen font");
}

juce::Image Screen::draw(const ScreenState& state) {
    juce::Graphics g(image);
    const juce::Colour background(0xff10151b), text(0xfff0f1eb), muted(0xff98a7b5), mint(0xff8de0bd);
    g.fillAll(background);
    auto label = [&](juce::String value, int x, int y, int w, int h, float size, juce::Colour colour,
                     juce::Justification justification = juce::Justification::centredLeft) {
        g.setColour(colour);
        g.setFont(juce::Font(juce::FontOptions(typeface).withHeight(size)));
        g.drawText(value, x, y, w, h, justification, false);
    };
    if (!state.prompt.empty()) {
        juce::StringArray lines;
        lines.addTokens(juce::String::fromUTF8(state.prompt.c_str()), "|", "");
        while (lines.size() < 5) lines.add({});
        g.setColour(juce::Colour(0xff263b34)); g.fillRoundedRectangle(12, 12, 296, 24, 5);
        label(lines[0], 22, 12, 280, 24, 14, mint);
        label(lines[1], 12, 48, 296, 44, 36, text);
        label(lines[2], 12, 96, 296, 26, 18, text);
        g.setColour(juce::Colour(0xff1b232b)); g.fillRoundedRectangle(12, 136, 296, 52, 5);
        label(lines[3], 22, 136, 280, 52, 16, mint);
        g.setColour(juce::Colour(0xff30404b)); g.fillRect(12, 205, 296, 1);
        label(lines[4], 12, 210, 296, 20, 12, muted);
        return image;
    }
    label("sample-base", 12, 7, 220, 27, 25, text);
    g.setColour(juce::Colour(0xff263b34)); g.fillRoundedRectangle(245, 13, 63, 19, 5);
    label("PLAY", 260, 13, 45, 19, 12, mint);
    label("Built-in synth", 12, 38, 130, 18, 13, muted);
    label("Beep / Boop", 12, 56, 190, 34, 32, text);
    label(state.last_pad >= 0 ? "Pad " + juce::String(state.last_pad + 1) + "  /  " + juce::String(state.velocity) : "Tap a pad to play", 12, 94, 210, 18, 13, muted);
    g.setColour(juce::Colour(0xff29333e)); g.fillRoundedRectangle(237, 67, 71, 6, 3);
    g.setColour(mint); g.fillRoundedRectangle(237, 67, juce::jlimit(1.0f, 71.0f, state.peak * 142), 6, 3);
    label("C major", 237, 83, 80, 18, 12, muted);
    for (int pad = 0; pad < 16; ++pad) {
        int x = 12 + (pad % 4) * 77, y = 123 + (3 - pad / 4) * 19;
        bool held = (state.held & (1u << pad)) != 0;
        g.setColour(held ? mint : juce::Colour(0xff26323d));
        g.fillRoundedRectangle(float(x), float(y), 65, 16, 3);
        label(pad_note_labels[pad], x + 7, y, 57, 16, 11, held ? background : text);
    }
    g.setColour(juce::Colour(0xff30404b)); g.fillRect(12, 205, 296, 1);
    label("Push: stock app", 12, 210, 140, 20, 13, text);
    if (!state.last_input.empty())
        label(juce::String::fromUTF8(state.last_input.c_str()), 150, 210, 158, 20, 13, mint, juce::Justification::centredRight);
    if (state.xruns) label("Audio recoveries: " + juce::String(state.xruns), 150, 39, 160, 16, 11, muted);
    return image;
}
