// SPDX-License-Identifier: AGPL-3.0-only
#include "app/application.h"
#include "audio.h"
#include "display.h"
#include "app/app_state.h"
#include "app/audio_render.h"
#include "app/control_socket.h"
#include "app/controller.h"
#include "app/reports.h"
#include "app/screen.h"
#include "app/screen_feed.h"
#include "app/synth.h"
#include <chrono>
#include <csignal>
#include <iostream>
#include <memory>
#include <thread>

namespace {
volatile std::sig_atomic_t interrupted = 0;
void signal_handler(int) { interrupted = 1; }
} // namespace

int run_application(const Options& options) {
    signal(SIGINT, signal_handler); signal(SIGTERM, signal_handler);
    Synth engine;
    std::unique_ptr<Screen> screen; std::unique_ptr<Display> display;
    if (!options.headless) { screen = std::make_unique<Screen>(default_font_path()); display = std::make_unique<Display>(); }
    AudioDevice audio(options.audio_output, options.audio_input);
    Controller controller(options.surface_required, options.usb_input);
    ControlServer server(options.runtime);
    AppState state; RenderContext context {state, engine};
    auto deadline = options.seconds ? std::chrono::steady_clock::now() + std::chrono::seconds(options.seconds)
                                    : std::chrono::steady_clock::time_point::max();
    std::thread audio_thread, control_thread;
    try {
        audio_thread = std::thread([&] { audio.run(state.running, state.failed, state.audio, render_audio, &context); });
        control_thread = std::thread([&] { controller.run(state, server); });
        std::cout << "READY sample-base JUCE " << JUCE_MAJOR_VERSION << '.' << JUCE_MINOR_VERSION << '.' << JUCE_BUILDNUMBER << std::endl;
        // This thread draws the screen.
        ScreenFeed feed;
        while (state.running.load() && !interrupted && std::chrono::steady_clock::now() < deadline) {
            if (screen && display) {
                auto image = screen->draw(feed.frame(state));
                display->present(image);
                if (state.screenshot.exchange(false)) save_png(image, options.runtime + "/screen.png");
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(75));
        }
    } catch (...) {
        state.running.store(false);
        if (audio_thread.joinable()) audio_thread.join();
        if (control_thread.joinable()) control_thread.join();
        throw;
    }
    state.running.store(false); audio_thread.join(); control_thread.join();
    std::cout << status_line(state) << std::endl;
    return state.failed.load() ? 1 : 0;
}
