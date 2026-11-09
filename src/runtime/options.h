// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <string>

// The runtime's command line. Usage:
//   sample-base [--app NAME] [--headless] [--no-surface] [--audio-output PCM]
//               [--audio-input PCM|off] [--usb-input PCM|auto]
//               [--usb-output PCM|auto|off]
//               [--runtime-dir DIR] [--seconds N]
//   sample-base [--runtime-dir DIR] --ctl COMMAND...   send one control command
//   sample-base [--runtime-dir DIR] --ctl-stream       commands from stdin
//   sample-base --hardware
// --ctl takes the rest of the line as its command; --ctl-stream ignores
// anything after it. Options before either still apply. Apps may add their
// own one-shot tools ahead of these; main.cpp runs the demos' first.
struct Options {
    enum class Mode { Run, Hardware, Control, ControlStream };
    Mode mode = Mode::Run;

    // Run
    std::string app = "demos"; // Which app main.cpp starts.
    std::string audio_output = "hw:I2S,0", audio_input = "hw:I2S,0", usb_input = "auto";
    std::string usb_output = "auto"; // What the computer records over USB-C; "off" opens nothing.
    std::string runtime = "/run/sample-base"; // Also where --ctl finds the app.
    bool headless = false, surface_required = true;
    unsigned seconds = 0; // Return to stock after this long; 0 runs until stopped.

    std::string command; // --ctl
};

// Throws std::runtime_error for unknown options and invalid values.
Options parse_options(int argc, char** argv);
