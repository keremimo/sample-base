# Validation — 9 November 2026

Target: MPC Sample AC50, firmware 1.3.0.12. Standalone JUCE 9.0.5 application
with reusable hardware interfaces and a built-in sine/triangle demo synth.

## Automated offline checks

- Linux ARM64 build passes with application sources using
  `-Wall -Wextra -Werror`. The build renders a two-second stereo WAV and the
  320 × 240 JUCE preview. Audio peak: `0.0228731`; RMS: `0.00846634`;
  final-half-second RMS: `0`.
- Thirty-four regression tests pass on Linux ARM64. On macOS, twenty-four
  portable tests pass and ten Linux-specific tests are skipped. The build
  also renders the recorder, light show, pad touch, sequencer and system page
  previews `build/preview-recorder.png`, `build/preview-lights.png`,
  `build/preview-pads.png`, `build/preview-seq.png` and
  `build/preview-system.png`.
- Recorder fixtures cover L + R / L / R selection, mono microphone mixing,
  S16_LE, S24_3LE and S32_LE decoding and rounded, clipped encoding,
  44.1/48/32 kHz linear rate conversion both ways, a trimmed conversion
  without a jump, the drift trim's gain and limits and a simulated 150 ppm
  drift settling 75 frames above target, ring priming, underrun re-priming, overflow and flush, takes
  refused without an input, record/finish/play/stop, playback mixing,
  route-change and full-buffer endings and stale overview peaks.
- A fake `UAC2_Gadget` card in front of ALSA's null PCM verifies discovery by
  driver name, waiting without opening while `Capture Rate` is 0, opening at
  the host's rate, conversion into the audio ring, reopening on a rate change
  and closing when the host stops. The same fake card checks the output
  thread: nothing opened or buffered while `Playback Rate` is 0, opening the
  playback PCM at the host's rate, every written frame silent, exact or a
  symmetric ramp between them, reopening on a rate change, closing when the
  host stops, nothing buffered after `stop()`, and a named PCM without a
  rate control opening at 44.1 kHz. The headless app test drives
  `usb-output`, `usb-output off|on` and `usb_output` in `status`; the
  starter runs with `--usb-output off`. The headless app test drives `source`,
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
- Sequencer fixtures check the default groove, step toggling and clamping,
  mutes and clearing; tap tempo averaging, its two-second reset and clamping
  to 40–240 BPM; the clock at 120 BPM putting step 5 on frame 22050, a full
  bar playing each step once, a tempo change from the next step, Play while
  playing, Stop with voices ringing out and Play from step 1; live hits
  rounding to the nearest step and a hit written ahead sounding once, not
  twice; every drum sounding within range, panned to its side, quieter when
  softer, silent within three seconds and at velocity 0; the closed hat
  cutting the open hat; the groove at full volume staying below 0.9; and
  eight distinct track colours. The headless app test drives `page seq`,
  `seq tempo`, `seq step`, `seq mute`, `seq play`, `seq stop`, `seq reset`
  and their errors, and waits for the audio thread to play the kick and a
  snare written in, still playing on another page.
- System page fixtures check panel stepping both ways, the CPU meter's
  first sample, loads between samples and a restart when the CPU count
  changes, the 24-second history clamped to 0–100 %, thermal zones found by
  type, the supply that reports a voltage, memory and storage shares that
  never pass full, and every piece of text the page shows (sizes, volts,
  degrees, rates, drift, running time, jacks, the power key and USB states).
  The headless app test checks that nothing is read before `page system`,
  then waits for two `/proc/stat` samples, reads loads, memory, unknown
  jacks, both USB-C directions, the power key and storage from `system`,
  drives `system next|previous|PANEL` and its errors, and checks that reads
  stop once the page closes. The USB-C fixture checks `stats()` in both
  directions and the MIDI fixture the typed port list, dropped sends and a
  failed port leaving it.
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
  Reading a jack or switch by name returns its value, and -1 for a missing
  or non-boolean control or without the card.
- Mock ALSA control events verify jack detection: both jacks reported once
  as initial, in order; no reads while nothing is queued; one report for a
  plug and one for an unplug, however many events queued with it; nothing
  for an event that changes no jack; a vanished control going unknown and
  returning as initial, not as a plug; a lost card closing and reopening
  only after a second; once-a-second re-reads when the card refuses events;
  and the `jacks` JSON. The headless app test reads `jacks` and the
  `headphones=-1 line_out=-1` status fields without an I2S card.
