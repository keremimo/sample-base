// SPDX-License-Identifier: AGPL-3.0-only
// sample-base entry point: picks the app and runs one mode. The runtime
// (src/runtime/) hosts any App; to run your own, add it to make_app() and
// start it with --app. docs/APPS.md explains how.
#include "runtime/app.h"
#include "runtime/control_socket.h"
#include "runtime/inventory.h"
#include "runtime/options.h"
#include "runtime/run_app.h"
#include "demos/demo_app.h"
#include "demos/tools.h"
#include <sys/stat.h>
#include <exception>
#include <iostream>
#include <memory>
#include <stdexcept>

namespace {
std::unique_ptr<App> make_app(const Options& options) {
    if (options.app == "demos") return std::make_unique<DemoApp>(options.usb_input);
    throw std::runtime_error("Unknown app: " + options.app + "; use demos");
}
} // namespace

int main(int argc, char** argv) {
    umask(0077);
    try {
        if (auto code = run_demo_tool(argc, argv)) return *code;
        auto options = parse_options(argc, argv);
        switch (options.mode) {
        case Options::Mode::Run: { auto app = make_app(options); return run_app(*app, options); }
        case Options::Mode::Control: return send_control_command(options.runtime, options.command);
        case Options::Mode::ControlStream: return stream_control_commands(options.runtime);
        case Options::Mode::Hardware: return print_hardware();
        }
    } catch (const std::exception& error) {
        std::cerr << "sample-base: " << error.what() << '\n';
    }
    return 1;
}
