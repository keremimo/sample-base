# sample-base

Standalone MPC Sample AC50 1.3.0.12 application. JUCE draws into an image;
Linux DRM presents it, ALSA owns audio and raw MIDI, and a built-in beep/boop
synth supplies audio.
No stock application injection, offsets, copied firmware, or proprietary assets.

Keep downloaded sources, binaries, captures, credentials and device data in
ignored `build/` or `.local/`. Preserve dependency attribution and licences.
Use four-space indentation, bounded audio/MIDI queues and no allocation or
file I/O in rendering callbacks. Keep screen work off the audio thread.

Keep `src/main.cpp` to picking the app and mode dispatch. `src/` is in
layers (README "Source layout", docs/APPS.md): `surface/` and `dsp/`
(portable headers), `drivers/` (one per device), `runtime/` (hosts an
`App`), then the apps `demos/` and `starter/`. Each include names its
folder; a layer includes only the layers before it, and apps never include
each other (`tests/test_layout.py`). One responsibility per file; each demo
has its own folder with `<demo>.h` (model), `<demo>_demo` (buttons, lights,
commands, audio) and `<demo>_page.cpp` (screen). Anything every app needs
belongs in the runtime, not in a demo.

Run `./build.sh` and `python3 -m unittest discover -s tests -v`.
`python3 device.py start` installs to RAM and switches apps; `stock` returns
to the existing stock service, including its existing plugin configuration.
Requested installs/restarts are authorized without another save confirmation.
Always retain crash, remote-stop and reboot recovery. Never overwrite mapped
binaries. Distinguish device automation from physical controls and listening.

Commit all outstanding source changes at completion. Use Conventional Commits.

Hardware interfaces are documented in `docs/HARDWARE.md`, using the pinned
inMusic public source release. Keep driver-owned power/mute/reset GPIOs in
Linux; only explicitly requested sync-line access uses GPIO. Keep MIDI
polling separate from slow platform/service requests. The MIDI-to-audio queue
has one producer. Raw MIDI/SysEx output and mixer/input changes are opt-in,
except the measured surface position request sent at startup; LEDs change
only when an application sets them through `SurfaceLights`. Never invent
surface LED/calibration opcodes or battery percentages. Hardware
reference downloads are optional and stay in ignored `.local/`.
