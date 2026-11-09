// SPDX-License-Identifier: AGPL-3.0-only
#include "demos/demo_screen.h"
#include "demos/synth/pad_scale.h"

// The synth page: the last pad and its velocity, the output meter and the
// pads labelled with the notes they play.
void draw_synth_page(juce::Graphics& g, const Ui& ui, const DemoView& state) {
    using namespace colours;
    auto label = [&](const juce::String& value, int x, int y, int w, int h, float size, juce::Colour colour,
                     juce::Justification justification = juce::Justification::centredLeft) {
        ui.label(g, value, x, y, w, h, size, colour, justification);
    };
    label("sample-base", 12, 7, 220, 27, 25, text);
    g.setColour(badge); g.fillRoundedRectangle(245, 13, 63, 19, 5);
    label("PLAY", 260, 13, 45, 19, 12, mint);
    label("Built-in synth", 12, 38, 130, 18, 13, muted);
    label("Beep / Boop", 12, 56, 190, 34, 32, text);
    label(state.last_pad >= 0 ? "Pad " + juce::String(state.last_pad + 1) + "  /  " + juce::String(state.velocity) : "Tap a pad to play", 12, 94, 210, 18, 13, muted);
    g.setColour(track); g.fillRoundedRectangle(237, 63, 71, 6, 3);
    g.setColour(mint); g.fillRoundedRectangle(237, 63, juce::jlimit(1.0f, 71.0f, state.peak * 142), 6, 3);
    label("C major", 237, 72, 80, 14, 12, muted);
    label("Pad FX: lights", 237, 85, 76, 12, 11, muted);
    label("16 Levels: pads", 237, 97, 76, 12, 11, muted);
    label("SEQ: drums", 237, 109, 76, 12, 11, muted);
    for (int pad = 0; pad < 16; ++pad) {
        int x = 12 + (pad % 4) * 77, y = 123 + (3 - pad / 4) * 19;
        bool held = (state.held & (1u << pad)) != 0;
        g.setColour(held ? mint : key);
        g.fillRoundedRectangle(float(x), float(y), 65, 16, 3);
        label(pad_note_labels[pad], x + 7, y, 57, 16, 11, held ? background : text);
    }
    ui.footer(g, "Push: stock app   Sample: recorder", state.last_input);
    if (state.xruns) label("Audio recoveries: " + juce::String(state.xruns), 150, 39, 160, 16, 11, muted);
}
