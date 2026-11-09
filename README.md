# sample-base

An open-source, standalone JUCE application base for **MPC Sample AC50 firmware 1.3.0.12**.
It draws its own screen, reads the physical pads and makes simple beeps and
boops without running or injecting into the stock MPC application.

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
Inputs stay untouched until one is chosen. Presets, sequencing, project
storage, saving takes and a USB audio output bridge are future work. The reusable base also supplies audio input, mixer and
jack controls, bidirectional MIDI and hotplug, pad/button/bank lights,
display/backlight, sync GPIO,
power telemetry, storage-service integration and session recovery.
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
`build/preview.png` and `build/preview-recorder.png`. The complete app is
`build/sample-base`; no sound module, factory-preset bank or sibling checkout
is needed.

The build downloads [JUCE 9.0.5](https://github.com/juce-framework/JUCE/releases/tag/9.0.5),
pinned to revision `d60b7a78030661574a615b586d9325ba7a7fa7dd`, into ignored
`build/deps/JUCE`. All synth code is included in [src/synth.h](src/synth.h).

On ARM64 Debian Linux, install the packages in `tools/Dockerfile`, then run
`python3 tools/dependencies.py` and `make -j4` for an equivalent native build.
The offline app checks are:

```sh
build/sample-base --self-test build/test.wav
build/sample-base --preview build/preview.png
build/sample-base --preview-recorder build/preview-recorder.png
```

No offline mode opens the MPC's audio, display or control surface.

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
before switching.
An independent `sample-base.service` stops `ac50.service` for the session and
starts it again after **normal exit, wheel Push, remote stop, startup failure,
or a process crash**. During system shutdown the recovery script lets systemd
finish stopping services. Existing stock/plugin startup configuration is preserved.
The session unit and binaries live under `/run`; reboot uses the previous
startup configuration. No firmware, stock libraries or SD startup files change.

For a bounded test that automatically returns to stock:

```sh
python3 device.py start --seconds 45
```

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
  channel-10 map in `src/pads.h`. Every other button, knob, the wheel and the
  fader use the measured channel-1 map in `src/controls.h`; wheel Push is
  note 111. The power button is the PMIC `KEY_POWER` input device.
  Running status, split reads, realtime
  bytes and note-on velocity-zero releases are handled. Queue overflow
  clears pending events and silences voices rather than losing a note-off.
- **Built-in synth** mixes sine and triangle oscillators with velocity and
  5 ms attack / 80 ms release envelopes. Sixteen fixed voice slots keep
  rendering bounded, with no allocations, locks or file I/O.
- **Input recorder** ([src/recorder.h](src/recorder.h)) keeps one
  preallocated 60-second stereo take in RAM and runs inside the audio
  callback. Line in and Mic come from the I2S capture pair through the
  existing exclusive `TRS` / `Builtin Microphone` route switch. USB-C input
  ([src/usb_audio.cpp](src/usb_audio.cpp)) reads the kernel's UAC2 gadget
  capture PCM on its own thread, converts the host's rate to 44.1 kHz and
  crosses into the audio thread through a lock-free ring.
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
and run `python3 tools/check_private_files.py` after staging. Keep generated
builds and local device material in ignored storage. This is an independent
community project, unaffiliated with Akai Professional or inMusic.
