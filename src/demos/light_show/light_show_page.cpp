// SPDX-License-Identifier: AGPL-3.0-only
#include "demos/demo_screen.h"
#include "demos/light_show/light_show.h"
#include <algorithm>

// The light show page: the scene playing, its progress, and a mirror of the
// pad, fader and bank lights.
void draw_light_show_page(juce::Graphics& g, const Ui& ui, const DemoView& state) {
    using namespace colours;
    auto label = [&](const juce::String& value, int x, int y, int w, int h, float size, juce::Colour colour,
                     juce::Justification justification = juce::Justification::centredLeft) {
        ui.label(g, value, x, y, w, h, size, colour, justification);
    };
    unsigned position = state.light_position % light_show_length();
    unsigned scene = light_scene_at(position), into = position - light_scene_start(scene);
    auto frame = light_frame(position);

    const std::array<std::pair<juce::String, juce::String>, 3> keys {{{"SCENE", "Previous"}, {"SCENE", "Next"}, {"PAGE", "Synth"}}};
    for (unsigned n = 0; n < keys.size(); ++n) ui.soft_key(g, int(n), keys[n].first, keys[n].second, text);

    label("LIGHT SHOW", 12, 54, 160, 14, 10, mint);
    label(light_scenes[scene].name, 12, 68, 168, 30, 24, text);
    label(light_scenes[scene].detail, 12, 100, 168, 18, 12, muted);
    label("Scene " + juce::String(scene + 1) + " of " + juce::String(int(light_scenes.size())), 12, 146, 168, 18, 12, text);
    // One segment per scene; the current one fills as it plays.
    for (unsigned n = 0; n < light_scenes.size(); ++n) {
        float x = 12 + n * 17.0f;
        g.setColour(track); g.fillRoundedRectangle(x, 170, 13, 5, 2.5f);
        float fill = n < scene ? 1.0f : n == scene ? float(into) / float(light_scenes[n].length_ms) : 0.0f;
        if (fill > 0) { g.setColour(mint); g.fillRoundedRectangle(x, 170, std::max(13 * fill, 3.0f), 5, 2.5f); }
    }

    // Mirror of the surface: fader light, pads (pad 1 bottom left) and bank lights A-H.
    g.setColour(shown_light(frame.fader)); g.fillRoundedRectangle(184, 56, 5, 108, 2.5f);
    for (unsigned pad = 0; pad < surface_pad_lights; ++pad) {
        float x = 196 + float(pad % 4) * 28, y = 56 + float(3 - pad / 4) * 28;
        g.setColour(shown_light(frame.pads[pad])); g.fillRoundedRectangle(x, y, 24, 24, 3);
    }
    for (unsigned bank = 0; bank < surface_bank_lights; ++bank) {
        g.setColour(frame.banks[bank] ? text : track);
        g.fillEllipse(196 + bank * (101.0f / 7), 170, 7, 7);
    }

    ui.footer(g, "Pad FX: synth   Sample: recorder", state.last_input);
}
