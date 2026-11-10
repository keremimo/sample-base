// SPDX-License-Identifier: AGPL-3.0-only
#include "runtime/controller.h"
#include "runtime/control_socket.h"
#include "runtime/util.h"
#include "surface/controls.h"
#include "surface/pads.h"
#include <algorithm>
#include <chrono>
#include <iostream>
#include <thread>
#include <utility>

namespace {
using Clock = std::chrono::steady_clock;
} // namespace

Controller::Controller(App& app, bool required, UsbAudioOutput& usb) : app(app), surface_required(required), usb_output(usb) {
    catalog = {midi.json(), midi.outputs(), midi.open_ports()};
    require(!required || midi.has_surface(), "Control surface unavailable; use --no-surface only for a headless/host test");
}

void Controller::run(Session& session, ControlServer& server) {
    std::thread io_thread;
    try {
        io_thread = std::thread([&] { run_midi(session); });
        while (session.running.load()) {
            record_surface_packets();
            MidiCatalog update;
            while (catalog_updates.pop(update)) catalog = std::move(update);
            poll_platform(session);
            server.serve(5, [&](const std::string& request) { return command(session, request); });
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; session.failed.store(true); session.running.store(false);
    }
    session.running.store(false);
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
void Controller::poll_platform(Session& session) {
    clock.poll(); services.poll();
    jacks.poll([&](const JackEvent& event) {
        session.jacks[unsigned(event.jack)].store(event.plugged ? 1 : 0);
        app.jack(event, hardware);
    });
    for (unsigned n = 0; n < jack_count; ++n) session.jacks[n].store(jacks.state(Jack(n))); // -1 once the card is lost.
    app.service(hardware);
    if (power.poll()) session.last_input.store(pack_last_input(InputKind::Power, 0, power.pressed()));
    if (services.stopping() || app.finished()) session.running.store(false);
}

void Controller::run_midi(Session& session) {
    try {
        auto release = Clock::time_point::max(); // When the audition press ends.
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
        auto lights_due = Clock::now(); // the app's lights, every 5 ms
        while (session.running.load()) {
            midi.read([&](const MidiPortInfo& port, const MidiPacket& packet) {
                midi_packet(session, port, packet, Clock::now() < syncing_until);
            });
            auto now = Clock::now();
            if (now >= scan) {
                if (midi.rescan()) app.midi_lost();
                require(!surface_required || midi.has_surface(), "Control surface disconnected");
                MidiCatalog update {midi.json(), midi.outputs(), midi.open_ports()}; catalog_updates.push(update);
                scan = now + std::chrono::seconds(1);
            }
            if (sync_requested.exchange(false)) request_positions();
            // Audition: pad 1 pressed for 300 ms, as if by hand.
            if (audition_requested.exchange(false)) {
                surface_event(session, {0x99, surface_pad_notes[0], 100}, false); ++session.synthetic_events;
                release = now + std::chrono::milliseconds(300);
            }
            if (now >= release) {
                surface_event(session, {0x89, surface_pad_notes[0], 0}, false); ++session.synthetic_events;
                release = Clock::time_point::max();
            }
            MidiSendRequest request;
            for (unsigned n = 0; n < 32 && midi_output.pop(request); ++n)
                if (!midi.send(request.address.data(), request.packet)) ++session.midi_send_errors;
            // The app's timed output: it asks to be woken for its next message.
            output.refresh();
            output.wake = Clock::time_point::max();
            app.midi_output(output, now);
            if (now >= lights_due) {
                app.lights(lights, now);
                lights.update(send_surface, 48);
                lights_due = now + std::chrono::milliseconds(5);
            }
            midi.flush();
            // Sleep until input arrives, the lights are due, the app's next
            // message, the audition's release or the next port scan.
            auto until = std::min({lights_due, output.wake, release, scan});
            midi.wait(std::chrono::duration_cast<std::chrono::nanoseconds>(until - Clock::now()).count());
        }
        // Leave the surface dark for the next app rather than frozen mid-frame.
        lights.release();
        lights.update(send_surface, 64);
        midi.flush();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; session.failed.store(true); session.running.store(false);
    }
}

void Controller::Output::refresh() {
    if (midi.generation() == seen) return;
    seen = midi.generation();
    external.clear();
    for (const auto& port : midi.open_list()) if (!port.surface) external.push_back(port);
}

bool Controller::Output::send(const std::string& address, const MidiPacket& packet) {
    for (const auto& port : external) if (port.address == address) return midi.send(address, packet);
    return false; // never the surface, never a port that is gone
}

// MIDI thread. Surface packets go through surface_event(); external MIDI
// goes to the app untouched.
void Controller::midi_packet(Session& session, const MidiPortInfo& port, const MidiPacket& packet, bool syncing) {
    if (port.surface) {
        surface_events.push(packet); ++session.hardware_events;
        MidiEvent event;
        if (!channel_event(packet, event)) return;
        session.last_midi.store((unsigned(event.status) << 16) | (unsigned(event.key) << 8) | event.value);
        surface_event(session, event, syncing);
        return;
    }
    ++session.external_events;
    if (packet.size == 1 && packet.bytes[0] == 0xf8) ++session.clocks;
    app.midi(port, packet);
}

// MIDI thread. What the runtime does with every surface message before the
// app sees it: wheel Push returns to the stock app (unless push-exit is off),
// the last control touched and the held pads are tracked, and replies to the
// position request are not counted as moves.
void Controller::surface_event(Session& session, MidiEvent midi_event, bool syncing) {
    SurfaceEvent event {midi_event, surface_input(midi_event), pad_index(midi_event), syncing};
    const auto& input = event.input;
    if (is_button(input, SurfaceButton::WheelPush) && input.pressed && session.push_exit.load() && !app.claims_wheel_push()) {
        session.running.store(false); return;
    }
    // The wheel's reply to the position request is a zero step.
    bool moved = input.kind == SurfaceInput::Kind::Control &&
        (is_control(input, SurfaceControl::Wheel) ? input.delta != 0 : !syncing);
    if (moved || input.pressed) session.last_input.store(pack_last_input(input));
    unsigned kind = midi_event.status & 0xf0;
    if (event.pad >= 0 && kind != 0xa0) {
        if (kind == 0x90 && midi_event.value != 0) {
            session.held.fetch_or(1u << event.pad); session.last_pad.store(event.pad); session.velocity.store(midi_event.value);
        } else {
            session.held.fetch_and(~(1u << event.pad));
        }
    }
    app.surface(event);
}
