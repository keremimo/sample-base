// SPDX-License-Identifier: AGPL-3.0-only
#include "app/controller.h"
#include "surface/controls.h"
#include "surface/pads.h"
#include "app/control_socket.h"
#include "app/surface_events.h"
#include "app/util.h"
#include <chrono>
#include <iostream>
#include <thread>
#include <utility>

namespace {
using Clock = std::chrono::steady_clock;

// The channel message in `packet`; false for SysEx, clock and other system messages.
bool channel_message(const MidiPacket& packet, MidiEvent& event) {
    if (packet.size < 2 || packet.bytes[0] >= 0xf0) return false;
    event = {packet.bytes[0], packet.bytes[1], packet.size == 3 ? packet.bytes[2] : uint8_t(0)};
    return true;
}
} // namespace

Controller::Controller(bool required, const std::string& usb) : surface_required(required), usb_device(usb) {
    catalog = {midi.json(), midi.outputs()};
    require(!required || midi.has_surface(), "Control surface unavailable; use --no-surface only for a headless/host test");
}

void Controller::run(AppState& state, ControlServer& server) {
    std::thread io_thread;
    try {
        io_thread = std::thread([&] { run_midi(state); });
        while (state.running.load()) {
            record_surface_packets();
            MidiCatalog update;
            while (catalog_updates.pop(update)) catalog = std::move(update);
            poll_platform(state);
            server.serve(5, [&](const std::string& request) { return command(state, request); });
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; state.failed.store(true); state.running.store(false);
    }
    state.running.store(false);
    if (io_thread.joinable()) io_thread.join();
}

// Service thread. Keeps the surface state and packet log for the `surface`
// and `surface-log` commands. After a queue overflow the state restarts empty.
void Controller::record_surface_packets() {
    MidiPacket packet;
    if (surface_events.take_overflow()) {
        for (unsigned n = 0; n < 256 && surface_events.pop(packet); ++n) {}
        surface = {}; ++surface_overflows;
    }
    for (unsigned n = 0; n < 256 && surface_events.pop(packet); ++n) { surface.apply(packet); surface_log.push(packet); }
}

// Service thread. Platform requests that may block too long for the MIDI thread.
void Controller::poll_platform(AppState& state) {
    clock.poll(); services.poll();
    int requested = state.source_request.exchange(-1);
    if (requested >= 0) select_source(state, InputSource(requested));
    if (power.poll()) state.last_input.store(pack_last_input(InputKind::Power, 0, power.pressed()));
    if (services.stopping()) state.running.store(false);
}

void Controller::run_midi(AppState& state) {
    try {
        auto release = Clock::time_point::max(); // When the audition note ends.
        auto scan = Clock::now();                // Next MIDI port scan.
        // Absolute controls only report when moved, so ask for their
        // current positions. The reply is applied like any other CC but
        // is not shown as the last control the player touched.
        auto syncing_until = Clock::time_point::min();
        auto request_positions = [&] {
            if (midi.send_surface(surface_position_packet()))
                syncing_until = Clock::now() + std::chrono::milliseconds(300);
        };
        auto send_surface = [&](const MidiPacket& packet) { return midi.send_surface(packet); };
        request_positions();
        while (state.running.load()) {
            midi.read([&](const MidiPortInfo& port, const MidiPacket& packet) {
                midi_packet(state, port, packet, Clock::now() < syncing_until);
            });
            auto now = Clock::now();
            if (now >= scan) {
                if (midi.rescan()) state.queue.push({0xb0,120,0}); // An input vanished: silence its notes.
                require(!surface_required || midi.has_surface(), "Control surface disconnected");
                MidiCatalog update {midi.json(), midi.outputs()}; catalog_updates.push(update);
                scan = now + std::chrono::seconds(1);
            }
            if (sync_requested.exchange(false)) request_positions();
            if (audition_requested.exchange(false)) {
                play_pad(state, {0x99,surface_pad_notes[0],100}); ++state.synthetic_events;
                release = now + std::chrono::milliseconds(300);
            }
            if (now >= release) { play_pad(state, {0x89,surface_pad_notes[0],0}); ++state.synthetic_events; release = Clock::time_point::max(); }
            MidiSendRequest request;
            for (unsigned n = 0; n < 32 && midi_output.pop(request); ++n)
                if (!midi.send(request.address.data(), request.packet)) ++state.midi_send_errors;
            light_driver.update(state, now);
            lights.update(send_surface, 48);
            midi.flush();
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        // Leave a dark surface rather than a frozen show frame.
        if (light_driver.lights_in_use()) {
            lights.all_off();
            lights.update(send_surface, 64);
            midi.flush();
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; state.failed.store(true); state.running.store(false);
    }
}

// MIDI thread. Surface packets drive the app; external MIDI plays the synth directly.
void Controller::midi_packet(AppState& state, const MidiPortInfo& port, const MidiPacket& packet, bool syncing) {
    MidiEvent event;
    if (port.surface) {
        surface_events.push(packet); ++state.hardware_events;
        if (!channel_message(packet, event)) return;
        state.last_midi.store((unsigned(event.status) << 16) | (unsigned(event.key) << 8) | event.value);
        handle_surface_event(state, event, syncing);
        return;
    }
    ++state.external_events;
    if (packet.size == 1 && packet.bytes[0] == 0xf8) ++state.clocks;
    if (channel_message(packet, event)) state.queue.push(event);
}

// Service thread only. Recording stops before any route changes, and the
// new source is published only once its route is in place. Without the
// I2S mixer (a host test or another --audio-input) routes are left alone.
bool Controller::select_source(AppState& state, InputSource source) {
    state.source.store(uint8_t(InputSource::Off));
    if (source != InputSource::Usb) state.usb.stop();
    if (mixer.available()) {
        const char* profile = source == InputSource::Line ? "line" : source == InputSource::Mic ? "mic" : "off";
        if (!mixer.input(profile)) { mixer.input("off"); return false; }
    }
    if (source == InputSource::Usb && state.usb.state() == UsbInputState::Stopped) state.usb.start(usb_device);
    state.source.store(uint8_t(source));
    return true;
}
