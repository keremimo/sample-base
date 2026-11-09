// SPDX-License-Identifier: AGPL-3.0-only
#include "runtime/run_app.h"
#include "drivers/audio.h"
#include "drivers/display.h"
#include "runtime/control_socket.h"
#include "runtime/controller.h"
#include "runtime/reports.h"
#include "runtime/screen.h"
#include "runtime/session.h"
#include <chrono>
#include <csignal>
#include <iostream>
#include <memory>
#include <mutex>
#include <thread>

namespace {
volatile std::sig_atomic_t interrupted = 0;
void signal_handler(int) { interrupted = 1; }

// AudioDevice's callback, handing each block to the app.
void render(void* app, const AudioBlock& input, AudioBlock& output) { static_cast<App*>(app)->render(input, output); }
} // namespace

int run_app(App& app, const Options& options) {
    signal(SIGINT, signal_handler); signal(SIGTERM, signal_handler);
    std::unique_ptr<Display> display;
    juce::Typeface::Ptr prompt_font;
    if (!options.headless) { prompt_font = load_typeface(); display = std::make_unique<Display>(); }
    AudioDevice audio(options.audio_output, options.audio_input);
    Controller controller(app, options.surface_required);
    ControlServer server(options.runtime);
    Session session;
    app.attach(session);
    auto deadline = options.seconds ? std::chrono::steady_clock::now() + std::chrono::seconds(options.seconds)
                                    : std::chrono::steady_clock::time_point::max();
    std::thread audio_thread, control_thread;
    try {
        audio_thread = std::thread([&] { audio.run(session.running, session.failed, session.audio, render, &app); });
        control_thread = std::thread([&] { controller.run(session, server); });
        std::cout << "READY sample-base JUCE " << JUCE_MAJOR_VERSION << '.' << JUCE_MINOR_VERSION << '.' << JUCE_BUILDNUMBER << std::endl;
        // This thread draws the screen: the remote prompt while one is shown, otherwise the app.
        juce::Image image(juce::Image::ARGB, screen_width, screen_height, true);
        while (session.running.load() && !interrupted && std::chrono::steady_clock::now() < deadline) {
            if (display) {
                {
                    juce::Graphics g(image);
                    std::string prompt;
                    if (session.prompting.load()) { std::lock_guard<std::mutex> lock(session.prompt_lock); prompt = session.prompt; }
                    if (!prompt.empty()) draw_prompt(g, prompt_font, prompt);
                    else app.draw(g);
                }
                display->present(image);
                if (session.screenshot.exchange(false)) save_png(image, options.runtime + "/screen.png");
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(75));
        }
    } catch (...) {
        session.running.store(false);
        if (audio_thread.joinable()) audio_thread.join();
        if (control_thread.joinable()) control_thread.join();
        throw;
    }
    session.running.store(false); audio_thread.join(); control_thread.join();
    std::cout << status_line(session, app) << std::endl;
    return session.failed.load() ? 1 : 0;
}
