# Writing an app on sample-base

sample-base runs one **app** on the MPC Sample. The runtime owns the
hardware, the threads, the control socket and the safe return to the stock
app. An app implements a single interface, `App` in
[src/runtime/app.h](../src/runtime/app.h), and decides what the pads,
buttons, sound, lights and screen do.

The quickest start is to copy the starter app, about 60 lines that play a
sine per pad, light held pads and draw a line of text:

```sh
build/sample-base --app starter --headless --no-surface \
    --audio-output null --audio-input off --runtime-dir /tmp/starter   # on a Linux host
python3 device.py start --app starter                                   # on the MPC
```

## Wiring in your own app

1. Copy `src/starter/` to `src/myapp/` and rename `StarterApp` to `MyApp`.
2. In `src/main.cpp`, add a line to `make_app()`:
   ```cpp
   if (options.app == "myapp") return std::make_unique<MyApp>();
   ```
   To make it the default, change `Options::app` in
   `src/runtime/options.h`.
3. Add `"myapp": BASE | {"drivers", "runtime", "myapp"}` to `ALLOWED` in
   `tests/test_layout.py`. The layout test fails until you do, so every
   folder has a deliberate place in the layers.
4. Run `./build.sh`, then `python3 device.py start --app myapp`. Push the
   wheel to go back to the stock app.

The Makefile picks up every `.cpp` under `src/` by itself.

## The layers

Each include names its folder (`"drivers/audio.h"`), and a layer only
includes the layers above it in this list. `tests/test_layout.py` checks
both rules.

| Folder | What it holds | May include |
| --- | --- | --- |
| `src/surface/` | MIDI parsing and the measured control-surface maps: buttons and knobs (`controls.h`), pads (`pads.h`), lights (`leds.h`), even-looking LED colours (`led_colours.h`) | `dsp/` |
| `src/dsp/` | Realtime-safe audio pieces: PCM decoding, rate conversion and a lock-free frame ring (`frame_ring.h`), the input recorder (`recorder.h`) | `surface/` |
| `src/drivers/` | One driver per device: audio, display, MIDI ports, mixer, jack detection, USB-C audio in and out, sync jacks, power key, platform files, storage service | `surface/`, `dsp/` |
| `src/runtime/` | Hosts one app: `App`, the threads, the control socket and its commands, `status`, options and screen helpers | `drivers/`, `surface/`, `dsp/` |
| `src/demos/`, `src/starter/`, your app | Apps | all of the above, not each other |
| `src/main.cpp` | Picks the app with `--app` and runs one mode | everything |

## The hooks and their threads

The runtime runs four threads at once and calls each hook from one of them.
Hooks on different threads run at the same time, so they share state only
through atomics, `MidiQueue` (single producer, single consumer) or a lock.
`render()` must never allocate, block, lock or touch files.

| Hook | Thread | When |
| --- | --- | --- |
| `render(input, output)` | audio | Every 128 frames (2.9 ms). Fill `output` from `input`; both are interleaved stereo S32 at 44.1 kHz. Input is silent without capture. |
| `stems()` | audio | Right after each `render()`. Optional: a `UsbStems` block (`drivers/usb_audio.h`) the app wrote in `render()`, one mono float channel per track, which a computer records on USB-C channels 3–18. The default, null, sends silence there. |
| `surface(event)` | MIDI | Every control-surface message. `event.pad` is 0–15 for a pad's note-on, note-off or pressure; `event.input` names a button or control (`surface/controls.h`); `event.syncing` marks the startup position replies. |
| `midi(port, packet)` | MIDI | Every packet from an external port (DIN or USB MIDI), including clock and SysEx. |
| `midi_lost()` | MIDI | An external MIDI input disappeared; silence its notes. |
| `midi_output(out, now)` | MIDI | Every pass of the MIDI loop: at least every 5 ms, as soon as input arrives, and when the app asked to be woken (`out.wake_at()`). `out` sends complete messages to an external port open now (`out.ports()`; never the surface) and sets the `CV_CLK_OUT` sync line (`out.sync()`, taken on first use). For MIDI and sync output timed by the app's own schedule; see "Timed MIDI out" below. |
| `lights(lights, now)` | MIDI | Every 5 ms. Ask for pad, fader, bank and button lights; only changes go to the surface. |
| `draw(g)` | screen | Once per frame, as fast as the panel takes frames and at most 60 a second; animate from the clock, not the frame count. Paint the whole 320 × 240 screen. `load_typeface()` in `runtime/screen.h` gives the device font. |
| `service(hardware)` | service | Every few milliseconds. Slow work: mixer routes, storage, files. |
| `jack(event, hardware)` | service | Headphones or line out plugged or unplugged (`drivers/jacks.h`), about 150 ms after the contact moved. Each jack also arrives once with `event.initial` set when its state is first known, so routing can follow the jacks from the start. |
| `command(action, argument, hardware)` | service | A `--ctl` command the runtime does not know. Return the reply, or nothing. |
| `command_names()`, `status()` | service | Your command names for the error reply, and extra `key=value` fields for `status`. |

