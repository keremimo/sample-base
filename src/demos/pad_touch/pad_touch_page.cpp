// SPDX-License-Identifier: AGPL-3.0-only
#include "demos/demo_screen.h"
#include "demos/pad_touch/pad_touch.h"
#include <algorithm>

// The pad touch page: every pad's latest velocity with a pressure fill while
// held, the last pad's velocity, pressure and peak, and five seconds of
// pressure with each strike marked.
void draw_pad_touch_page(juce::Graphics& g, const Ui& ui, const DemoView& state) {
    using namespace colours;
    auto label = [&](const juce::String& value, int x, int y, int w, int h, float size, juce::Colour colour,
                     juce::Justification justification = juce::Justification::centredLeft) {
        ui.label(g, value, x, y, w, h, size, colour, justification);
    };
    auto force_shown = [](float force) { return shown_light(pad_touch::force_colour(force)); };
    const auto& view = state.pads;
    int last = state.last_pad >= 0 && state.last_pad < 16 ? state.last_pad : -1;

    label("PAD TOUCH", 12, 8, 190, 14, 10, mint);
    label(last >= 0 ? "Pad " + juce::String(last + 1) : "Hit a pad", 12, 20, 190, 26, 22, text);
    // Only the right button above the screen is a soft key here.
    ui.soft_key(g, 2, "PAGE", "Synth", text);

    // Pads, pad 1 bottom left, with the velocity of their latest strike. A
    // held pad is tinted by its strike and fills from the bottom with pressure.
    for (unsigned pad = 0; pad < 16; ++pad) {
        float x = 12 + float(pad % 4) * 37, y = 56 + float(3 - pad / 4) * 37;
        bool held = (state.held & (1u << pad)) != 0;
        float fill = held ? 33 * view.pressure[pad] / 127.0f : 0;
        g.setColour(key); g.fillRoundedRectangle(x, y, 33, 33, 3);
        if (held) {
            g.setColour(force_shown(pad_touch::held_force(view.velocity[pad], 0, pad_touch::flash_ms)).withAlpha(0.5f));
            g.fillRoundedRectangle(x, y, 33, 33, 3);
            if (fill > 0) { g.setColour(force_shown(view.pressure[pad] / 127.0f)); g.fillRoundedRectangle(x, y + 33 - fill, 33, fill, 3); }
        }
        // Dark text once the pressure fill reaches the number.
        if (view.velocity[pad]) label(juce::String(view.velocity[pad]), int(x) + 4, int(y) + 2, 27, 14, 12, fill > 19 ? background : held ? text : muted);
        else label(juce::String(pad + 1), int(x) + 4, int(y) + 2, 27, 14, 10, track.brighter(0.4f));
    }

    // The latest pad's strike and pressure; the tick marks the highest pressure of this touch.
    uint8_t velocity = last >= 0 ? view.velocity[size_t(last)] : 0, pressure = last >= 0 ? view.pressure[size_t(last)] : 0;
    uint8_t peak = last >= 0 ? view.peak[size_t(last)] : 0;
    auto meter = [&](const juce::String& name, int y, uint8_t value) {
        label(name, 172, y, 100, 16, 11, muted);
        label(juce::String(value), 268, y, 40, 16, 14, text, juce::Justification::centredRight);
        g.setColour(track); g.fillRoundedRectangle(172, float(y + 18), 136, 6, 3);
        float width = 136 * value / 127.0f;
        if (width > 0) { g.setColour(force_shown(value / 127.0f)); g.fillRoundedRectangle(172, float(y + 18), std::max(width, 3.0f), 6, 3); }
    };
    meter("Velocity", 54, velocity);
    meter("Pressure  peak " + juce::String(peak), 84, pressure);
    if (peak) { g.setColour(text); g.fillRect(172 + 134 * peak / 127.0f, 100.0f, 2.0f, 10.0f); }

    // Pressure over the last five seconds, the hardest pad in each frame;
    // dots mark strikes at their velocity.
    label("Pressure, strikes: 5 s", 172, 116, 136, 14, 10, muted);
    g.setColour(juce::Colour(0xff151c23)); g.fillRoundedRectangle(172, 132, 136, 68, 5);
    for (unsigned n = 0; n < PadsView::trace_length; ++n) {
        float x = 172 + n * 2.0f;
        if (view.trace[n]) {
            float height = std::max(1.0f, 62 * view.trace[n] / 127.0f);
            g.setColour(force_shown(view.trace[n] / 127.0f)); g.fillRect(x, 197 - height, 2.0f, height);
        }
        if (view.strikes[n]) { g.setColour(text); g.fillEllipse(x - 1, 195 - 62 * view.strikes[n] / 127.0f, 4, 4); }
    }

    ui.footer(g, "16 Levels: synth   Pad FX: lights", state.last_input);
}
