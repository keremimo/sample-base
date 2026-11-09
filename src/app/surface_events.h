// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include "surface/midi.h"
#include "app/app_state.h"

// How the app answers the control surface. MIDI thread only.

// One channel message from the surface: wheel Push returns to the stock app
// (unless push-exit is off), main volume sets the output gain, buttons switch
// pages and drive the recorder, and pads play the synth and report their
// velocity and pressure. `syncing` is true
// while the surface answers the startup position request, so those replies
// are not shown as the last control touched.
void handle_surface_event(AppState& state, MidiEvent event, bool syncing);

// A pad message on channel 10: records the strike, pressure or release in
// state.pads, tracks held pads and queues the pad's scale note.
void play_pad(AppState& state, MidiEvent event);

// The light show scene `step` scenes after the one playing, or the first
// scene when the show is closed.
int next_light_scene(const AppState& state, int step);
