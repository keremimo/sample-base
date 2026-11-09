// SPDX-License-Identifier: AGPL-3.0-only
#include "app/options.h"
#include "app/light_show.h"
#include "app/util.h"
#include <cstring>

Options parse_options(int argc, char** argv) {
    Options options;
    using Mode = Options::Mode;
    // Offline modes take exactly their own arguments.
    auto offline = [&](const char* flag, int min_args, int max_args) {
        return argc - 1 >= min_args && argc - 1 <= max_args && !strcmp(argv[1], flag);
    };
    if (offline("--self-test", 2, 2)) { options.mode = Mode::SelfTest; options.file = argv[2]; return options; }
    if (offline("--preview", 2, 3)) {
        options.mode = Mode::Preview; options.file = argv[2];
        if (argc == 4) options.prompt = argv[3];
        return options;
    }
    if (offline("--preview-recorder", 2, 2)) { options.mode = Mode::PreviewRecorder; options.file = argv[2]; return options; }
    if (offline("--preview-lights", 2, 3)) {
        // By default, mid-way through the rainbow.
        options.mode = Mode::PreviewLights; options.file = argv[2]; options.light_position = light_scene_start(3) + 1800;
        require(argc == 3 || parse_unsigned(std::string(argv[3]), options.light_position), "Invalid light show position");
        return options;
    }

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
            if (option == "--audio-output") options.audio_output = value;
            else if (option == "--audio-input") options.audio_input = value;
            else if (option == "--usb-input") options.usb_input = value;
            else if (option == "--runtime-dir") options.runtime = value;
            else if (option == "--seconds") require(parse_unsigned(value, options.seconds) && options.seconds <= 86400, "Invalid session duration");
            else throw std::runtime_error("Unknown option: " + option);
        }
    }
    return options;
}
