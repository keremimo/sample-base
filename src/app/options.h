// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <string>

// The command line. Usage:
//   sample-base [--headless] [--no-surface] [--audio-output PCM] [--audio-input PCM|off]
//               [--usb-input PCM|auto] [--runtime-dir DIR] [--seconds N]
//   sample-base [--runtime-dir DIR] --ctl COMMAND...   send one control command
//   sample-base [--runtime-dir DIR] --ctl-stream       commands from stdin
//   sample-base --self-test FILE.wav
//   sample-base --preview FILE.png [PROMPT]
//   sample-base --preview-recorder FILE.png
//   sample-base --preview-lights FILE.png [MS]
//   sample-base --hardware
// --ctl takes the rest of the line as its command; --ctl-stream ignores
// anything after it. Options before either still apply.
struct Options {
    enum class Mode { Run, SelfTest, Preview, PreviewRecorder, PreviewLights, Hardware, Control, ControlStream };
    Mode mode = Mode::Run;

    // Run
    std::string audio_output = "hw:I2S,0", audio_input = "hw:I2S,0", usb_input = "auto";
    std::string runtime = "/run/sample-base"; // Also where --ctl finds the app.
    bool headless = false, surface_required = true;
    unsigned seconds = 0; // Return to stock after this long; 0 runs until stopped.

    // Offline modes and --ctl
    std::string file;            // WAV or PNG to write.
    std::string prompt;          // --preview
    unsigned light_position = 0; // --preview-lights, in ms
    std::string command;         // --ctl
};

// Throws std::runtime_error for unknown options and invalid values.
Options parse_options(int argc, char** argv);
