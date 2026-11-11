// SPDX-License-Identifier: AGPL-3.0-only
#include "runtime/controller.h"
#include "drivers/usb_gadget.h"
#include "runtime/control_socket.h"
#include "runtime/util.h"
#include "surface/controls.h"
#include "surface/pads.h"
#include <algorithm>
#include <chrono>
#include <iostream>
#include <pthread.h>
#include <sched.h>
#include <thread>
#include <utility>

namespace {
using Clock = std::chrono::steady_clock;
uint64_t monotonic_ns() { return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count()); }
} // namespace

Controller::Controller(App& app, bool required, UsbAudioOutput& usb, bool usb_rate)
    : app(app), surface_required(required), usb_output(usb), usb_rate_follows(usb_rate) {
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
    card.shutdown();
    follow_card(session);
}

// Service thread: the card share's state for the other threads.
void Controller::follow_card(Session& session) {
    const auto& status = card.status();
    session.card_share.store(uint8_t(status.state));
    session.card_connected.store(status.connected);
    session.card_ejected.store(status.ejected);
    session.card_returns.store(status.returns);
}

// Service thread: the USB-C audio gadget offers the computer the rate the
// audio device runs at, so a DAW records and plays at it. Rewriting the rate
// rebinds the gadget (USB audio, MIDI and SSH drop for about a second), so
// never while the card is lent or on its way, and once a rate: a failed
// change keeps the old rate, and USB-C audio converts as before.
void Controller::follow_usb_rate(Session& session) {
    unsigned rate = session.audio.rate.load();
    if (!usb_rate_follows || rate == usb_rate_tried || card.status().state != CardShareState::Home) return;
    usb_rate_tried = rate;
    auto change = set_uac2_rate(rate);
    if (change.result == GadgetChange::Result::Changed || change.result == GadgetChange::Result::Failed)
        std::cout << "USB-C audio: " << describe(change) << std::endl;
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
    card.poll();
    follow_card(session);
    follow_usb_rate(session);
    app.service(hardware);
    follow_card(session); // What the app asked of it shows at once
    if (power.poll()) session.last_input.store(pack_last_input(InputKind::Power, 0, power.pressed()));
    if (services.stopping() || app.finished()) session.running.store(false);
}

void Controller::run_midi(Session& session) {
    // A low real-time priority, under the audio thread's 30 and the 20 of the
    // USB audio thread (and of an app's disk streaming): a pad or a MIDI
    // message is read the moment it arrives, not when the scheduler gets round
    // to it. The loop sleeps in poll between passes, so it starves nothing.
    // Best effort, as the audio thread's.
    sched_param priority {}; priority.sched_priority = 15;
    if (int error = pthread_setschedparam(pthread_self(), SCHED_FIFO, &priority)) std::cerr << "MIDI priority unavailable: " << error << '\n';
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
                surface_event(session, {0x99, surface_pad_notes[0], 100}, false, monotonic_ns()); ++session.synthetic_events;
                release = now + std::chrono::milliseconds(300);
            }
            if (now >= release) {
                surface_event(session, {0x89, surface_pad_notes[0], 0}, false, monotonic_ns()); ++session.synthetic_events;
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
        surface_event(session, event, syncing, packet.timestamp_ns);
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
void Controller::surface_event(Session& session, MidiEvent midi_event, bool syncing, uint64_t timestamp_ns) {
    SurfaceEvent event {midi_event, surface_input(midi_event), pad_index(midi_event), syncing, timestamp_ns};
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
