// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include "drivers/audio.h"
#include "drivers/hardware_midi.h"
#include "drivers/screen_mirror.h"
#include "runtime/session.h"
#include "surface/controls.h"
#include "surface/leds.h"
#include "surface/midi.h"
#include <juce_graphics/juce_graphics.h>
#include <chrono>
#include <string>
#include <vector>

class ClockIo;
class CardShare;
class HardwareMixer;
class JackDetect;
class Platform;
class PowerButton;
class SystemServices;
class UsbAudioOutput;
struct UsbStems;

// One control-surface message, decoded. `input` names the button or control
// (Kind::None for pads); `pad` is 0-15 for a pad's note-on, note-off or
// pressure and -1 otherwise. `syncing` marks the surface's replies to the
// startup position request: controls at their current positions, not moves.
// `timestamp_ns` is when the message came in, CLOCK_MONOTONIC (the kernel's
// receive stamp where the port has one, else when the MIDI thread read it),
// so an app can place a pad hit to the frame; 0 for one made up by the app.
struct SurfaceEvent {
    MidiEvent midi;
    SurfaceInput input;
    int pad = -1;
    bool syncing = false;
    uint64_t timestamp_ns = 0;
};

// Hardware an app may use from the service thread only, in App::service()
// and App::command(). The runtime owns it and restores it on exit (a card
// lent to a computer comes back). The last four are read-only views of what the runtime runs itself.
struct Hardware {
    HardwareMixer& mixer;
    Platform& platform;
    SystemServices& services;
    ClockIo& clock;
    CardShare& card;                  // SD card access over USB-C (drivers/card_share.h)
    const PowerButton& power;         // The power key, polled on this thread
    const JackDetect& jacks;          // Headphones and line out, polled on this thread
    const UsbAudioOutput& usb_output; // What a computer records over USB-C
    const MidiPorts& midi;            // MIDI ports as last scanned, once a second
};

// What an app may send from the MIDI thread, in App::midi_output(): MIDI to
// an external port open now (DIN or USB, never the control surface) and the
// CV_CLK_OUT sync line. The runtime calls the hook on every pass of its MIDI
// loop: at least every 5 ms, as soon as input arrives, and as close after
// the time asked for with wake_at() as the kernel wakes it (well under a
// millisecond), so an app can keep its own schedule of timed messages.
class MidiOutput {
public:
    using Clock = std::chrono::steady_clock;
    virtual ~MidiOutput() = default;
    // The external ports open now (the surface is left out); `output` says
    // which can be written. Rescanned once a second, after USB hotplug.
    virtual const std::vector<MidiPortInfo>& ports() const = 0;
    // One complete message to `address`, written before this pass ends.
    // False when that port is gone or its queue is full.
    virtual bool send(const std::string& address, const MidiPacket& packet) = 0;
    // CV_CLK_OUT high or low now. The first call takes the line (opt-in, as
    // the `clock-out` command does); false when the kernel or another owner
    // holds it, so an app should stop asking until the user turns it on
    // again. On exit the runtime leaves it low and releases it.
    virtual bool sync(bool high) = 0;
    // Call the hook again by `at`; the earliest asked for in a pass wins.
    virtual void wake_at(Clock::time_point at) = 0;
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
    // Any thread: the rate and queued periods the app wants the audio device
    // in. The runtime opens it so at start and, within about 0.1 s of a
    // change, closes and reopens it (a short silence). A format the device
    // refuses falls back to the default; session().audio.rate / periods say
    // what runs. Apps that never override it run at 44.1 kHz, three periods.
    virtual AudioFormat audio_format() const { return {}; }
    // Audio thread, before the first render() of each opened device: the
    // format it runs in (rate, and the frames queued ahead of the jacks).
    virtual void audio_opened(const AudioFormat&) {}
    // Any thread: whether the screen is offered to a computer as a USB webcam
    // and at which size (drivers/screen_mirror.h; ScreenMirror::widths and
    // heights list the sizes). The runtime acts on a change within a frame or
    // two; turning it on or off, or another size, rebinds the USB gadget.
    // session().mirror says how it goes. Apps that never override it have
    // no webcam (a webcam an earlier app left is taken out).
    virtual ScreenMirrorMode screen_mirror() const { return {}; }
    // Audio thread, right after each render(): that block's stems, one mono
    // channel per track, which a computer records over USB-C beside the main
    // mix (channels 3 to 18), or null for silence there. The app owns the
    // block and writes it in render().
    virtual const UsbStems* stems() const { return nullptr; }

    // MIDI thread: a channel message from the control surface. Wheel Push
    // never arrives while it returns to the stock app.
    virtual void surface(const SurfaceEvent&) {}
    // MIDI thread: any packet from an external port (DIN or USB MIDI),
    // including clock and SysEx.
    virtual void midi(const MidiPortInfo&, const MidiPacket&) {}
    // MIDI thread: an external input disappeared; silence its notes.
    virtual void midi_lost() {}
    // MIDI thread, every pass of the MIDI loop (see MidiOutput): send
    // external MIDI and drive the sync line, each at its time.
    virtual void midi_output(MidiOutput&, Clock::time_point) {}
    // MIDI thread, every 5 ms: request surface lights. Only changes are sent,
    // so asking for the same colour again costs nothing.
    virtual void lights(SurfaceLights&, Clock::time_point) {}

    // Screen thread, once per frame: draw the whole 320 x 240 screen. Frames
    // come as fast as the panel takes them (it has no vsync), at most 60 a
    // second; animate from the clock, not by counting frames. Not called
    // while a remote prompt is shown or with --headless.
    virtual void draw(juce::Graphics&) = 0;

    // Service thread, every few milliseconds: slow work such as mixer routes.
    virtual void service(Hardware&) {}
    // Service thread: headphones or line out plugged or unplugged, about
    // 150 ms after the contact moved (the kernel's debounce); session()
    // already says so. Each jack also arrives once with `initial` set when its
    // state is first known, normally before the first service(), so an app
    // can set its routing from the start.
    virtual void jack(const JackEvent&, Hardware&) {}
    // Service thread: an app's own control command, `action` then the rest
    // of the line. Return the reply ("OK ..." or "ERR ..."), or nothing for a
    // command the app does not know. The runtime's commands come first.
    virtual std::string command(const std::string& /*action*/, const std::string& /*argument*/, Hardware&) { return {}; }
    // The app's command names, for the reply to an unknown command.
    virtual std::string command_names() const { return {}; }
    // Any thread: extra `key=value` fields for the `status` line.
    virtual std::string status() const { return {}; }

    // MIDI thread: true when the app uses wheel Push itself. It must then
    // offer its own way back to the stock app (an exit page) and return true
    // from finished() when chosen. Remote stop, the deadline, a crash and the
    // recovery service still return to stock as before.
    virtual bool claims_wheel_push() const { return false; }
    // Service thread, every few milliseconds: true once the app wants the
    // runtime to stop and return to the stock app.
    virtual bool finished() const { return false; }

    // run_app() calls this once, before any hook.
    void attach(const Session& session) { attached = &session; }

protected:
    const Session& session() const { return *attached; }

private:
    const Session* attached = nullptr;
};
