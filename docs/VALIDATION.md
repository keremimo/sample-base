# Validation — 9 November 2026

Target: MPC Sample AC50, firmware 1.3.0.12. Standalone JUCE 9.0.5 application
with reusable hardware interfaces and a built-in sine/triangle demo synth.

## Automated offline checks

- Linux ARM64 build passes with application sources using
  `-Wall -Wextra -Werror`. The build renders a two-second stereo WAV and the
  320 × 240 JUCE preview. Audio peak: `0.0228731`; RMS: `0.00846634`;
  final-half-second RMS: `0`.
- Sixteen regression tests pass on Linux ARM64. On macOS, nine portable
  tests pass and seven Linux-specific tests are skipped.
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
channel-10 map now recorded in `src/pads.h`; raw captures remain ignored.
The corrected note order still needs the owner's physical confirmation after
installation. Source references and offline tests do not qualify every
electrical route, latency, sound or device interoperability.

Physical pad/knob/button response, LED protocols, listening, microphone/line
selection, headphone/speaker behavior, USB and TRS MIDI, USB PCM formats,
backlight changes, sync jacks, storage removal, suspend/resume, wheel return,
crash restoration and sustained full-polyphony/underrun behavior require
hardware confirmation. The pad map is now measured; wheel Push behavior comes
from the initial
proof of concept. The public reference establishes the underlying transports,
not the complete surface microcontroller protocol.

The base supplies raw surface output but does not invent LED colour,
calibration or battery commands. It reports available supply telemetry
without inventing a battery percentage. Sync output is an explicit level
interface, not a qualified musical clock generator. USB PCM selection does
not implement a simultaneous USB/I2S audio bridge or resampler.

Recording, project storage, presets and sequencing remain application work.
The installer remains RAM-only; reboot removes the session setup and uses
the existing startup configuration. Hardware mixer/backlight restoration is
best effort on normal exit; crash recovery restores the stock service.
