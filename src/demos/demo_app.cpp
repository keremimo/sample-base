// SPDX-License-Identifier: AGPL-3.0-only
#include "demos/demo_app.h"
#include "runtime/screen.h"
#include "runtime/util.h"
#include "surface/controls.h"
#include "demos/demo_screen.h"
#include "demos/synth/synth_demo.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace {
constexpr double full_scale = 2147483648.0; // S32 sample to -1..1.
} // namespace

// Audio thread: the synth, the recorder's take playback and input monitoring,
// mixed at the main volume.
void DemoApp::render(const AudioBlock& input, AudioBlock& output) {
    play_queued_midi(state, synth);
    synth.render(output.data());
    std::array<float, block_frames * 2> playback {};
    recorder_audio.process(state, input, playback);

    float gain = state.gain.load(), monitor = state.monitor.load();
    for (unsigned i = 0; i < output.size(); ++i) {
        double sample = (output[i] / full_scale + playback[i] + input[i] / full_scale * monitor) * gain;
        output[i] = int32_t(std::clamp(sample, -0.99, 0.99) * 2147483647.0);
    }
}

// MIDI thread. Main volume sets the output gain; buttons switch pages and
// drive the page's demo, except while a remote prompt owns the screen; pads
// play the synth and feed the pad touch page.
void DemoApp::surface(const SurfaceEvent& event) {
    const auto& input = event.input;
    if (is_control(input, SurfaceControl::MainVolume)) state.gain.store(main_volume_gain(input.value));
    if (input.kind == SurfaceInput::Kind::Button && input.pressed && !session().prompting.load())
        press_button(SurfaceButton(input.index));
    if (event.pad >= 0) {
        record_pad_touch(state.pads, event);
        queue_pad_note(state, event);
    }
}

// Sample, Pad FX and 16 Levels toggle their page; the right button above the
// screen goes home to the synth, or from there to the recorder. The others
// belong to the page's demo, and the recorder's transport works anywhere.
void DemoApp::press_button(SurfaceButton button) {
    auto page = state.page.load();
    auto toggle = [&](Page wanted) { state.page.store(page == wanted ? Page::Synth : wanted); };
    switch (button) {
    case SurfaceButton::Sample: toggle(Page::Recorder); return;
    case SurfaceButton::PadFx: toggle(Page::Lights); return;
    case SurfaceButton::SixteenLevels: toggle(Page::Pads); return;
    case SurfaceButton::TopRight: state.page.store(page == Page::Synth ? Page::Recorder : Page::Synth); return;
    default: break;
    }
    if (page == Page::Lights && light_show_button(state, button)) return;
    if (page == Page::Pads && (button == SurfaceButton::TopLeft || button == SurfaceButton::TopMiddle)) return;
    recorder_button(state, button);
}

// MIDI thread: external MIDI plays the synth at its own pitches.
void DemoApp::midi(const MidiPortInfo&, const MidiPacket& packet) {
    MidiEvent event;
    if (channel_event(packet, event)) state.queue.push(event);
}

void DemoApp::midi_lost() { state.queue.push({0xb0, 120, 0}); }

// MIDI thread. The page that was left closes before the open one draws, so
// the old page's last lights never cover the new page's first frame. The
// recorder's lights show unless the light show is running.
void DemoApp::lights(SurfaceLights& lights, Clock::time_point now) {
    auto page = state.page.load();
    uint32_t held = session().held.load();
    if (page != Page::Lights && light_show_lights.close(lights, state)) recorder_lights.forget();
    if (page != Page::Pads) pad_lights.close(lights);
    if (page == Page::Lights) light_show_lights.update(lights, state, held, now);
    if (page == Page::Pads) pad_lights.update(lights, state.pads, held, now);
    if (!light_show_lights.running()) recorder_lights.update(lights, state);
}

void DemoApp::draw(juce::Graphics& g) {
    if (!ui) ui = std::make_unique<Ui>(load_typeface());
    draw_demo_screen(g, *ui, feed.frame(state, session()));
}

void DemoApp::service(Hardware& hardware) { apply_source_request(state, hardware, usb_device); }

std::string DemoApp::command(const std::string& action, const std::string& argument, Hardware& hardware) {
    if (action == "gain" || action == "monitor") {
        unsigned value = 0;
        if (!parse_unsigned(argument, value) || value > 100) return "ERR use a percentage from 0 to 100";
        (action == "gain" ? state.gain : state.monitor).store(value / 100.0f); return "OK " + action + ' ' + argument;
    }
    if (action == "page") {
        auto name = std::find(page_names.begin(), page_names.end(), argument);
        if (name == page_names.end()) return "ERR use page synth, recorder, lights or pads";
        state.page.store(Page(name - page_names.begin())); return "OK page " + argument;
    }
    if (action == "lights") return light_show_command(state, argument);
    return recorder_command(state, hardware, usb_device, action, argument);
}

std::string DemoApp::command_names() const {
    return "gain, monitor, page, lights, input, source, channels, transport or recorder";
}

std::string DemoApp::status() const {
    int last = session().last_pad.load();
    auto touch = last >= 0 ? state.pads.touch(unsigned(last)) : PadTouch {};
    auto page = state.page.load();
    char fields[256];
    snprintf(fields, sizeof(fields), "pressure=%u peak_pressure=%u queue_overruns=%u gain=%.2f monitor=%.2f",
             unsigned(touch.pressure), unsigned(touch.peak), state.overruns.load(), double(state.gain.load()), double(state.monitor.load()));
    return std::string(fields) + ' ' + recorder_status(state) + " page=" + page_names[size_t(page)] +
           " light_scene=" + std::to_string(page == Page::Lights ? light_scene_at(state.light_position.load()) + 1 : 0u);
}
