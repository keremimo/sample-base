// SPDX-License-Identifier: AGPL-3.0-only
#include "runtime/options.h"
#include "runtime/util.h"

Options parse_options(int argc, char** argv, const std::string& default_app) {
    Options options;
    options.app = default_app;
    using Mode = Options::Mode;
    for (int i = 1; i < argc; ++i) {
        std::string option = argv[i];
        if (option == "--headless") options.headless = true;
        else if (option == "--no-surface") options.surface_required = false;
        else if (option == "--hardware" && argc == 2) { options.mode = Mode::Hardware; return options; }
        else if (option == "--ctl-stream") { options.mode = Mode::ControlStream; return options; }
        else if (option == "--ctl") {
            require(i + 1 < argc, "Control command is missing");
            for (++i; i < argc; ++i) { if (!options.command.empty()) options.command += ' '; options.command += argv[i]; }
            options.mode = Mode::Control; return options;
        } else {
            require(i + 1 < argc, "Missing option value");
            std::string value = argv[++i];
            if (option == "--app") options.app = value;
            else if (option == "--audio-output") options.audio_output = value;
            else if (option == "--audio-input") options.audio_input = value;
            else if (option == "--usb-input") options.usb_input = value;
            else if (option == "--usb-output") options.usb_output = value;
            else if (option == "--usb-stems") {
                require(value == "on" || value == "off", "--usb-stems takes on or off");
                options.usb_stems = value == "on";
            }
            else if (option == "--runtime-dir") options.runtime = value;
            else if (option == "--seconds") require(parse_unsigned(value, options.seconds) && options.seconds <= 86400, "Invalid session duration");
            else throw std::runtime_error("Unknown option: " + option);
        }
    }
    return options;
}
