// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <string>

// One-shot modes for build checks and hardware discovery. Each writes or
// prints its result and returns the exit code; none opens the MPC's audio
// output, display or control surface.

// --self-test: renders a note and its release into a 16-bit WAV and checks
// that it sounds and then falls silent.
int self_test(const std::string& wav_path);
// --preview, --preview-recorder, --preview-lights: draws one page as a PNG.
// `prompt` is an optional remote prompt; `position_ms` is the light show time.
int write_synth_preview(const std::string& png_path, const std::string& prompt);
int write_recorder_preview(const std::string& png_path);
int write_lights_preview(const std::string& png_path, unsigned position_ms);
// --hardware: prints platform, audio, mixer, storage and MIDI ports as JSON.
int print_hardware();
