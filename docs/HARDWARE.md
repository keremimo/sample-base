# MPC Sample platform base

This is an independent userspace application base for the MPC Sample AC50.
Applications reuse the interfaces in `src/`; the included beep/boop app is a
small example. Drivers, regulators, codec sequencing and hardware ownership
remain with Linux.

## Reference and reproducibility

Reference: inMusic's [MPC Sample 1.3.0 open-source release](https://github.com/inmusic-software/az0x-open-source/tree/mpc-sample/1.3.0),
pinned to commit `d09cedcdce9d48edda975be31d0e4b88ba67eb25`.
The release uses Git LFS source archives. Download the relevant public sources
and selected reference files into ignored local storage with:

```sh
python3 tools/hardware_reference.py
```

This helper is optional, does not install anything and is not a build dependency.
It verifies these archive SHA-256 hashes before extracting:

| Public package | SHA-256 |
| --- | --- |
| `linux-imb-6.12.60+git-r0` | `9d6999d9f69f5afd396a634facadc9219dcb85407264273d912355dd6db38f9b` |
| `edisksd-0.19-r0` | `ece9cd1f6e04333dd016499e9f7bf0b65e878af45f3d2a463959212468acc80f` |

The board wiring comes from `arch/arm64/boot/dts/rockchip/rk3566-az07-ac50.dts`
and `rk3566-az07.dtsi` inside the Linux archive. Other useful references are
`sound/drivers/serial-generic.c`, `sound/soc/codecs/ak4558.c`,
`sound/soc/codecs/simple-mute.c`, `sound/soc/generic/simple-card-utils.c`,
`drivers/gpu/drm/tiny/st7796s.c` and
`drivers/power/supply/generic-adc-mains.c`. Storage interfaces come from
`edisksd`'s `src/dbus.h`, `src/main.c` and `src/filesystem.c`.

Only public interface facts are used here. No downloaded driver source,
firmware, private device data, factory banks or captures are committed.
The kernel sources remain under their own upstream licences.

## Interfaces supplied by the base

| Area | Public wiring | Application interface |
| --- | --- | --- |
| Audio output and capture | `I2S` card, AK4558 codec, stereo, 44.1 kHz | `AudioDevice` gives a fixed 128-frame stereo S32 callback with input and output blocks; partial I/O and underruns are handled outside the callback |
| Microphone, line input, speaker and jacks | DAPM pin switches `Builtin Microphone`, `TRS`, `Builtin Speaker`; headphone/line-out detection | `HardwareMixer` lists live ALSA controls, types, ranges, enum labels and jack values; validates changes and restores changed values on normal exit |
| Surface inputs and outputs | UART3, 4,000,000 baud, `Control Surface` raw MIDI | `HardwareMidi` receives all channel, pressure, system-common, realtime and bounded SysEx messages; queues complete outbound messages |
| External MIDI | UART5, 31,250 baud, `External MIDI` raw MIDI | The same discovered bidirectional MIDI interface; incoming channel messages play the demo synth at their native MIDI pitches |
| USB host MIDI | Kernel USB audio/MIDI driver | Input/output subdevices are discovered and reopened after hotplug; disconnect and read failure panic the demo synth |
| USB audio | Kernel ALSA PCM devices | Enumerated PCM endpoints can be selected with `--audio-output` and `--audio-input`; no fixed USB card number is assumed |
| Display | SPI3 ST7796S-compatible panel, 320 × 240, rotation 270 | `Display` finds the connected panel across DRM cards, presents JUCE software images and attempts to restore the previous CRTC on release |
| Backlight | PWM4, seven brightness indices, maximum index 6 | `Platform` discovers the actual sysfs range, validates explicit changes and restores its original brightness on normal exit |
| Power | `mains-power`, ADC supply voltage, kernel power handling | `Platform` reads actual power-supply attributes; `SystemServices` observes logind sleep/shutdown notifications and requests a clean app exit |
| SD/eMMC/removable storage | Kernel block devices and the existing `edisksd` service | Mounted paths, read-only status and free space; D-Bus volume metadata and explicit mount/unmount requests by service object path |
| Sync input/output | GPIO lines named `CV_CLK_IN` / `CV_CLK_OUT` | Opt-in `ClockIo`: timestamped rising/falling input edges, loss counts and explicit output levels; kernel-owned lines are never taken over |
| USB device connection | Existing USB device controller and gadget configuration | Read-only UDC connection state; the app preserves the existing USB networking/gadget setup |
| Thermal/network status | Kernel thermal zones and network interfaces | Read-only temperatures and interface state; no radio, network or thermal policy changes |
| Session recovery | Existing stock systemd service | RAM-only application and recovery script; wheel Push, remote stop, failure and crash restore the existing service; shutdown does not restart it |

## Threading and extension points

`AudioDevice::Render` is the audio extension point. It receives exactly 128
stereo input frames and writes exactly 128 stereo output frames. Capture
failure supplies zeros while playback continues. The callback must remain
bounded: no allocation, file access, blocking locks or service calls.
The demo's synth and software gain/monitoring are separate from the hardware
adapter, so another application can replace the callback.

