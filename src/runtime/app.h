// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include "drivers/audio.h"
#include "drivers/hardware_midi.h"
#include "runtime/session.h"
#include "surface/controls.h"
#include "surface/leds.h"
#include "surface/midi.h"
#include <juce_graphics/juce_graphics.h>
#include <chrono>
#include <string>

class ClockIo;
class HardwareMixer;
class Platform;
class PowerButton;
class SystemServices;
class UsbAudioOutput;

// One control-surface message, decoded. `input` names the button or control
// (Kind::None for pads); `pad` is 0-15 for a pad's note-on, note-off or
// pressure and -1 otherwise. `syncing` marks the surface's replies to the
// startup position request: controls at their current positions, not moves.
struct SurfaceEvent {
    MidiEvent midi;
    SurfaceInput input;
    int pad = -1;
    bool syncing = false;
};

// Hardware an app may use from the service thread only, in App::service()
// and App::command(). The runtime owns it and restores it on exit. The
// last three are read-only views of what the runtime runs itself.
struct Hardware {
    HardwareMixer& mixer;
    Platform& platform;
    SystemServices& services;
    ClockIo& clock;
    const PowerButton& power;         // The power key, polled on this thread
    const UsbAudioOutput& usb_output; // What a computer records over USB-C
    const MidiPorts& midi;            // MIDI ports as last scanned, once a second
};

// What an application on sample-base provides. run_app() owns the hardware,
// starts four threads and calls these hooks; each says which thread calls it.
// Hooks on different threads run at the same time, so share state between
// them only through atomics, lock-free queues or locks. render() must never
// allocate, block, lock or touch files.
//
// The runtime already handles, for every app: wheel Push back to the stock
// app, the remote control commands (`--ctl`), held-pad and last-control
// tracking, the remote prompt, MIDI port hotplug and turning the app's
// lights off on exit. docs/APPS.md walks through writing one.
class App {
public:
    using Clock = std::chrono::steady_clock;
    virtual ~App() = default;

    // Audio thread, every 128 frames (2.9 ms at 44.1 kHz): writes `output`
    // from `input`, both interleaved stereo S32. Input is silent when capture
    // is unavailable.
    virtual void render(const AudioBlock& input, AudioBlock& output) = 0;

    // MIDI thread: a channel message from the control surface. Wheel Push
    // never arrives while it returns to the stock app.
    virtual void surface(const SurfaceEvent&) {}
    // MIDI thread: any packet from an external port (DIN or USB MIDI),
    // including clock and SysEx.
    virtual void midi(const MidiPortInfo&, const MidiPacket&) {}
    // MIDI thread: an external input disappeared; silence its notes.
    virtual void midi_lost() {}
    // MIDI thread, every 5 ms: request surface lights. Only changes are sent,
    // so asking for the same colour again costs nothing.
    virtual void lights(SurfaceLights&, Clock::time_point) {}

    // Screen thread, every 75 ms: draw the whole 320 x 240 screen. Not called
    // while a remote prompt is shown or with --headless.
    virtual void draw(juce::Graphics&) = 0;

    // Service thread, every few milliseconds: slow work such as mixer routes.
    virtual void service(Hardware&) {}
    // Service thread: an app's own control command, `action` then the rest
    // of the line. Return the reply ("OK ..." or "ERR ..."), or nothing for a
    // command the app does not know. The runtime's commands come first.
    virtual std::string command(const std::string& /*action*/, const std::string& /*argument*/, Hardware&) { return {}; }
    // The app's command names, for the reply to an unknown command.
    virtual std::string command_names() const { return {}; }
    // Any thread: extra `key=value` fields for the `status` line.
    virtual std::string status() const { return {}; }

    // run_app() calls this once, before any hook.
    void attach(const Session& session) { attached = &session; }

protected:
    const Session& session() const { return *attached; }

private:
    const Session* attached = nullptr;
};
