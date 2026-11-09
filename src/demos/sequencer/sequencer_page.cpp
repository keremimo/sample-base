// SPDX-License-Identifier: AGPL-3.0-only
#include "demos/demo_screen.h"
#include "demos/sequencer/sequencer.h"

// The SEQ page: eight lanes of sixteen steps, a step's brightness its
// velocity, the selected lane highlighted and the playhead over the column
// playing. A lane's bar flashes as its drum sounds. Below: tempo, transport
// and the step playing.
void draw_sequencer_page(juce::Graphics& g, const Ui& ui, const DemoView& state) {
    using namespace colours;
    auto label = [&](const juce::String& value, int x, int y, int w, int h, float size, juce::Colour colour,
                     juce::Justification justification = juce::Justification::centredLeft) {
        ui.label(g, value, x, y, w, h, size, colour, justification);
    };
    const auto& view = state.seq;
    unsigned selected = view.track % seq::tracks;
    auto lane_colour = [](unsigned track, float level = 1) { return shown_light(seq::track_colour(track, level)); };

    ui.soft_key(g, 0, "PADS", view.drums ? "Drums" : "Steps", text, view.drums);
    ui.soft_key(g, 1, "TRACK", juce::String::charToString(juce::juce_wchar(seq::track_letter(selected))) + "  " + seq::drum_names[selected],
                lane_colour(selected));
    ui.soft_key(g, 2, "PAGE", "Synth", text);

    // Lane A at the top; steps in groups of four, one beat each.
    constexpr int top = 50, pitch = 16, cell = 12, first_step = 70;
    auto step_x = [](unsigned step) { return first_step + int(step) * 14 + int(step / 4) * 4; };
    for (unsigned track = 0; track < seq::tracks; ++track) {
        int y = top + int(track) * pitch;
        bool silenced = (view.muted >> track) & 1, hit = (view.hits >> track) & 1;
        if (track == selected) { g.setColour(panel); g.fillRoundedRectangle(8, float(y - 1), 304, float(cell + 2), 3); }
        g.setColour(hit ? lane_colour(track) : lane_colour(track, 0.25f)); g.fillRect(12, y, 3, cell);
        label(juce::String::charToString(juce::juce_wchar(seq::track_letter(track))) + " " + seq::drum_labels[track], 19, y - 1, 48, cell + 2,
              10, silenced ? muted : text);
        if (silenced) label("M", 58, y - 1, 10, cell + 2, 10, red);
        for (unsigned step = 0; step < seq::steps; ++step) {
            uint8_t velocity = view.steps[track][step];
            auto colour = velocity ? lane_colour(track, 0.35f + 0.65f * velocity / 127) : step % 4 ? key : rule;
            if (velocity && silenced) colour = colour.interpolatedWith(background, 0.6f);
            g.setColour(colour); g.fillRoundedRectangle(float(step_x(step)), float(y), float(cell), float(cell), 2);
        }
    }
    if (view.step >= 0) {
        g.setColour((view.armed ? red : text).withAlpha(0.28f));
        g.fillRoundedRectangle(float(step_x(unsigned(view.step)) - 1), float(top - 3), float(cell + 2), float(seq::tracks * pitch + 2), 2);
    }

    // Tempo, transport and position.
    int y = top + int(seq::tracks) * pitch + 4;
    label(tempo_text(view.tempo), 12, y, 54, 22, 20, text);
    label("BPM", 66, y + 4, 30, 16, 10, muted);
    bool playing = view.step >= 0;
    label(playing ? "PLAYING" : "STOPPED", 104, y, 70, 22, 12, playing ? mint : muted);
    if (view.armed) {
        g.setColour(red); g.fillRoundedRectangle(170, float(y + 3), 34, 16, 4);
        label("REC", 170, y + 3, 34, 16, 11, background, juce::Justification::centred);
    }
    label(playing ? "Step " + juce::String(view.step + 1) + " / 16" : "Play to start", 208, y, 100, 22, 12, playing ? text : muted,
          juce::Justification::centredRight);

    ui.footer(g, view.drums ? "Pads 1-8: drums  9-16: mute" : "Pads: steps  Pad Bank: track", state.last_input);
}
