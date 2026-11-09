// SPDX-License-Identifier: AGPL-3.0-only
#include "demos/synth/synth_demo.h"
#include "demos/synth/pad_scale.h"

void queue_pad_note(DemoState& state, const SurfaceEvent& event) {
    unsigned kind = event.midi.status & 0xf0;
    if (event.pad < 0 || kind == 0xa0) return;
    bool down = kind == 0x90 && event.midi.value != 0;
    state.queue.push({uint8_t(down ? 0x90 : 0x80), pad_scale_notes[size_t(event.pad)], down ? event.midi.value : uint8_t(0)});
}

void play_queued_midi(DemoState& state, Synth& synth) {
    MidiEvent event;
    if (state.queue.take_overflow()) {
        for (unsigned n = 0; n < 256 && state.queue.pop(event); ++n) {}
        synth.midi({0xb0,120,0}); ++state.overruns;
    }
    for (unsigned n = 0; n < 256 && state.queue.pop(event); ++n) synth.midi(event);
}
