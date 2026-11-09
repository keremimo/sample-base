// SPDX-License-Identifier: AGPL-3.0-only
#include "app/surface_events.h"
#include "controls.h"
#include "pads.h"
#include "app/light_show.h"

namespace {
// The buttons above the screen are soft keys for the labels drawn under
// them, so on the synth page they only open the recorder. Sample toggles the
// recorder page and Pad FX the light show.
void press_button(AppState& state, SurfaceButton button) {
    auto page = state.page.load();
    bool recorder = page == Page::Recorder, lights_page = page == Page::Lights;
    switch (button) {
    case SurfaceButton::Sample: state.page.store(recorder ? Page::Synth : Page::Recorder); break;
    case SurfaceButton::PadFx: state.page.store(lights_page ? Page::Synth : Page::Lights); break;
    case SurfaceButton::TopLeft: {
        if (lights_page) { state.scene_request.store(next_light_scene(state, -1)); break; }
        if (recorder) {
            // The service thread applies the source; step from one still pending.
            int pending = state.source_request.load();
            auto current = pending >= 0 ? InputSource(pending) : InputSource(state.source.load());
            state.source_request.store(int(next_input_source(current)));
        }
        state.page.store(Page::Recorder); break;
    }
    case SurfaceButton::TopMiddle:
        if (lights_page) { state.scene_request.store(next_light_scene(state, 1)); break; }
        if (recorder && input_has_channels(InputSource(state.source.load())))
            state.channels.store(uint8_t(next_input_channels(InputChannels(state.channels.load()))));
        state.page.store(Page::Recorder); break;
    case SurfaceButton::TopRight: state.page.store(page == Page::Synth ? Page::Recorder : Page::Synth); break;
    case SurfaceButton::SampleRecord: state.recorder.record(); state.page.store(Page::Recorder); break;
    case SurfaceButton::Play: state.recorder.play(); state.page.store(Page::Recorder); break;
    case SurfaceButton::Stop: state.recorder.stop(); break;
    default: break;
    }
}
} // namespace

void handle_surface_event(AppState& state, MidiEvent event, bool syncing) {
    auto input = surface_input(event);
    if (is_button(input, SurfaceButton::WheelPush) && input.pressed && state.push_exit.load()) { state.running.store(false); return; }
    if (is_control(input, SurfaceControl::MainVolume)) state.gain.store(main_volume_gain(input.value));
    // Replies to the position request are not moves; the wheel's reply is a zero step.
    bool moved = input.kind == SurfaceInput::Kind::Control &&
        (is_control(input, SurfaceControl::Wheel) ? input.delta != 0 : !syncing);
    if (moved || input.pressed) state.last_input.store(pack_last_input(input));
    if (input.pressed && input.kind == SurfaceInput::Kind::Button && !state.prompting.load()) press_button(state, SurfaceButton(input.index));
    play_pad(state, event);
}

void play_pad(AppState& state, MidiEvent event) {
    int pad = pad_index(event);
    if (pad < 0) return;
    unsigned kind = event.status & 0xf0;
    if (kind == 0xa0) return; // Polyphonic pressure is not used.
    bool down = kind == 0x90 && event.value != 0;
    if (down) { state.held.fetch_or(1u << pad); state.last_pad.store(pad); state.velocity.store(event.value); }
    else state.held.fetch_and(~(1u << pad));
    state.queue.push({uint8_t(down ? 0x90 : 0x80), pad_scale_notes[pad], down ? event.value : uint8_t(0)});
}

int next_light_scene(const AppState& state, int step) {
    if (state.page.load() != Page::Lights) return 0;
    int count = int(light_scenes.size()), current = int(light_scene_at(state.light_position.load()));
    return ((current + step) % count + count) % count;
}
