// SPDX-License-Identifier: AGPL-3.0-only
#include "demos/demo_screen.h"

void draw_demo_screen(juce::Graphics& g, const Ui& ui, const DemoView& view) {
    g.fillAll(colours::background);
    switch (view.page) {
    case Page::Recorder: draw_recorder_page(g, ui, view); break;
    case Page::Lights: draw_light_show_page(g, ui, view); break;
    case Page::Pads: draw_pad_touch_page(g, ui, view); break;
    case Page::Seq: draw_sequencer_page(g, ui, view); break;
    case Page::System: draw_system_page(g, ui, view); break;
    default: draw_synth_page(g, ui, view); break;
    }
}
