// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <juce_graphics/juce_graphics.h>
#include <string>

// Screen helpers for the runtime and for apps. The panel is 320 x 240;
// run_app() presents what App::draw() paints.
constexpr int screen_width = 320, screen_height = 240;

// The Liberation Sans file on the MPC, or its Debian path for host builds.
juce::String default_font_path();
// Loads a font file once, for drawing text. Throws when it cannot.
juce::Typeface::Ptr load_typeface(const juce::String& path = default_font_path());
void save_png(const juce::Image& image, const std::string& path);

// A remote tool's instruction (the `prompt` command), drawn over the whole
// screen in place of the app: `|`-separated header, title, detail, status
// and footer.
void draw_prompt(juce::Graphics& g, const juce::Typeface::Ptr& typeface, const std::string& prompt);
