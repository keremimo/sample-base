// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include "drivers/clock_io.h"
#include "drivers/hardware_midi.h"
#include "surface/leds.h"
#include "surface/midi.h"
#include "drivers/mixer.h"
#include "drivers/platform.h"
#include "drivers/power_button.h"
#include "dsp/recorder.h"
#include "drivers/system_services.h"
#include "app/app_state.h"
#include "app/light_driver.h"
#include <array>
#include <atomic>
#include <string>
#include <vector>

class ControlServer;

// Owns the MPC hardware the app uses and runs two threads over it:
//   run()       service thread: control commands and the slow platform
//               requests (sync clock, storage service, power key, input routes)
//   run_midi()  MIDI thread, started by run(): surface and external MIDI,
//               synth notes, MIDI output and the surface lights
// MIDI polling never waits on the slow requests. The threads hand work to
// each other only through AppState and the bounded queues below.
class Controller {
public:
    // Throws when `surface_required` and no control surface is connected.
    Controller(bool surface_required, const std::string& usb_device);
    // Service thread. Returns once state.running is cleared and the MIDI
    // thread has stopped; a failure sets state.failed.
    void run(AppState& state, ControlServer& server);
private:
    struct MidiSendRequest { std::array<char, 64> address {}; MidiPacket packet; };
    struct MidiCatalog { std::string json; std::vector<std::string> outputs; };

    // controller.cpp
    void run_midi(AppState& state);
    void midi_packet(AppState& state, const MidiPortInfo& port, const MidiPacket& packet, bool syncing);
    void record_surface_packets();
    void poll_platform(AppState& state);
    bool select_source(AppState& state, InputSource source);

    // commands.cpp: the control protocol, run on the service thread.
    std::string command(AppState& state, const std::string& text);
    std::string led(const std::string& text);
    std::string lights_command(AppState& state, const std::string& argument);
    std::string midi_send_command(const std::string& argument);
    std::string mixer_set_command(const std::string& argument);

    bool surface_required;
    std::string usb_device;

    // Hardware, opened in this order.
    HardwareMidi midi;
    HardwareMixer mixer;
    Platform platform;
    SystemServices services;
    ClockIo clock;
    PowerButton power;
    SurfaceLights lights;
    LightDriver light_driver {lights}; // MIDI thread only.

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
