// SPDX-License-Identifier: AGPL-3.0-only
#include "demos/ui.h"
#include <algorithm>
#include <cmath>

void Ui::label(juce::Graphics& g, const juce::String& value, int x, int y, int w, int h, float size, juce::Colour colour,
               juce::Justification justification) const {
    // Readable at arm's length on the 320 x 240 panel: no line under 13 px
    // (about 11 px type, the same floor as Melodee's).
    constexpr float smallest = 13;
    g.setColour(colour);
    g.setFont(juce::Font(juce::FontOptions(typeface).withHeight(std::max(size, smallest))));
    g.drawText(value, x, y, w, h, justification, false);
}

void Ui::soft_key(juce::Graphics& g, int index, const juce::String& title, const juce::String& value, juce::Colour value_colour,
                  bool accent) const {
    int x = 8 + index * 104;
    g.setColour(colours::panel); g.fillRoundedRectangle(float(x), 6, 96, 38, 5);
    if (accent) { g.setColour(colours::mint); g.fillRoundedRectangle(float(x), 6, 3, 38, 1.5f); }
    label(g, title, x + 10, 8, 82, 14, 10, colours::muted);
    label(g, value, x + 10, 22, 82, 20, 17, value_colour);
}

void Ui::footer(juce::Graphics& g, const juce::String& hints, const std::string& last_input) const {
    g.setColour(colours::rule); g.fillRect(12, 205, 296, 1);
    label(g, hints, 12, 210, 190, 20, 12, colours::text);
    if (!last_input.empty())
        label(g, juce::String::fromUTF8(last_input.c_str()), 196, 210, 112, 20, 12, colours::mint, juce::Justification::centredRight);
}

juce::Colour shown_light(Rgb colour) {
    if (!colour.r && !colour.g && !colour.b) return juce::Colour(0xff1f2830);
    auto channel = [](uint8_t value) { return uint8_t(std::lround(std::sqrt(value / 127.0f) * 255)); };
    return juce::Colour(channel(colour.r), channel(colour.g), channel(colour.b));
}
