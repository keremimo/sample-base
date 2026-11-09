// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include "surface/leds.h"
#include <juce_graphics/juce_graphics.h>
#include <string>

// The demo pages' shared look: palette, text, the soft keys under the three
// buttons above the screen and the footer. Screen thread only.
namespace colours {
inline const juce::Colour background {0xff10151b}, text {0xfff0f1eb}, muted {0xff98a7b5}, mint {0xff8de0bd},
    red {0xffff6b6b}, amber {0xfff0c674}, panel {0xff1b232b}, track {0xff29333e}, rule {0xff30404b},
    key {0xff26323d}, badge {0xff263b34};
} // namespace colours

class Ui {
public:
    explicit Ui(juce::Typeface::Ptr typeface) : typeface(std::move(typeface)) {}
    void label(juce::Graphics& g, const juce::String& value, int x, int y, int w, int h, float size, juce::Colour colour,
               juce::Justification justification = juce::Justification::centredLeft) const;
    // The box under button `index` above the screen (0 left, 2 right): a
    // small title and its value. `accent` marks a live choice.
    void soft_key(juce::Graphics& g, int index, const juce::String& title, const juce::String& value, juce::Colour value_colour,
                  bool accent = false) const;
    // The rule along the bottom, the page's button hints and the name of the
    // last control touched.
    void footer(juce::Graphics& g, const juce::String& hints, const std::string& last_input) const;
private:
    juce::Typeface::Ptr typeface;
};

// A surface light as the screen shows it. LED drive is squared for even
// fades; the LCD has its own gamma, so the square root shows roughly what
// the eye sees on the pads. An unlit light is drawn as a dark key.
juce::Colour shown_light(Rgb colour);
