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
| `src/drivers/` | One driver per device: audio, display, MIDI ports, mixer, USB-C audio input, sync jacks, power key, platform files, storage service | `surface/`, `dsp/` |
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
| `surface(event)` | MIDI | Every control-surface message. `event.pad` is 0–15 for a pad's note-on, note-off or pressure; `event.input` names a button or control (`surface/controls.h`); `event.syncing` marks the startup position replies. |
| `midi(port, packet)` | MIDI | Every packet from an external port (DIN or USB MIDI), including clock and SysEx. |
| `midi_lost()` | MIDI | An external MIDI input disappeared; silence its notes. |
| `lights(lights, now)` | MIDI | Every 5 ms. Ask for pad, fader, bank and button lights; only changes go to the surface. |
| `draw(g)` | screen | Every 75 ms. Paint the whole 320 × 240 screen. `load_typeface()` in `runtime/screen.h` gives the device font. |
| `service(hardware)` | service | Every few milliseconds. Slow work: mixer routes, storage, files. |
| `command(action, argument, hardware)` | service | A `--ctl` command the runtime does not know. Return the reply, or nothing. |
| `command_names()`, `status()` | service | Your command names for the error reply, and extra `key=value` fields for `status`. |

`session()` gives every hook read-only access to what the runtime tracks
(`runtime/session.h`):
- the pads held now, the last pad and its velocity;
- the last control touched;
- audio peaks and recoveries;
- whether a remote prompt owns the screen.

`Hardware` gives `service()` and `command()` the mixer, platform (backlight,
power, temperatures), storage service and sync jacks. Anything else in
`src/drivers/`, such as `UsbAudioInput`, an app can own itself.

## What the runtime already does

- **Back to stock:** wheel Push returns to the stock app, and so do
  `stock`, the `--seconds` deadline, a crash and the recovery service. The
  app never sees Push.
- **Remote commands:** `status`, `screenshot`, `audition` (pad 1 pressed for
  300 ms), `prompt`, `led`, `surface`, `surface-log`, `midi`, `midi-send`,
  the mixer, brightness, clock, power and storage commands all work for every
  app (see [HARDWARE.md](HARDWARE.md)).
- **Lights:** the runtime sends only what changed, at most 48 messages per
  pass, and on exit turns off every light the app set.
- **Remote prompt:** while a tool such as `tools/capture_controls.py` shows
  one, the runtime draws it instead of calling `draw()`. Apps should leave
  button presses alone while `session().prompting` is set.
- **MIDI ports:** found by name, reopened after USB hotplug, and the surface
  positions requested at startup.

## Examples

`src/demos/` holds the four proofs of concept as one `DemoApp`, one folder
each:
- `synth/`: pads play C major, and external MIDI plays the same synth.
- `recorder/`: record and play the line inputs, the microphone or USB-C audio.
- `light_show/`: every surface light.
- `pad_touch/`: pad velocity and pressure.

Each folder has the same three parts:
- `<demo>.h`: the pure model that tests can include;
- `<demo>_demo`: its buttons, lights, commands and audio;
- `<demo>_page.cpp`: its screen.

`demo_app.cpp` sends each hook to the demos that use it. `demos/tools.cpp`
shows how an app can add offline tools, such as PNG previews of its
screens, for the build to check.
