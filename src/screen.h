// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <juce_graphics/juce_graphics.h>

struct ScreenState {
    uint32_t held = 0;
    int last_pad = -1, velocity = 0;
    float peak = 0;
    unsigned xruns = 0;
};
class Screen {
public:
    explicit Screen(const juce::String& font_path);
    juce::Image draw(const ScreenState& state);
private:
    juce::Typeface::Ptr typeface;
    juce::Image image {juce::Image::ARGB, 320, 240, true};
};