- Injected sysfs/proc fixtures cover power, missing battery capacity,
  backlight bounds/restoration, USB state, temperatures and mount decoding,
  including spaces in mount paths and read-only mounts. The `/proc` reader
  sums CPU time per core without counting guest time twice, accepts short
  lines, computes loads between samples (none when no time passed or the
  counters went back) and reads `MemAvailable`, falling back to `MemFree`.
  The typed readers return the supply's type, `online` and voltage, every
  thermal zone (not cooling devices), the backlight, the USB controller
  state and each mounted device once, bind mounts left out, with the space
  `statvfs()` reports; on a missing root they return nothing.
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
  The starter app runs on the same runtime with no demo code: `status`
  carries only the runtime's fields, `audition` reaches its pad hook and
  updates the held-pad diagnostics, demo commands are refused and an unknown
  `--app` fails to start.
- A layout test keeps the source layers apart: only `main.cpp` sits in
  `src/`, every include names its folder and exists, and each layer includes
  only the layers below it. Light fixtures check that the runtime's exit
  release turns off exactly the lights an app set. Unit generation passes
  `--app` through and rejects names that could alter the systemd unit.
- Moving to the layered layout left every screen unchanged: the synth,
  prompt, recorder, pad touch and three light show previews rendered
  pixel-identical before and after.
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
USB-C output was then installed and recorded on a Mac with
`sox -t coreaudio "MPC Sample Audio" -r 48000 -c 2 -b 32 out.wav` while the
sequencer played: `running` at 48 kHz, stereo, 32-bit, 92 seconds with no
ring underruns or overflows and no PCM xruns, the drift trim settling between
−15 and −48 ppm, and nothing buffered or overflowed after the Mac stopped
recording. The recording's only silence was the main volume knob turned to
0 by hand for 178 ms, which the surface log shows and the output follows.
Only macOS has recorded from it so far, and nobody has listened critically.
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
project storage, presets and sequences longer than one bar remain
application work.

The system page ran on the MPC from a RAM install, opened remotely with
`page system` and stepped through its four panels with `system PANEL`; each
device screenshot matched its preview's layout. It read a total CPU load of
3 % (four cores, 2–6 % each), 233 MB of 1.9 GB memory used, `cpu-thermal`
38.9 °C and `gpu-thermal` 37.2 °C, `mains-power` (Mains, online) at
3.592 V, backlight 6 of 6, the power key with no presses, UDC `configured`,
USB-C in off and out waiting with no computer recording, the Control
Surface, External MIDI and `f_midi` ports open both ways with no drops,
headphones plugged in and line out empty, and five mounts: `/` and
`/content` read-only and full, `/factory`, `/data` and a 60 GB SD card. No
playback or capture underruns occurred while it read. The Knob FX button,
its light and wheel stepping use the same paths as the other pages but have
not yet been pressed and looked at on the device.

Jack detection ran on the MPC from a RAM install. The I2S card accepted the
event subscription (`events` true) and both jacks started empty, as the
mixer read them. With a guided on-screen prompt and `jacks` read every
250 ms over one `--ctl-stream` session, headphones were plugged in and
pulled out, then line out. Each of the four moves showed up once, with the
right jack and state, and nothing else changed: no contact bounce got
through the kernel's debounce, and the other jack never moved. Both ended at
two changes, `status` read `headphones=0 line_out=0`, and no playback or
capture underruns occurred. What reaches the ears on each jack, and the
speaker while they are plugged, was not checked.

A second guided run checked the two line-out jacks, with each step
confirmed by pad 1. With every cable out, line out read 0. One cable in L
alone read 1; moved to R alone it went 0, then 1 as it went in; adding a
second cable in L kept it at 1; pulling both brought it back to 0 in one
change. Either jack alone trips the single line-out detect line, and
headphones never moved during the run.

The step sequencer ran on the MPC from a RAM install. A four-second remote
run at 110 BPM played the default groove on schedule (9 kicks, 4 claps, 17
closed and 8 open hats) with no playback or capture underruns and no dropped
surface messages, and the device screenshot matched the preview. The owner
then played it on the device and found it fine.
The installer remains RAM-only; reboot removes the session setup and uses
the existing startup configuration. Hardware mixer/backlight restoration is
best effort on normal exit; crash recovery restores the stock service.
