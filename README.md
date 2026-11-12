# sample-base

An open-source, standalone JUCE application base for **MPC Sample AC50 firmware 1.3.0.12**.
It draws its own screen, reads the physical pads and makes simple beeps and
boops without running or injecting into the stock MPC application.

The base is built for your own app to plug into: implement one interface,
`App`, and the runtime handles the hardware, threads, remote control and the
safe return to the stock app. See [docs/APPS.md](docs/APPS.md); the smallest
working app is in [src/starter/](src/starter/). The bundled proofs of concept
below live in [src/demos/](src/demos/). An app can also live in its own
repository with this one as a submodule, as Melodee, a sixteen-track
groovebox, does in `melodee-sample`.

The proof of concept has 16 velocity-sensitive pads mapped to C major from
C4 to D6, a 16-voice sine/triangle synth, a pad display and an output meter.
**Push the wheel to return to the stock app.** The main volume knob sets the
synth's output level from its actual position at startup, and the screen
names the last button, knob or power press. The synth has a short attack and release.
External MIDI now plays the demo synth.

**Input recording showcase.** Press **Sample** for the recorder page. The
three buttons above the screen are its soft keys: the left one steps the
input through Line in, Mic, USB-C and Off; the middle one picks L + R, L or
R for the line inputs and USB-C (the microphone is always mono); the right
one returns to the synth. **Sample Record** starts and finishes a take of up
to 60 seconds, **Play** plays it from the start (also ending a take in
progress) and **Stop** stops either. The screen shows L/R input meters, the
take's waveform and the playhead, and the transport buttons light up.
Inputs stay untouched until one is chosen.

**Light show.** Press **Pad FX** for a 49-second looping LED showcase that
uses every surface light: a roll call that lights the bank lights, buttons,
fader and pads one by one, every button colour, a colour for each pad, then
rainbow, breathing fades, ripples, sparkles, a comet, level meters and a
finale. The screen names the scene and mirrors the pad, fader and bank
lights; the left and middle buttons above the screen step back and forward
through the scenes, and the right one (or **Pad FX** again) returns to the
synth and turns the lights off. Pads still play while it runs, glowing white
under your finger.

**Pad touch.** Press **16 Levels** to see how hard the pads are played. Each
pad lights up when struck, in a colour and brightness set by its velocity
(blue when soft, through green and yellow, to red when hard), then follows
its pressure (aftertouch) while held and fades out after release. The bank
lights A–H meter the hardest held pad and the fader light takes its colour.
The screen shows every pad's latest velocity, a pressure fill on held pads,
the last pad's velocity, pressure and peak pressure, and five seconds of
pressure with each strike marked. The right button above the screen (or
**16 Levels** again) returns to the synth. Pressure does not change the sound.

**Step sequencer.** Press **SEQ** for a drum machine: eight drums made in
code (kick, snare, clap, closed and open hat, low and high tom, bell), one
track each, sixteen 16th-note steps. It starts with a house groove. In step
mode the pads are the selected track's steps: tap one to set it at the
strike's velocity, tap it again to clear it. The left button above the screen
switches to drum mode, where pads 1–8 play the drums and pads 9–16 mute them.
**Pad Bank** or the middle button above the screen picks the track (backwards
with **Shift**), shown on the bank lights A–H. **Play** starts the bar and
**Stop** stops it from any page; it keeps playing while other pages are open.
**Seq Record** arms recording: in drum mode, hits are written onto the
nearest step while it plays. **Tap Tempo**, the wheel, **−** and **+** set the
tempo from 40 to 240 BPM (0.1 BPM steps with **Shift**), and Tap Tempo
flashes on the beat. **Mute** mutes the selected track and **Erase** clears it
(**Shift + Erase** clears every track). The screen shows all eight tracks,
the playhead, the tempo and the step playing.

