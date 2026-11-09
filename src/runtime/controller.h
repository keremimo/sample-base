// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include "drivers/clock_io.h"
#include "drivers/hardware_midi.h"
#include "drivers/mixer.h"
#include "drivers/platform.h"
#include "drivers/power_button.h"
#include "drivers/system_services.h"
#include "runtime/app.h"
#include "runtime/session.h"
#include "surface/leds.h"
#include "surface/midi.h"
#include <array>
#include <atomic>
#include <string>
#include <vector>

class ControlServer;

// Owns the MPC hardware and runs two threads over it for one App:
//   run()       service thread: control commands, App::service() and the
//               slow platform requests (sync clock, storage service, power key)
//   run_midi()  MIDI thread, started by run(): surface and external MIDI,
//               App::surface()/midi()/lights() and MIDI output
// MIDI polling never waits on the slow requests. The threads hand work to
// each other only through Session and the bounded queues below.
class Controller {
public:
    // Throws when `surface_required` and no control surface is connected.
    Controller(App& app, bool surface_required);
    // Service thread. Returns once session.running is cleared and the MIDI
    // thread has stopped; a failure sets session.failed.
    void run(Session& session, ControlServer& server);
private:
    struct MidiSendRequest { std::array<char, 64> address {}; MidiPacket packet; };
    struct MidiCatalog { std::string json; std::vector<std::string> outputs; };

    // controller.cpp
    void run_midi(Session& session);
    void midi_packet(Session& session, const MidiPortInfo& port, const MidiPacket& packet, bool syncing);
    void surface_event(Session& session, MidiEvent midi, bool syncing);
    void record_surface_packets();
    void poll_platform(Session& session);

    // commands.cpp: the control protocol, run on the service thread.
    std::string command(Session& session, const std::string& text);
    std::string led(const std::string& text);
    std::string midi_send_command(const std::string& argument);
    std::string mixer_set_command(const std::string& argument);

    App& app;
    bool surface_required;

    // Hardware, opened in this order.
    HardwareMidi midi;
    HardwareMixer mixer;
    Platform platform;
    SystemServices services;
    ClockIo clock;
    PowerButton power;
    SurfaceLights lights;
    Hardware hardware {mixer, platform, services, clock}; // What App::service() and App::command() may use.

    // Surface diagnostics; the MIDI thread queues packets, the service thread keeps them.
    MidiQueue<256, MidiPacket> surface_events;
    SurfaceState surface;
    SurfaceLog<1024> surface_log;
    unsigned surface_overflows = 0;

    // MIDI ports as last scanned by the MIDI thread, copied for the service thread.
    MidiCatalog catalog;
    MidiQueue<4, MidiCatalog> catalog_updates;

    // Service thread asks, MIDI thread acts.
    MidiQueue<32, MidiSendRequest> midi_output;
    std::atomic<bool> audition_requested {false}, sync_requested {false};
};
