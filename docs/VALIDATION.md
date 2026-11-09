# Validation — 9 November 2026

Target: MPC Sample AC50, firmware 1.3.0.12. Standalone JUCE 9.0.5 application
with reusable hardware interfaces and a built-in sine/triangle demo synth.

## Automated offline checks

- Linux ARM64 build passes with application sources using
  `-Wall -Wextra -Werror`. The build renders a two-second stereo WAV and the
  320 × 240 JUCE preview. Audio peak: `0.0228731`; RMS: `0.00846634`;
  final-half-second RMS: `0`.
- Twenty-seven regression tests pass on Linux ARM64. On macOS, eighteen
  portable tests pass and nine Linux-specific tests are skipped. The build
  also renders the recorder, light show and pad touch page previews
  `build/preview-recorder.png`, `build/preview-lights.png` and
  `build/preview-pads.png`.
- Recorder fixtures cover L + R / L / R selection, mono microphone mixing,
  S16_LE, S24_3LE and S32_LE conversion, 44.1/48/32 kHz linear rate
  conversion, ring priming, underrun re-priming, overflow and flush, takes
  refused without an input, record/finish/play/stop, playback mixing,
  route-change and full-buffer endings and stale overview peaks.
- A fake `UAC2_Gadget` card in front of ALSA's null PCM verifies discovery by
  driver name, waiting without opening while `Capture Rate` is 0, opening at
  the host's rate, conversion into the audio ring, reopening on a rate change
  and closing when the host stops. The headless app test drives `source`,
  `channels`, `transport`, `page` and `recorder`, including a USB-C selection
  with no gadget present.
- Light fixtures cover the measured pad, fader, bank and button messages,
  which buttons accept red, nothing sent before an application sets a light,
  change-only updates and a full or limited queue resuming on the next pass.
  Output fixtures fill the 64-message port queue and drain it in one pass.
- Light show fixtures check scene timing and wrap-round, a roll call that
  names all 42 lights once in panel order, red only on white/red buttons,
  sixteen distinct pad hues, smooth fades, and every 25 ms frame of the loop:
  only values the hardware accepts, every light used, at least 100 colours
  per pad and each frame within one 48-message update. The headless app test
  drives `page lights`, `lights N|next|previous` and their errors.
- Pad touch fixtures check that a strike resets pressure and peak, that
  pressure raises the peak and release keeps the strike, that the screen
  trace sees a press shorter than a frame, out-of-range pads, the strike
  flash settling to its resting share, pressure taking over, force colours
  within 0–127 from blue to full red, release fades and the bank meter.
  The headless app test opens `page pads` and reads `pressure` and
  `peak_pressure` from `status`.
- Control-map fixtures cover every measured button and control number,
  channel separation from pads, release by note-off or velocity zero, signed
  wheel steps, main-volume gain and the lossless surface log. A fake control
  socket drives the guided capture through next, redo, back, empty-step
  confirmation, mixer comparison and power-key steps. An injected evdev
  fixture covers power-key discovery, press/release, autorepeat and
  monotonic timestamps.
- Native MIDI fixtures cover running status, fragmented reads, realtime
  interleaving, system-common messages, bounded SysEx, malformed messages,
  note/pressure/CC surface state, queue wrap-around and overflow notification.
- Mock ALSA MIDI ports verify input/output subdevice discovery, partial
  writes and retry, output-queue limits, USB disconnect/reconnect, resource
  cleanup, monotonic receipt timestamps and disconnect panic events.
- Mock PCM transfers exercise fragmented capture, short writes, EAGAIN and
  playback/capture underruns. A renderer always receives complete input
  blocks or silence; the recovery paths preserve frame accounting.
- Mock ALSA mixer controls verify exact channel counts, range checking,
  read-only controls, mutually exclusive input routes and restoration of
  the original stereo volume and route without briefly enabling both inputs.
- Injected sysfs/proc fixtures cover power, missing battery capacity,
  backlight bounds/restoration, USB state, temperatures and mount decoding,
  including spaces in mount paths and read-only mounts.
