// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include "runtime/app.h"
#include "surface/midi.h"
#include "demos/demo_state.h"
#include "demos/synth/synth.h"

// Beep/boop synth: every pad plays its note of C major (pad_scale.h) on a
// 16-voice sine/triangle synth (synth.h) on every page, and external MIDI
// plays it at its own pitches. Main volume sets the output level.

// MIDI thread: a pad's note-on or note-off, queued as the pad's scale note.
// Pressure is not played.
void queue_pad_note(DemoState& state, const SurfaceEvent& event);
// Audio thread: plays every queued note. After a queue overflow the backlog
// is dropped and all sound stops, rather than playing stale notes late.
void play_queued_midi(DemoState& state, Synth& synth);
