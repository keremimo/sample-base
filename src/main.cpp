// SPDX-License-Identifier: AGPL-3.0-only
// sample-base entry point: reads the command line and runs one mode. The
// app lives in src/app/ and the reusable MPC interfaces in src/.
#include "app/application.h"
#include "app/control_socket.h"
#include "app/offline.h"
#include "app/options.h"
#include <sys/stat.h>
#include <exception>
#include <iostream>

int main(int argc, char** argv) {
    umask(0077);
    try {
        auto options = parse_options(argc, argv);
        switch (options.mode) {
        case Options::Mode::Run: return run_application(options);
        case Options::Mode::Control: return send_control_command(options.runtime, options.command);
        case Options::Mode::ControlStream: return stream_control_commands(options.runtime);
        case Options::Mode::SelfTest: return self_test(options.file);
        case Options::Mode::Preview: return write_synth_preview(options.file, options.prompt);
        case Options::Mode::PreviewRecorder: return write_recorder_preview(options.file);
        case Options::Mode::PreviewLights: return write_lights_preview(options.file, options.light_position);
        case Options::Mode::Hardware: return print_hardware();
        }
    } catch (const std::exception& error) {
        std::cerr << "sample-base: " << error.what() << '\n';
    }
    return 1;
}
