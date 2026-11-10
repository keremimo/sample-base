// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include "drivers/card_share.h"
#include "drivers/clock_io.h"
#include "drivers/hardware_midi.h"
#include "drivers/jacks.h"
#include "drivers/mixer.h"
#include "drivers/platform.h"
#include "drivers/power_button.h"
#include "drivers/system_services.h"
#include "drivers/usb_audio.h"
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
//               slow platform requests (sync clock, storage service, power
//               key, jack detection)
//   run_midi()  MIDI thread, started by run(): surface and external MIDI,
//               App::surface()/midi()/lights() and MIDI output
// MIDI polling never waits on the slow requests. The threads hand work to
// each other only through Session and the bounded queues below.
class Controller {
public:
    // Throws when `surface_required` and no control surface is connected.
    // `usb_output` belongs to run_app(), which feeds it from the audio thread;
    // the controller only starts, stops and reports it.
    // `usb_rate`: the USB-C audio gadget follows the audio device's rate (set_uac2_rate()).
    Controller(App& app, bool surface_required, UsbAudioOutput& usb_output, bool usb_rate = false);
    // Service thread. Returns once session.running is cleared and the MIDI
    // thread has stopped; a failure sets session.failed.
    void run(Session& session, ControlServer& server);
private:
    struct MidiSendRequest { std::array<char, 64> address {}; MidiPacket packet; };
    struct MidiCatalog { std::string json; std::vector<std::string> outputs; MidiPorts ports; };

    // controller.cpp
    void run_midi(Session& session);
    void midi_packet(Session& session, const MidiPortInfo& port, const MidiPacket& packet, bool syncing);
    void surface_event(Session& session, MidiEvent midi, bool syncing);
    void record_surface_packets();
    void poll_platform(Session& session);
    void follow_card(Session& session);
    void follow_usb_rate(Session& session);

    // commands.cpp: the control protocol, run on the service thread.
    std::string command(Session& session, const std::string& text);
    std::string led(const std::string& text);
    std::string midi_send_command(const std::string& argument);
    std::string mixer_set_command(const std::string& argument);
    std::string usb_output_command(const std::string& argument);
    std::string card_share_command(Session& session, const std::string& argument);

    // App::midi_output()'s view of the MIDI thread's ports and the sync line.
    class Output : public MidiOutput {
    public:
        Output(HardwareMidi& midi, ClockIo& clock) : midi(midi), clock(clock) {}
        const std::vector<MidiPortInfo>& ports() const override { return external; }
        bool send(const std::string& address, const MidiPacket& packet) override;
        bool sync(bool high) override { return clock.output(high); }
        void wake_at(Clock::time_point at) override { if (at < wake) wake = at; }
        void refresh(); // each pass: the external ports open now, when they changed
        Clock::time_point wake = Clock::time_point::max();
    private:
        HardwareMidi& midi;
        ClockIo& clock;
        std::vector<MidiPortInfo> external;
        unsigned seen = ~0u; // HardwareMidi::generation() last copied
    };

    App& app;
    bool surface_required;
    UsbAudioOutput& usb_output;

    // Hardware, opened in this order.
    HardwareMidi midi;
    HardwareMixer mixer;
    JackDetect jacks;
    Platform platform;
    SystemServices services;
    CardShare card {services}; // Takes back a card an earlier run left lent
    ClockIo clock;
    PowerButton power;
    SurfaceLights lights;
    Output output {midi, clock}; // MIDI thread: App::midi_output()

    // Surface diagnostics; the MIDI thread queues packets, the service thread keeps them.
    MidiQueue<256, MidiPacket> surface_events;
    SurfaceState surface;
    SurfaceLog<1024> surface_log;
    unsigned surface_overflows = 0;

    // MIDI ports as last scanned by the MIDI thread, copied for the service thread.
    MidiCatalog catalog;
    MidiQueue<4, MidiCatalog> catalog_updates;

    // What App::service() and App::command() may use.
    Hardware hardware {mixer, platform, services, clock, card, power, jacks, usb_output, catalog.ports};

    // The audio device's rate the USB-C gadget was last set to (or found at); 0 never, or not to follow.
    bool usb_rate_follows = false;
    unsigned usb_rate_tried = 0;

    // Service thread asks, MIDI thread acts.
    MidiQueue<32, MidiSendRequest> midi_output;
    std::atomic<bool> audition_requested {false}, sync_requested {false};
};
