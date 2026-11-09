// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include "demos/page.h"
#include "demos/pad_touch/pad_touch_demo.h"
#include "demos/recorder/recorder_demo.h"
#include "demos/ui.h"
#include <cstdint>
#include <string>

// What the demo screen shows in one frame. ScreenFeed copies it out of the
// shared state once per frame, so drawing never reads state that other
// threads are changing, and the offline previews can make one up.
struct DemoView {
    Page page = Page::Synth;
    uint32_t held = 0;
    int last_pad = -1, velocity = 0;
    float peak = 0;
    unsigned xruns = 0;
    std::string last_input; // Name of the most recent control.
    RecorderView recorder;
    unsigned light_position = 0; // Milliseconds into the light show loop.
    PadsView pads;
};

// The page `view.page` names, over the whole screen.
void draw_demo_screen(juce::Graphics& g, const Ui& ui, const DemoView& view);

// One page each, in demos/<demo>/<demo>_page.cpp.
void draw_synth_page(juce::Graphics& g, const Ui& ui, const DemoView& view);
void draw_recorder_page(juce::Graphics& g, const Ui& ui, const DemoView& view);
void draw_light_show_page(juce::Graphics& g, const Ui& ui, const DemoView& view);
void draw_pad_touch_page(juce::Graphics& g, const Ui& ui, const DemoView& view);
