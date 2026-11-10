// SPDX-License-Identifier: AGPL-3.0-only
#include "runtime/screen.h"
#include "runtime/util.h"
#include <stdexcept>

juce::String default_font_path() {
    const juce::String device = "/usr/share/fonts/ttf/LiberationSans-Regular.ttf";
    return juce::File(device).existsAsFile() ? device : "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf";
}

juce::Typeface::Ptr load_typeface(const juce::String& path) {
    juce::MemoryBlock data;
    if (!juce::File(path).loadFileAsData(data)) throw std::runtime_error("Cannot read screen font");
    auto typeface = juce::Typeface::createSystemTypefaceFor(data.getData(), data.getSize());
    if (!typeface) throw std::runtime_error("Cannot load screen font");
    return typeface;
}

void save_png(const juce::Image& image, const std::string& path) {
    juce::FileOutputStream output {juce::File(path)};
    require(output.openedOk() && output.setPosition(0) && output.truncate().wasOk() &&
            juce::PNGImageFormat().writeImageToStream(image, output), "Write screen preview");
}

void draw_prompt(juce::Graphics& g, const juce::Typeface::Ptr& typeface, const std::string& prompt) {
    const juce::Colour background(0xff10151b), text(0xfff0f1eb), muted(0xff98a7b5), mint(0xff8de0bd),
        header(0xff263b34), panel(0xff1b232b), rule(0xff30404b);
    auto label = [&](const juce::String& value, int x, int y, int w, int h, float size, juce::Colour colour) {
        g.setColour(colour);
        g.setFont(juce::Font(juce::FontOptions(typeface).withHeight(size)));
        g.drawText(value, x, y, w, h, juce::Justification::centredLeft, false);
    };
    juce::StringArray lines;
    lines.addTokens(juce::String::fromUTF8(prompt.c_str()), "|", "");
    while (lines.size() < 5) lines.add({});
    g.fillAll(background);
    g.setColour(header); g.fillRoundedRectangle(12, 12, 296, 24, 5);
    label(lines[0], 22, 12, 280, 24, 14, mint);
    label(lines[1], 12, 48, 296, 44, 36, text);
    label(lines[2], 12, 96, 296, 26, 18, text);
    g.setColour(panel); g.fillRoundedRectangle(12, 136, 296, 52, 5);
    label(lines[3], 22, 136, 280, 52, 16, mint);
    g.setColour(rule); g.fillRect(12, 205, 296, 1);
    label(lines[4], 12, 210, 296, 20, 13, muted);
}
