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
state. Measured button, knob, wheel and fader names are in `src/controls.h`.

`surface-sync` asks the surface for the current main volume, K1–K3 and
fader positions. `surface-log [SEQUENCE]` returns up to 256 retained surface packets newer
than the sequence, oldest first, with receipt timestamps and a `next` cursor;
the last 1,024 packets are retained. `power` reports the power key.
`prompt HEADER|TITLE|DETAIL|STATUS|FOOTER` replaces the instrument screen
with instructions until an empty `prompt` clears it. `push-exit on|off`
controls the wheel Push shortcut; remote stop, the session deadline and crash
recovery are unaffected. `sample-base --ctl-stream` reads one command per
stdin line and prints one reply per line, so one SSH session can drive an
interactive tool.

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
provided for implementing those protocols. LED messages are sent only after
an application or `led` command sets a light; no guessed LED SysEx or
calibration commands are sent. The physical pad map was measured on the device
by tapping pads 1–16 in
printed-number order. Their channel-10 note numbers are:
`37, 36, 42, 82, 40, 38, 46, 44, 48, 47, 45, 43, 49, 55, 51, 53`.
`src/pads.h` is the shared map for routing and screen labels; pad 1 plays C4,
pad 2 D4, through pad 16 D6. Note-off, velocity-zero release and polyphonic
pressure use the same physical map. The earlier sequential 36–51 assumption
was incorrect. This changes note routing, not sensor sensitivity or factory
calibration.

### Buttons, knobs, wheel, fader and power

`tools/capture_controls.py` measured every other control with a guided
on-screen capture: pads 1, 2 and 3 move to the next step, redo it or go back,
and every packet in between is read losslessly through `surface-log`.
`src/controls.h` is the shared map; `surface_input()` decodes an event into a
named button or control. All of these use MIDI channel 1, so a channel-1 note
whose number matches a pad note is a button, not a pad.

| Control | Message |
| --- | --- |
| Main volume | CC 19, absolute 0–127 |
| K1 / K2 / K3 | CC 16 / 17 / 18, absolute 0–127 |
| Fader | CC 20, absolute 0–127 |
| Wheel | CC 100, relative: 1 = one step right, 127 = one step left |
| Wheel push | Note 111 |
| Left: Sample, SEQ, Pad FX, Knob FX | Notes 6, 7, 1, 0 |
| Left: Shift, Pad Bank, Erase, Note Repeat | Notes 49, 35, 9, 11 |
| Above screen: left, middle, right | Notes 12, 13, 14 |
| Right: Chop, Mute, Loop, 16 Levels | Notes 75, 43, 74, 40 |
| Right: Sample Select, Tap Tempo, −, + | Notes 36, 53, 55, 54 |
| Right: Sample Record, Seq Record, Stop, Play | Notes 73, 80, 81, 82 |
| Power | `KEY_POWER` on the `rk805 pwrkey` evdev device, not MIDI |

Buttons send note-on velocity 127 when pressed and note-off velocity 0 when
released. The knobs and fader send absolute positions when they move; the
wheel sent one message per detent in the capture, and larger relative steps
are decoded as signed 7-bit values. No touch-sensing messages were observed.
Main volume changed no ALSA mixer control: it is surface MIDI only, so the
demo maps it to software output gain with a squared taper.

At startup sample-base sends the position request `F0 47 7F 5C 42 00 00 F7`
(inMusic SysEx command `0x42`, product `0x5C`, empty payload). The surface
answers at once with CC 19, 16, 17, 18 and 20 at their current physical
positions, then wheel CC 100 value 0, so main volume and the surface state
match the hardware before anything moves. `surface-sync` repeats the request.
The reply is applied normally but is not shown as the last control touched.

The request was found by tracing the stock app's `snd_rawmidi_write()` calls
during one startup, using a temporary uprobe on the device's libasound that
was removed afterwards; the stock app and its files were not modified. Stock
sent only two SysEx commands to the surface: `0x42` once at startup and
`0x65` for pad colours (`F0 47 7F 5C 65 00 04 PAD R G B F7`). Button LEDs
are channel-1 CCs numbered like the button notes, with small state values;
Tap Tempo's LED alternates between 1 and 2 at the tempo. Stock's
separate Identity Request comes from a system `amidi` call; the surface
replies with manufacturer `47`, family `5C 00`, member `19 01`.

### Lights

The stock trace gave the message types; `tools/probe_leds.py` measured what
each value shows. It lights one button at a time and the owner taps the pad
whose colour matches, so every answer is recorded rather than remembered.
`src/leds.h` is the shared map.

| Light | Message | Values |
| --- | --- | --- |
| Pads 1–16 | `F0 47 7F 5C 65 00 04 I R G B F7`, I = printed pad number − 1 | R, G, B 0–127 |
| Fader light | Same SysEx with I = 16 | R, G, B 0–127; stock leaves it at 101, 101, 101 |
| Pad bank A–H | Channel-1 CC 57–64 | 0 off; 20 and 127 both looked fully on, so the base sends 127 |
| White/red buttons | Channel-1 CC = button note | 0 off, 1 dim white, 2 white, 3 dim red, 4 red with some white showing |
| Pad Bank, Note Repeat, Stop (white), Sample Record, Seq Record (red), Play (green) | Channel-1 CC = button note | 0 off, 1 dim, 2 full; 3 and 4 also showed full |

The white/red buttons are Sample, SEQ, Pad FX, Knob FX, Erase, Chop, Mute,
Loop, 16 Levels, Sample Select and Tap Tempo. Shift, Minus, Plus, the three
buttons above the screen and wheel Push have no light. Stock also clears CC
83–86 at startup. No AC50 light is known for them and they were not probed,
so the base does not send them.

`SurfaceLights` keeps the requested state, which any thread may change, and
the MIDI thread sends only the lights that changed, at most 48 messages per
5 ms pass. Nothing is sent for a light the application has never set, so
startup leaves the stock app's last lights in place. The control commands
are `led pad 1-16 R G B`, `led fader R G B`, `led bank A-H on|off`,
`led button NAME off|dim|bright|red-dim|red-bright` (names as in
`src/controls.h`, ignoring case and punctuation, so `tap-tempo` or `TapTempo`)
and `led off`.

Each output port queues 64 complete messages, and every pass writes until the
driver buffer is full. Before this, a port held 7 messages and wrote one per
pass, so a burst of LED messages lost all but a few; `midi` reports any
`send_drops`.

The power button is read from `/dev/input/eventN` by device name, shared and
never grabbed. On 1.3.0.12 logind is not running and the vendor power-button
udev rule matches only a `gpio-keys` device, so nothing else acts on a short
press while sample-base runs. A long hold may still trigger the power
management chip's own hardware power-off. `ctl power` reports availability,
press count, current state and a monotonic timestamp; the demo only displays
the press and never shuts down. During `capture_controls.py`, `push-exit off`
temporarily disables the wheel Push shortcut so it can be measured.

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
