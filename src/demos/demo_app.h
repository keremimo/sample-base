// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include "runtime/app.h"
#include "demos/demo_state.h"
#include "demos/light_show/light_show_demo.h"
#include "demos/pad_touch/pad_touch_demo.h"
#include "demos/recorder/recorder_demo.h"
#include "demos/screen_feed.h"
#include "demos/synth/synth.h"
#include "demos/ui.h"
#include <memory>
#include <string>

// The bundled proofs of concept as one App, one page each:
//   synth       demos/synth/       pads play C major; the home page
//   recorder    demos/recorder/    Sample: record and play the inputs
//   light show  demos/light_show/  Pad FX: every surface light
//   pad touch   demos/pad_touch/   16 Levels: velocity and pressure
// DemoApp only routes: each runtime hook goes to the demos that use it, and
// the page buttons choose which page shows.
class DemoApp : public App {
public:
    // `usb_device` is the USB-C input PCM, or "auto" to find the gadget.
    explicit DemoApp(std::string usb_device) : usb_device(std::move(usb_device)) {}

    void render(const AudioBlock& input, AudioBlock& output) override;
    void surface(const SurfaceEvent& event) override;
    void midi(const MidiPortInfo& port, const MidiPacket& packet) override;
    void midi_lost() override;
    void lights(SurfaceLights& lights, Clock::time_point now) override;
    void draw(juce::Graphics& g) override;
    void service(Hardware& hardware) override;
    std::string command(const std::string& action, const std::string& argument, Hardware& hardware) override;
    std::string command_names() const override;
    std::string status() const override;

private:
    void press_button(SurfaceButton button);

    const std::string usb_device;
    DemoState state;

    Synth synth;                  // Audio thread.
    RecorderAudio recorder_audio; // Audio thread.

    LightShowLights light_show_lights; // MIDI thread.
    PadTouchLights pad_lights;         // MIDI thread.
    RecorderLights recorder_lights;    // MIDI thread.

    std::unique_ptr<Ui> ui; // Screen thread; loads its font on the first frame.
    ScreenFeed feed;        // Screen thread.
};