- Mock GPIO requests verify opt-in sync access by line name, rejection of
  already-owned lines, timestamped rising/falling edges, loss detection and
  returning the output low on normal cleanup. Codec/reset GPIOs are not
  requested.
- A private D-Bus daemon and fake storage/logind services exercise the actual
  sd-bus client, ObjectManager metadata, mount/unmount method signatures,
  unavailable/invalid objects, storage property notifications and suspend
  handling. No system storage is mounted or unmounted.
- The application runs headless against ALSA's software `null` PCM, with an
  intentionally missing system bus. Tests exercise inventory, audition,
  gain/monitor validation, raw MIDI rejection, surface diagnostics, remote
  stop and socket cleanup. No physical audio/display/MIDI devices are opened.
- Installation/recovery tests mock SSH and file transfers, verify stock
  identity checks and rollback, and execute the recovery script against a
  fake service manager to confirm it suppresses restarts during shutdown.
- Public reference archives are checksum-verified against the hashes in
  `docs/HARDWARE.md`; the source/privacy check excludes ignored paths,
  binary data, credentials and detected private identifiers.

## Hardware status and boundaries

The hardware-base build was installed in RAM and started successfully on the
MPC. Playback and capture were active with zero reported underruns at startup.
The owner found that the original sequential pad-order assumption was wrong.
A live capture of pads 1–16 in printed-number order established the explicit
channel-10 map now recorded in `src/surface/pads.h`; raw captures remain ignored.
The corrected note order still needs the owner's physical confirmation after
installation. A guided on-screen capture then measured all 24 surface buttons
(including wheel Push), the main volume, K1–K3, wheel and fader, plus the power
key. Each step was recorded with no lost packets; the owner performed every
gesture on the device. Results are in `src/surface/controls.h` and `docs/HARDWARE.md`.
The main volume knob changed no ALSA mixer control. Tracing the stock app's
startup writes with a temporary libasound uprobe identified the position
request; replaying it from sample-base returned K1 18, K2 33, K3 39, main
volume 22 and fader 71, matching the positions last reported by movement.
The stock trace also identified the LED messages. A guided probe lit each
button LED with values 1–4 while the owner tapped a matching colour pad
(68 answers, no lost messages), then pad, fader and pad-bank lights were
checked by eye. The installed build's `led` commands then lit pads, fader,
a bank light and three buttons exactly as requested. Results are in
`src/surface/leds.h` and `docs/HARDWARE.md`. The
probe exposed a surface output limit (7 queued messages, one written per
5 ms pass) that dropped 167 messages; the queue now holds 64 and drains fully.
Source references and offline tests do not qualify every
electrical route, latency, sound or device interoperability.

Physical pad/knob/button response, LED brightness steps, listening, microphone/line
selection, headphone/speaker behavior, USB and TRS MIDI, USB PCM formats,
backlight changes, sync jacks, storage removal, suspend/resume, wheel return,
crash restoration and sustained full-polyphony/underrun behavior require
hardware confirmation. The pad and control maps are measured, but the
on-screen control names and main-volume level have not yet been confirmed by
looking and listening. The public reference establishes the underlying
transports, not the complete surface microcontroller protocol.

The base drives only measured LED messages and does not invent
calibration or battery commands. It reports available supply telemetry
without inventing a battery percentage. Sync output is an explicit level
interface, not a qualified musical clock generator. USB PCM selection does
not implement a simultaneous USB/I2S audio bridge or resampler.

The input recorder has not yet run on the MPC. Line, microphone and USB-C
levels, the L/R jack assignment, whether 1.3.0.12's USB gadget configuration
exposes the UAC2 function while sample-base runs, the host's chosen format and
long-session clock drift all need hardware confirmation. Saving takes,
project storage, presets and sequencing remain application work.
The installer remains RAM-only; reboot removes the session setup and uses
the existing startup configuration. Hardware mixer/backlight restoration is
best effort on normal exit; crash recovery restores the stock service.