`HardwareMidi::Receive` is the input extension point. It receives the source
port identity and a complete `MidiPacket`, including its MIDI channel and
message length, plus monotonic receipt timestamps. These are userspace receipt
times, not physical MIDI-wire timestamps. Surface messages and musical MIDI are kept separate. Device
addresses, input/output direction and subdevices are discovered from ALSA,
not inferred from card order. The surface is required for normal device
operation; `--no-surface` is available for host/headless tests.

The demo has separate audio, MIDI, display and service loops. A single MIDI
thread produces the audio queue; remote audition requests are handed to that
thread rather than introducing another producer. MIDI output uses bounded
queues and preserves short-write offsets. Slow storage and mixer requests
run on the service thread, so they do not delay pad releases or audio.

Surface diagnostics cross another bounded queue. A slow service operation can
overflow that diagnostic queue; its state is reset and an overflow is reported.
This does not drop the separately routed audio MIDI events. The diagnostic
state shows the last value for each note/CC index; applications needing
per-channel surface state should use the complete packet callback directly.

No MIDI thru route is created automatically. The demo consumes incoming
musical MIDI and exposes explicit output; applications can implement their
own routing without creating an accidental feedback loop.

## Inspect and control

`build/sample-base --hardware` prints JSON without opening PCM streams or
raw MIDI endpoints. It reads platform, mixer, PCM/MIDI inventory and available
storage-service metadata. It can also run while another audio app is active.

For a running sample-base session, the USB/SSH helper forwards commands:

```sh
python3 device.py ctl hardware
python3 device.py ctl surface
python3 device.py ctl midi
python3 device.py ctl mixer
python3 device.py ctl storage
python3 device.py ctl input line
python3 device.py ctl monitor 0
python3 device.py ctl gain 75
python3 device.py ctl brightness 5
python3 device.py ctl clock
```

Monitoring is off by default. `input off`, `input mic` and `input line` select
physical capture routes through the named ALSA switches. The input profile
first disables both routes; it never enables microphone and line together.
Generic `mixer-set ID VALUE...` uses the live control ID, exact channel count
and range reported by `mixer`, including enum indices. Read-only controls
cannot be written. Hardware control restoration is best effort on normal
exit; crash recovery returns ownership to the existing stock service.

`midi-send ADDRESS HEX...` queues one complete message to an address returned
by `midi`. For example, `midi-send hw:2,0,0 90 3c 64` sends C4 only if that
exact discovered port is currently available; use the actual address from
your inventory. `surface` reports the last raw message, note, pressure and CC
state. Non-pad buttons, knobs and faders are available through their raw
messages without assigning unverified product-specific labels.

`storage-mount OBJECT` and `storage-unmount OBJECT` use the `object` returned
by `storage`, not a disk path or shell command. Requests go to
`io.github.inmusicdev.edisksd.Filesystem` with the published `a{sv}` options
signature. Object discovery uses the service's ObjectManager at `/`.
Storage hotplug/property signals increment a generation counter, so applications
can refresh their view. No card is mounted or unmounted automatically. Timeouts are bounded; a timed
out mount request may still finish in the daemon, so refresh storage state
before retrying it.

`clock-in on|off` opts into GPIO input edge reporting. `clock-out 0|1` opts
into ownership of the dedicated output and sets its level; normal cleanup
returns it low and releases the line. These commands do not change the
undocumented `CV_SEL` or `CVCLK_RST` mux/reset lines. Event timestamps are
kernel monotonic nanoseconds. The service loop is not a musical clock
scheduler, and output timing/electrical routing requires hardware validation.

`--audio-output NAME --audio-input NAME|off` selects another ALSA PCM. This
base currently requires native 44.1 kHz, stereo S32, 128-frame periods and
at least two periods of buffering. Unsupported playback settings fail
explicitly; optional capture failure is reported by `capture_active=0`.
There is no implicit resampling or simultaneous USB/I2S audio bridge.

## Deliberate hardware boundaries

The public serial driver transports bytes; it does not define the surface
microcontroller's LED colours, pad calibration, battery commands or every
button/encoder number. Bidirectional transport and full packet callbacks are
provided for implementing those protocols. No guessed LED SysEx or calibration
commands are sent at startup. The known demo pad map and wheel Push behavior
are retained from the initial proof of concept, separately from the driver
reference.

The AC50 source describes supply voltage, not a battery percentage gauge.
Only attributes actually present are reported. In particular, missing battery
capacity is not displayed as zero and supply `online` is not treated as a
battery shutdown threshold.

Codec reset, speaker/mic power, mute delays, headphone detection and regulators
belong to their kernel drivers and ALSA routing. The application does not
write those GPIOs or codec registers directly. It does not rebuild the kernel,
reconfigure the USB gadget or replace system services.

All new hardware-facing behavior still requires physical qualification.
See [VALIDATION.md](VALIDATION.md) for the offline tests and remaining checks.
