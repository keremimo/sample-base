// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include "drivers/audio.h"
#include "app/app_state.h"
#include "app/synth.h"

// What the audio callback works on. The meters keep decaying peaks between
// blocks; only the audio thread touches this.
struct RenderContext { AppState& state; Synth& synth; float meter_left = 0, meter_right = 0; };

// AudioDevice callback; `context` is a RenderContext. Plays queued MIDI
// through the synth, feeds the selected input to the recorder and mixes
// synth, take playback and input monitoring into `output`. Never allocates,
// blocks or touches files.
void render_audio(void* context, const AudioBlock& input, AudioBlock& output);