`session()` gives every hook read-only access to what the runtime tracks
(`runtime/session.h`):
- the pads held now, the last pad and its velocity;
- the last control touched;
- audio peaks and recoveries;
- whether headphones and line out are plugged in, `session().plugged(Jack::Headphones)`;
- whether a remote prompt owns the screen;
- SD card access: whether the card is home, lending, lent or coming back,
  whether a computer is connected and has ejected it, and a count that
  moves each time it comes back (`card_share`, `card_connected`,
  `card_ejected`, `card_returns`). While it is not home the app must not
  touch the card.

`Hardware` gives `service()`, `jack()` and `command()` the mixer (routes),
platform (backlight, power, temperatures, CPU load, memory and storage
space), storage service, SD card access (`card`: lend the card to a
computer over USB-C and take it back) and sync jacks, plus read-only views of the power
key, jack detection, the USB-C audio output and the MIDI ports as last
scanned.
Anything else in `src/drivers/`, such as `UsbAudioInput`, an app can own
itself. The system demo (`src/demos/system/`) reads all of these.

## What the runtime already does

- **Back to stock:** wheel Push returns to the stock app, and so do
  `stock`, the `--seconds` deadline, a crash and the recovery service. The
  app never sees Push, unless it returns true from `claims_wheel_push()`;
  it must then offer its own way out (an exit page) and return true from
  `finished()` once chosen. The other ways back stay as they are.
- **Remote commands:** `status`, `screenshot`, `audition` (pad 1 pressed for
  300 ms), `prompt`, `led`, `surface`, `surface-log`, `midi`, `midi-send`,
  the mixer, brightness, clock, power, storage, `card-share` and `usb-output` commands
  all work for every app (see [HARDWARE.md](HARDWARE.md)).
- **USB-C audio out:** whenever a computer records from the MPC over USB-C,
  it gets exactly what `render()` wrote, after the app's own volume, on
  channels 1 and 2, and whatever `stems()` returns on 3 to 18. The app does
  nothing for the main mix; `--usb-output off` or `usb-output off` turns it
  off.
- **Lights:** the runtime sends only what changed, at most 48 messages per
  pass, and on exit turns off every light the app set.
- **Remote prompt:** while a tool such as `tools/capture_controls.py` shows
  one, the runtime draws it instead of calling `draw()`. Apps should leave
  button presses alone while `session().prompting` is set.
- **MIDI ports:** found by name, reopened after USB hotplug, and the surface
  positions requested at startup.
- **SD card access:** `Hardware::card` lends the microSD card to a
  computer as a USB drive beside USB audio, MIDI and networking, and takes
  it back (HARDWARE.md, "SD card access"); `card-share` does the same from
  the remote. On exit, after a crash or before the stock app starts, a lent
  card comes back. The app shows its own page for it and keeps away from
  the card meanwhile (Melodee: `src/melodee/shell/card_access.*`).
- **Jacks:** headphone and line-out detection followed through ALSA control
  events, in `session()` for every thread, `jack()` on each change and the
  `jacks` command. Nothing is rerouted unless the app does it.

## Timed MIDI out

External MIDI arrives with the kernel's receive time in
`MidiPacket::timestamp_ns` (CLOCK_MONOTONIC, the clock `steady_clock`
reads; alsa-lib's timestamped rawmidi read), so an app can measure an
incoming clock without the loop's wake-up in it. For output the runtime
keeps no schedule of its own: an app that wants a message at a given time
holds it until then and asks `wake_at()` for that time; the MIDI loop
sleeps in `ppoll` on the inputs with that deadline, which the PREEMPT_RT
kernel's high-resolution timers keep to well under a millisecond. To line
MIDI up with the sound, time it on the audio clock: a block rendered now is
heard `audio_frames * audio_queued_periods` frames later
(`drivers/audio.h`). Melodee does this (`src/melodee/engine/midi_out.h`).
Messages are built with `juce::MidiMessage` (`juce_audio_basics` is part of
the build). The ALSA sequencer could schedule in the kernel instead, but on
this kernel its queues tick on the 1 ms system timer or the PCM's 2.9 ms
period (no `snd-hrtimer`), and `snd-seq-midi` would contend for the raw
ports the runtime holds.

## Examples

`src/demos/` holds the six proofs of concept as one `DemoApp`, one folder
each:
- `synth/`: pads play C major, and external MIDI plays the same synth.
- `recorder/`: record and play the line inputs, the microphone or USB-C audio.
- `light_show/`: every surface light.
- `pad_touch/`: pad velocity and pressure.
- `sequencer/`: a one-bar step sequencer playing eight drums made in code.
- `system/`: a dashboard of the MPC's live state, read on the service
  thread only while its page is open.

Each folder has the same three parts:
- `<demo>.h`: the pure model that tests can include;
- `<demo>_demo`: its buttons, lights, commands and audio;
- `<demo>_page.cpp`: its screen.

`demo_app.cpp` sends each hook to the demos that use it. `demos/tools.cpp`
shows how an app can add offline tools, such as PNG previews of its
screens, for the build to check.
