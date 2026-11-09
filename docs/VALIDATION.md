# Validation — 9 November 2026

Target: MPC Sample AC50, firmware 1.3.0.12. App: standalone JUCE 9.0.5,
with the built-in sine/triangle beep/boop synth.

## Automated offline checks

- Linux ARM64 build, with application sources using `-Wall -Wextra -Werror`.
- Seven portable regression tests pass on macOS and Linux ARM64. Native C++ fixtures
  cover MIDI running status, fragmented reads, realtime interleaving,
  discarded SysEx, single-data-byte messages, pad identification, queue
  wrap-around and overflow notification, plus synth pitch, velocity,
  polyphony, voice stealing, note releases and panic recovery.
- Python tests cover device identity, recovery setup, rejected paths/durations,
  single-binary staging and rollback after startup failure. SSH and file
  transfer calls are mocked; these tests do not contact a device.
- The app's audio self-test renders a two-second stereo WAV at 44.1 kHz.
  A note is released after approximately half a second. The test requires
  nonzero sound and a silent final half-second.
  Peak: `0.0228731`; RMS: `0.00846634`; tail RMS: `0`.
- JUCE renders the offline 320 × 240 preview, including all 16 pad labels
  and the return-to-stock hint. Generated audio and images stay ignored.
- The staged source privacy check excludes ignored paths, binary data,
  credentials and detected private identifiers.

## Hardware status and boundaries

This built-in synth version has **not** been installed or tested on the MPC.
No device operations were performed during publication. Earlier proof-of-concept
hardware checks used a different sound engine and do not qualify this synth.
Physical pad response, listening, wheel return, audio underruns, full-polyphony
stress, latency, battery/suspend behavior and recovery need confirmation on
this version.

No recording, project saving, presets UI, external MIDI or USB audio bridge
is implemented. The installer is RAM-only; a power cycle removes its session
setup. The helper retains stock-identity checks and independent systemd
restoration after normal exit or a crash.