**System dashboard.** Press **Knob FX** for the MPC's live state on four
panels; the left and middle buttons above the screen, or the wheel, step
through them. *Health*: CPU load overall, per core and over the last 24
seconds, memory, the CPU and GPU temperatures, the supply voltage, the power
key and the backlight level. *Audio*: the audio engine's running time, output
and input meters and recoveries, the USB-C port's state and USB-C audio in
and out with their rates, underruns and drift trim. *Ports*: every open MIDI
port and its directions, dropped messages, and whether headphones and line
out are plugged in. *Storage*: each mounted device with its free space. The
readings are taken twice a second, only while the page is open. Power shows
as a voltage: the AC50 has no battery gauge, so there is no percentage. The
right button above the screen (or **Knob FX** again) returns to the synth.

Presets, sequences longer than one bar, saved patterns, project
storage and saving takes are future work. The reusable base also supplies audio input, mixer
controls, headphone and line-out plug detection, USB-C audio to a computer (whatever the app plays), bidirectional MIDI and hotplug, pad/button/bank lights,
display/backlight, sync GPIO,
power, thermal, CPU and memory telemetry, storage-service integration and session recovery.
See [docs/HARDWARE.md](docs/HARDWARE.md) for APIs, commands and reference wiring.

## Build

On a Mac, install Docker and Python 3, then run:

```sh
git clone https://github.com/keremimo/sample-base.git
cd sample-base
./build.sh
```

This builds Linux ARM64 binaries in a Debian container, runs the tests,
renders a note and its release into `build/test.wav`, and creates
`build/preview.png`, `build/preview-recorder.png`, `build/preview-lights.png`,
`build/preview-pads.png`, `build/preview-seq.png` and `build/preview-system.png`. The complete app is
`build/sample-base`; no sound module, factory-preset bank or sibling checkout
is needed.

