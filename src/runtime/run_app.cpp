// SPDX-License-Identifier: AGPL-3.0-only
#include "runtime/run_app.h"
#include "drivers/audio.h"
#include "drivers/display.h"
#include "drivers/usb_audio.h"
#include "drivers/usb_gadget.h"
#include "runtime/control_socket.h"
#include "runtime/controller.h"
#include "runtime/reports.h"
#include "runtime/screen.h"
#include "runtime/session.h"
#include <algorithm>
#include <chrono>
#include <csignal>
#include <iostream>
#include <memory>
#include <mutex>
#include <thread>

namespace {
volatile std::sig_atomic_t interrupted = 0;
void signal_handler(int) { interrupted = 1; }

// AudioDevice's callback: the app renders each block, and the computer
// records what it rendered, and its stems, when it is recording over USB-C.
struct AudioRoute { App& app; UsbAudioOutput& usb; };
// The screen's counters for the status line.
void record_screen(ScreenStats& screen, const Display::Stats& stats, std::chrono::steady_clock::duration draw) {
    screen.presented.store(stats.presented, std::memory_order_relaxed);
    screen.pushed.store(stats.pushed, std::memory_order_relaxed);
    screen.pixels.store(stats.pixels, std::memory_order_relaxed);
    screen.last_push_us.store(stats.last_push_us, std::memory_order_relaxed);
    screen.max_push_us.store(stats.max_push_us, std::memory_order_relaxed);
    screen.full_push_us.store(stats.full_push_us, std::memory_order_relaxed);
    screen.draw_us.store(uint32_t(std::chrono::duration_cast<std::chrono::microseconds>(draw).count()), std::memory_order_relaxed);
}

void render(void* context, const AudioBlock& input, AudioBlock& output) {
    auto& route = *static_cast<AudioRoute*>(context);
    route.app.render(input, output);
    route.usb.push(output.data(), audio_frames, route.app.stems());
}

// The app's format, or the default when the device refuses it. Throws only
// when the default fails too.
std::unique_ptr<AudioDevice> open_audio(const Options& options, AudioFormat wanted) {
    if (wanted != AudioFormat {}) {
        try { return std::make_unique<AudioDevice>(options.audio_output, options.audio_input, wanted); }
        catch (const std::exception& error) { std::cerr << "Audio format refused, using 44100 Hz: " << error.what() << '\n'; }
    }
    return std::make_unique<AudioDevice>(options.audio_output, options.audio_input);
}
} // namespace

int run_app(App& app, const Options& options) {
    signal(SIGINT, signal_handler); signal(SIGTERM, signal_handler);
    std::unique_ptr<Display> display;
    juce::Typeface::Ptr prompt_font;
    if (!options.headless) { prompt_font = load_typeface(); display = std::make_unique<Display>(); }
    // Before anything opens audio or MIDI: the gadget's rebind replaces its cards.
    if (options.usb_output == "auto" && options.usb_stems)
        std::cout << "USB-C audio: " << describe(widen_uac2_playback(UsbAudioOutput::channels)) << std::endl;
    UsbAudioOutput usb_output; // Outlives the audio thread, which feeds it.
    AudioFormat requested = app.audio_format(); // Screen thread: what the app last asked for
    auto audio = open_audio(options, requested);
    Controller controller(app, options.surface_required, usb_output);
    ControlServer server(options.runtime);
    Session session;
    app.attach(session);
    AudioRoute route {app, usb_output};
    if (options.usb_output != "off") usb_output.start(options.usb_output);
    auto deadline = options.seconds ? std::chrono::steady_clock::now() + std::chrono::seconds(options.seconds)
                                    : std::chrono::steady_clock::time_point::max();
    std::thread audio_thread, control_thread;
    try {
        // The audio thread runs one device at a time; a format change closes it and opens the next.
        audio_thread = std::thread([&] {
            while (true) {
                usb_output.set_device_rate(audio->format().rate);
                app.audio_opened(audio->format());
                audio->run(session.running, session.failed, session.audio, render, &route, &session.audio_reopen);
                if (!session.running.load()) break;
                audio.reset();
                try { audio = open_audio(options, app.audio_format()); }
                catch (const std::exception& error) {
                    std::cerr << error.what() << '\n';
                    session.failed.store(true); session.running.store(false); break;
                }
                session.audio.reopens.fetch_add(1);
                session.audio_reopen.store(false);
            }
        });
        control_thread = std::thread([&] { controller.run(session, server); });
        std::cout << "READY sample-base JUCE " << JUCE_MAJOR_VERSION << '.' << JUCE_MINOR_VERSION << '.' << JUCE_BUILDNUMBER << std::endl;
        // This thread draws the screen: the remote prompt while one is shown,
        // otherwise the app. present() waits while the panel still receives
        // the last frame, so the panel's SPI push sets the rate, and frames
        // never come closer than busy_interval (about the panel's own scan).
        // Drawing costs CPU even when nothing moves, so once the screen has
        // stood still for idle_after the loop draws every idle_interval, and
        // any control or remote input wakes it at once.
        using ScreenClock = std::chrono::steady_clock;
        constexpr std::chrono::microseconds busy_interval {16667}, idle_interval {100000}, idle_after {500000}, input_poll {4000};
        auto inputs = [&] { return session.hardware_events.load() + session.external_events.load() + session.synthetic_events.load(); };
        juce::Image image(juce::Image::ARGB, screen_width, screen_height, true);
        auto next = ScreenClock::now(), last_change = next;
        while (session.running.load() && !interrupted && ScreenClock::now() < deadline) {
            auto start = ScreenClock::now();
            if (auto wanted = app.audio_format(); wanted != requested) { requested = wanted; session.audio_reopen.store(true); }
            if (display) {
                {
                    juce::Graphics g(image);
                    std::string prompt;
                    if (session.prompting.load()) { std::lock_guard<std::mutex> lock(session.prompt_lock); prompt = session.prompt; }
                    if (!prompt.empty()) draw_prompt(g, prompt_font, prompt);
                    else app.draw(g);
                }
                auto drawn = ScreenClock::now();
                if (display->present(image)) last_change = drawn;
                record_screen(session.screen, display->stats(), drawn - start);
                if (session.screenshot.exchange(false)) save_png(image, options.runtime + "/screen.png");
            }
            bool idle = !display || start - last_change > idle_after;
            next = std::max(next + (idle ? idle_interval : busy_interval), ScreenClock::now() - busy_interval);
            if (!idle) { std::this_thread::sleep_until(next); continue; }
            auto seen = inputs();
            while (ScreenClock::now() < next && inputs() == seen && session.running.load() && !interrupted && !session.screenshot.load())
                std::this_thread::sleep_for(input_poll);
            if (inputs() != seen) { last_change = ScreenClock::now(); next = last_change; }
        }
    } catch (...) {
        session.running.store(false);
        if (audio_thread.joinable()) audio_thread.join();
        if (control_thread.joinable()) control_thread.join();
        throw;
    }
    session.running.store(false); audio_thread.join(); control_thread.join();
    std::cout << status_line(session, app, usb_output) << std::endl;
    return session.failed.load() ? 1 : 0;
}