The build downloads [JUCE 9.0.5](https://github.com/juce-framework/JUCE/releases/tag/9.0.5),
pinned to revision `d60b7a78030661574a615b586d9325ba7a7fa7dd`, into ignored
`build/deps/JUCE`. All synth code is included in [src/demos/synth/synth.h](src/demos/synth/synth.h).

On ARM64 Debian Linux, install the packages in `tools/Dockerfile`, then run
`python3 tools/dependencies.py` and `make -j4` for an equivalent native build.
The offline app checks are:

```sh
build/sample-base --self-test build/test.wav
build/sample-base --preview build/preview.png
build/sample-base --preview-recorder build/preview-recorder.png
build/sample-base --preview-lights build/preview-lights.png [MS]
build/sample-base --preview-pads build/preview-pads.png
build/sample-base --preview-seq build/preview-seq.png
build/sample-base --preview-system build/preview-system.png [health|audio|ports|storage]
```

No offline mode opens the MPC's audio, display or control surface.

## Install from a release (no SSH)

Each [release](https://github.com/keremimo/sample-base/releases) has a zip
with an `autorun` folder. Copy it to the top level of the MPC's microSD card
(move any existing `autorun` folder away first) and switch the MPC on: the
stock app starts, runs the card's signed autorun, and a few seconds later
the demos take over. Push the wheel to go back to stock until the next boot;
delete the folder to uninstall. The zip's `README.txt` has the details.

To make one, commit, then run `./release.sh v0.1.0`. It builds, strips and
self-tests the app, and [tools/make_release.py](tools/make_release.py)
writes `dist/sample-base-demos-MPC-Sample-<version>.zip` and its `.sha256`
from [release/README.txt](release/README.txt), the
[SD boot scripts](install/) and the licences. The autorun hands
`autorun/sample-base/` to the same SD boot path as `install-sd` below.

## Run on the MPC

Connect USB networking with root SSH access configured:

```sh
python3 device.py start
python3 device.py status
python3 device.py ctl audition
python3 device.py screenshot
python3 device.py stock
```

The helper defaults to `192.168.155.1:2222` and
`~/.ssh/mpc_sample_client_ed25519`. `--host`, `--port` and `--key` override
these. Supply your own SSH key and keep it outside this repository.
The start command restarts applications, so finish any project edits first;
there is no additional confirmation prompt.

Start verifies the known stock identity, copies the app and recovery script
into a **new RAM directory**, checks their hashes, and runs its audio self-test
before switching. It stops first when `/run` is short of room, and once the new
app runs it removes the copies earlier starts left there (never the SD boot's).
An independent `sample-base.service` stops `ac50.service` for the session and
starts it again after **normal exit, wheel Push, remote stop, startup failure,
or a process crash**. During system shutdown the recovery script lets systemd
finish stopping services. Existing stock/plugin startup configuration is preserved.
The session unit and binaries live under `/run`; reboot uses the previous
startup configuration. No firmware, stock libraries or SD startup files change.

To start an app from the SD card at every boot:

```sh
python3 device.py install-sd --app demos
python3 device.py uninstall-sd
```

`install-sd` copies the app, the recovery script and two small boot scripts
into `sample-base/` on the card with their hashes, keeps the card's autorun
in `sample-base/autorun-backup/`, and chains a start after the card's own
`autorun/autorun.sh` (signed in `autorun.check`, the firmware's salted
digest). At boot the stock app runs that autorun; the boot script hands off
to a separate service that checks the stock identity and the files, copies
them into a new RAM directory, runs the self-test and starts the same
session unit as `start`. Leaving the app (or any failure) returns to stock
until the next boot. Only the card changes; `--remove-plugins` also takes
MPC-Plugins' start out of that autorun.

For a bounded test that automatically returns to stock:

```sh
python3 device.py start --seconds 45
```

`--app starter` runs the starter app instead of the demos.

## Source layout

`src/` is in layers. Each include names its folder (`"drivers/audio.h"`),
and a layer only includes the ones before it in this table;
`tests/test_layout.py` checks both.

| Folder | What it is |
| --- | --- |
| `src/surface/` | MIDI parsing and the measured control-surface maps: buttons and knobs, pads, lights and LED colours |
| `src/dsp/` | Realtime-safe audio pieces: PCM decoding, rate conversion, a lock-free frame ring and the input recorder |
| `src/drivers/` | One driver per device: audio (and the codec's power around it), display, MIDI ports, mixer, jack detection, USB-C audio, SD card access over USB-C, sync, power key, platform files, storage service |
| `src/runtime/` | Hosts one app: the `App` interface (`app.h`), threads, control socket and commands, `status`, options, screen helpers |
| `src/demos/` | The proofs of concept as one app, one folder each (below) |
| `src/starter/` | The smallest complete app, to copy for your own |
| `src/main.cpp` | Picks the app (`--app`) and runs one mode |

Each demo folder has its pure, testable model (`<demo>.h`), its buttons,
lights, commands and audio (`<demo>_demo`) and its screen
(`<demo>_page.cpp`):

| `src/demos/` | What it is |
| --- | --- |
| `synth/` | Pads play C major on the beep/boop synth; the home page |
| `recorder/` | **Sample**: record and play an input |
| `light_show/` | **Pad FX**: the LED showcase |
| `pad_touch/` | **16 Levels**: pad velocity and pressure |
| `sequencer/` | **SEQ**: step sequencer and drum machine |
| `system/` | **Knob FX**: the system dashboard |
| `demo_app` | Routes the runtime's hooks to the demos and switches pages |
| `demo_state.h` | What the demos share across threads |
| `ui`, `demo_screen`, `screen_feed` | Shared look, per-frame view and page drawing |
| `tools` | `--self-test` and the previews |

A new feature gets its own file in the folder it belongs to; `main.cpp`
only gains a line for a new app. [docs/APPS.md](docs/APPS.md) explains the
hooks, their threads and how to wire in an app.

## Implementation

- **JUCE 9 graphics** draws to a software `juce::Image`. DRM presents the
  image on the existing ST7796S panel driver. X11, a compositor and OpenGL
  are unnecessary. This is an embedded app loop, not a desktop window.
- **ALSA** owns stereo I2S playback and optional capture at 44.1 kHz, S32,
  with 128-frame processing blocks and a nominal 512-frame hardware buffer.
  Rendering, MIDI and service work run separately. Input monitoring starts off.
  The mixer exposes real controls and validates explicit route changes.
- **ALSA raw MIDI** discovers and opens the surface, external and USB ports
  in both directions. Full packets, realtime and SysEx are supported.
  The control surface is read directly. Pads use the measured, nonsequential
  channel-10 map in `src/surface/pads.h`. Every other button, knob, the wheel and the
  fader use the measured channel-1 map in `src/surface/controls.h`; wheel Push is
  note 111. The power button is the PMIC `KEY_POWER` input device.
  Running status, split reads, realtime
  bytes and note-on velocity-zero releases are handled. Queue overflow
  clears pending events and silences voices rather than losing a note-off.
- **Built-in synth** mixes sine and triangle oscillators with velocity and
  5 ms attack / 80 ms release envelopes. Sixteen fixed voice slots keep
  rendering bounded, with no allocations, locks or file I/O.
- **Input recorder** ([src/dsp/recorder.h](src/dsp/recorder.h)) keeps one
  preallocated 60-second stereo take in RAM and runs inside the audio
  callback. Line in and Mic come from the I2S capture pair through the
  existing exclusive `TRS` / `Builtin Microphone` route switch. USB-C input
  ([src/drivers/usb_audio.cpp](src/drivers/usb_audio.cpp)) reads the kernel's UAC2 gadget
  capture PCM on its own thread, converts the host's rate to the device's and
  crosses into the audio thread through a lock-free ring.
- **USB-C output** (the same driver) sends whatever the app plays to a
  computer recording from the MPC: the runtime hands every rendered block to
  another ring, and a thread converts it to the computer's rate, 44.1 kHz on
  1.3.0.12, and writes the gadget's playback PCM. Both ways the conversion is
  a band-limited windowed sinc, flat to 20 kHz with images and aliases at
  least 90 dB down, and a slow trim of it follows the clock drift between
  the two, so long sessions stay continuous. The runtime widens the gadget to 18 channels so an app
  can send 16 mono track stems after the main mix
  ([src/drivers/usb_gadget.h](src/drivers/usb_gadget.h)).
- Fonts come from the MPC's existing Liberation Sans installation.

The hardware reference is inMusic's
[MPC Sample 1.3.0 source release](https://github.com/inmusic-software/az0x-open-source/tree/mpc-sample/1.3.0),
especially the kernel's `rk3566-az07-ac50.dts`,
`drivers/gpu/drm/tiny/st7796s.c` and `sound/drivers/serial-generic.c`.
Existing kernel drivers are used; no kernel rebuild or installation is needed.

## Validation and licensing

See [docs/VALIDATION.md](docs/VALIDATION.md) for checked behavior and hardware
limitations. Run the regression tests with:

```sh
python3 -m unittest discover -s tests -v
```

Copyright (C) 2026 Kerem Kilic (Ellic Studio). The standalone app and its
project tooling are [AGPL-3.0-only](LICENSE), using JUCE's open-source option;
JUCE also offers commercial licensing. The adapted privacy checker retains
its [MIT licence](LICENSES/MPC-Plugins-MIT.txt).
See [THIRD_PARTY.md](THIRD_PARTY.md) for attribution and scope.
Proprietary firmware, keys, projects, captures, downloaded dependencies and
binaries stay out of Git.

Contributions are welcome under the licence of the affected files. Before
committing, enable the privacy hook with `git config core.hooksPath .githooks`
and run `python3 tools/check_private_files.py` after staging. It takes source
text only, plus the docs' own images (PNG, JPEG, GIF or WebP under a
`docs/images/` folder). Keep generated builds and local device material in
ignored storage. This is an independent
community project, unaffiliated with Akai Professional or inMusic.
