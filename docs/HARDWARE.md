# MPC Sample platform base

This is an independent userspace application base for the MPC Sample AC50.
Applications implement `App` ([src/runtime/app.h](../src/runtime/app.h);
see [APPS.md](APPS.md)) on top of the drivers in `src/drivers/`; the demos
in `src/demos/` and the starter in `src/starter/` are examples. Kernel
drivers, regulators, codec sequencing and hardware ownership remain with
Linux.

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
| USB audio | Kernel ALSA PCM devices; USB-C device audio from the `UAC2_Gadget` card (`CONFIG_USB_CONFIGFS_F_UAC2`) | Enumerated PCM endpoints can be selected with `--audio-output` and `--audio-input`; `UsbAudioInput` finds the gadget by driver name, waits for the host and delivers its capture stream at 44.1 kHz; no fixed USB card number is assumed |
| Display | SPI3 ST7796S-compatible panel, 320 × 240, rotation 270 | `Display` finds the connected panel across DRM cards, presents JUCE software images and attempts to restore the previous CRTC on release |
| Backlight | PWM4, seven brightness indices, maximum index 6 | `Platform` discovers the actual sysfs range, validates explicit changes and restores its original brightness on normal exit |
| Power | `mains-power`, ADC supply voltage, kernel power handling | `Platform` reads actual power-supply attributes; `SystemServices` observes logind sleep/shutdown notifications and requests a clean app exit |
| SD/eMMC/removable storage | Kernel block devices and the existing `edisksd` service | Mounted paths, read-only status and free space; D-Bus volume metadata and explicit mount/unmount requests by service object path |
| Sync input/output | GPIO lines named `CV_CLK_IN` / `CV_CLK_OUT` | Opt-in `ClockIo`: timestamped rising/falling input edges, loss counts and explicit output levels; kernel-owned lines are never taken over |
| USB device connection | Existing USB device controller and gadget configuration | Read-only UDC connection state; the app preserves the existing USB networking/gadget setup |
| Thermal/network status | Kernel thermal zones and network interfaces | Read-only temperatures and interface state; no radio, network or thermal policy changes |
| Session recovery | Existing stock systemd service | RAM-only application and recovery script; wheel Push, remote stop, failure and crash restore the existing service; shutdown does not restart it |

## Threading and extension points

An application plugs in through `App` (see [APPS.md](APPS.md)), whose
hooks the runtime calls from its audio, MIDI, service and screen threads.
`App::render()` sits on `AudioDevice::Render`. It receives exactly 128
stereo input frames and writes exactly 128 stereo output frames. Capture
failure supplies zeros while playback continues. The callback must remain
bounded: no allocation, file access, blocking locks or service calls.
The demo's synth and software gain/monitoring are separate from the hardware
adapter, so another application replaces them with its own `render()`.

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
state. Measured button, knob, wheel and fader names are in `src/surface/controls.h`.

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
There is no implicit resampling or simultaneous USB/I2S audio bridge; the
recorder's USB-C input is a separate capture stream, described above.

## Input recording showcase

The demo's recorder page (the **Sample** button) records one take of up to
60 seconds from a chosen input and plays it back. Sources come from public
wiring, not measurement:

| Input | Public wiring | How sample-base reads it |
| --- | --- | --- |
| Line in L / R | DAPM widget `TRS` → `Line Input` amplifier → AK4558 `LIN` / `RIN` | I2S capture channels 0 and 1 with `input line` routing |
| Built-in microphone | DAPM widget `Builtin Microphone` → `Internal Mic` amplifier → both `LIN` and `RIN` | I2S capture with `input mic` routing, averaged to mono |
| USB-C | `f_uac2` gadget function; card driver and short name `UAC2_Gadget`, PCM `UAC2 PCM` | The gadget's capture PCM, which carries what the computer plays |

The line and microphone routes share one ADC pair, so only one is enabled at
a time, through the existing exclusive `input` profile. The microphone feeds
both ADC inputs from one widget and is recorded as their average. Line in and
USB-C can record L + R, L only or R only; one chosen channel is copied to both
sides so it plays back centred.

The gadget facts come from the kernel's `drivers/usb/gadget/function/u_audio.c`
and `f_uac2.c` (fetched by `tools/hardware_reference.py`). The capture PCM
offers exactly one sample format, S16_LE, S24_3LE or S32_LE, set by the gadget's
`c_ssize`, and exactly the channel count of `c_chmask`. Its rate is the rate
the host chose; the PCM-interface control `Capture Rate` reports it and reads 0
while the host is not streaming. `UsbAudioInput` therefore opens nothing until
that control is non-zero, reopens when it changes, closes when it returns to 0,
and converts the first two channels to 44.1 kHz stereo float with linear
interpolation on its own thread. An 8,192-frame lock-free ring hands the result
to the audio thread, which waits for 1,024 frames before using it and again
after running dry. The host and the AK4558 have independent clocks; the ring
absorbs drift, and a long session may hear an occasional re-prime. The
gadget's `Capture Pitch 1000000` feedback control is left alone. Whether the
1.3.0.12 gadget configuration includes the UAC2 function at all is not in the
public source; without the card the screen says so and USB-C records silence.
`--usb-input NAME` reads another ALSA capture PCM instead of discovering the
gadget.

Nothing changes until an input is chosen: the routes start off, as before,
and the USB PCM is opened only while USB-C is selected. A recording ends when
its input or channel choice changes, and the route switches only after the
recorder has stopped writing. Recording and playback run inside the audio
callback with no allocation, locking or file I/O; the take lives in RAM and is
not saved. Playback is mixed before the main volume gain.

Controls on the recorder page:

| Control | Action |
| --- | --- |
| Sample | Shows or hides the recorder page |
| Left button above the screen | Input: Line in → Mic → USB-C → Off |
| Middle button above the screen | Channels for Line in and USB-C: L + R → L → R |
| Right button above the screen | Back to the synth page |
| Sample Record | Starts a take, or finishes the one in progress |
| Play | Plays the take from the start; ends a take in progress |
| Stop | Stops recording or playback |

On the synth page the buttons above the screen only open the recorder, since
their labels are not shown. While a remote `prompt` owns the screen (for
example during `capture_controls.py`) the recorder ignores buttons. The demo
sets the Sample, Sample Record, Play and Stop lights at startup and again
whenever the recorder calls for a different light: Sample Record is dim when
an input is chosen and bright while recording, Play dim with a take and
bright while playing, Stop dim while either is running.

The same functions are available remotely:

```sh
python3 device.py ctl source line
python3 device.py ctl channels left
python3 device.py ctl transport record
python3 device.py ctl transport stop
python3 device.py ctl transport play
python3 device.py ctl recorder
python3 device.py ctl page recorder
```

`source off|line|mic|usb` applies the route and starts or stops USB input;
`input off|mic|line` now selects the same source. `channels stereo|left|right`,
`transport record|play|stop` and `page synth|recorder` mirror the buttons.
`recorder` returns JSON with the source, channels, state, take length,
position, capacity, input peaks and USB stream details (state, device, rate,
channels, bits, ring overflows and underruns). `status` also reports `source`,
`channels`, `recorder`, `recorded_frames`, `play_frame`, `takes` and `usb`.

## Step sequencer

The demo's SEQ page (`src/demos/sequencer/`) is a one-bar drum machine: eight
tracks of sixteen 16th-note steps, each track playing one drum made in code.
A step stores the velocity it was entered with. The pattern is a grid of
atomic bytes, so the MIDI, service, screen and audio threads share it with
no lock.

The clock runs inside the audio callback and counts frames, carrying the
fraction, so steps start on the frame they are due and a bar at 120 BPM is
exactly 88,200 frames. A tempo change takes effect from the next step.
Nothing here is a MIDI clock or sync output: the sync jack stays an explicit
level interface, and the sequencer sends no MIDI.

Each drum is one voice per track: kick and toms are a sine with a falling
pitch, snare adds high-passed noise, clap is three bursts of band-passed noise
and a tail, the hats are six detuned square waves through a high-pass filter
and the bell is two through a band-pass. Envelopes fall by a multiply per
sample and filter coefficients are set once per hit. A new hit restarts the
track's voice; a closed hat cuts the open hat short.

A live hit while recording is written onto the nearest step: the playing
step in its first half, the next one in its second. Written ahead of the
playhead, it has already sounded, so that step skips it once rather than
playing it twice a moment later.

| Control | Action |
| --- | --- |
| SEQ | Shows or hides the sequencer page |
| Left button above the screen | Pads: steps or drums |
| Middle button above the screen, Pad Bank | Next track; with Shift, the previous one |
| Right button above the screen | Back to the synth page |
| Pads, step mode | Set the selected track's step at the strike's velocity, or clear it; heard at once while stopped |
| Pads 1–8, drum mode | Play drums A–H and select that track; written in while recording |
| Pads 9–16, drum mode | Mute or unmute tracks A–H |
| Play | Starts from step 1 |
| Stop | Stops the sequencer and the recorder, on any page |
| Seq Record | Arms or disarms recording and turns the pads to drums; from another page it opens the sequencer armed |
| Tap Tempo | Tempo from the last taps' average interval; a pause over two seconds starts again |
| Wheel, Minus, Plus | Tempo ±1 BPM, or ±0.1 with Shift |
| Mute | Mutes or unmutes the selected track |
| Erase | Clears the selected track; with Shift, every track |

In step mode the pads show the selected track's steps in its colour, brighter
the harder they were struck, and the playhead in white (red while recording).
In drum mode pads 1–8 rest dim in their track colours and flash as their drum
sounds, and pads 9–16 show red for a muted track. The bank lights and fader
light show the selected track. Play is bright while playing, Seq Record dim
when armed and bright when recording, Mute red for a muted track and Erase
dim while the track has steps. Stock alternates Tap Tempo's light between
values 1 and 2 at the tempo; here it is dim between beats, bright for the
first 16th of each beat and red on the bar's first. Closing the page turns
these lights off and gives the transport lights back to the recorder. Pads on
the SEQ page do not play the synth; their releases still reach it, so a note
held across a page change ends.

The same functions are available remotely:

```sh
python3 device.py ctl page seq
python3 device.py ctl seq play
python3 device.py ctl seq tempo 92.5
python3 device.py ctl seq step B 5 100
python3 device.py ctl seq mute H on
python3 device.py ctl seq
```

`seq` alone returns JSON with the state, step, tempo, selected track, pad mode,
whether recording is armed and, per track, its drum, mute, hit count and
sixteen step velocities. `seq stop`, `seq clear` (every step and mute) and
`seq reset` (the default groove at 110 BPM) complete the set. `status` also
reports `seq`, `seq_step` (0 when stopped) and `tempo`.

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
`src/surface/pads.h` is the shared map for routing and screen labels; pad 1 plays C4,
pad 2 D4, through pad 16 D6. Note-off, velocity-zero release and polyphonic
pressure use the same physical map. A held pad sends polyphonic pressure on
its own note: 0 straight after the note-on, a new value whenever the
pressure changes, and 0 again just before the note-off. Pressure uses the
full range: in one session on 9 November 2026, pad 5 sent 1,009 pressure
messages with 125 distinct values from 0 to 127, and strike velocities went
as low as 3. The earlier sequential 36–51 assumption
was incorrect. This changes note routing, not sensor sensitivity or factory
calibration.

### Buttons, knobs, wheel, fader and power

`tools/capture_controls.py` measured every other control with a guided
on-screen capture: pads 1, 2 and 3 move to the next step, redo it or go back,
and every packet in between is read losslessly through `surface-log`.
`src/surface/controls.h` is the shared map; `surface_input()` decodes an event into a
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
`src/surface/leds.h` is the shared map.

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
startup leaves the stock app's last lights in place, apart from the four
transport lights the demo's recorder sets and the light show. The control commands
are `led pad 1-16 R G B`, `led fader R G B`, `led bank A-H on|off`,
`led button NAME off|dim|bright|red-dim|red-bright` (names as in
`src/surface/controls.h`, ignoring case and punctuation, so `tap-tempo` or `TapTempo`)
and `led off`.

### Light show

`src/demos/light_show/light_show.h` is a looping LED showcase of ten scenes (48.9 s) built only
from the measured messages above. `light_frame(ms)` is a pure function of the
position in the loop: the MIDI thread requests a frame every 25 ms while the
light page is open, and the screen draws the same frame as its mirror.
Because `SurfaceLights` sends only changes, a frame costs at most 42 messages
and usually far fewer; 16 pad SysEx at 40 frames a second is about 7.7 KB/s
on the 4 Mbaud surface link. Fades square their level, as LED drive is
linear while brightness perception is not.

| Scene | Lights |
| --- | --- |
| Roll call | Banks A–H, the left buttons and fader, pads 1–16, then the right buttons light one by one, all together, then dark |
| Button colours | White/red buttons step dim, bright, dim red, red; single-colour buttons dim and bright; pads and fader match |
| Sixteen colours | Each pad fades in with its own hue |
| Rainbow | Diagonal hues flow across the pads; bank and button chases |
| Breathe | Teal, magenta and amber fades; banks as a level meter |
| Ripple | Blue-violet rings moving out from the pad centre |
| Sparkle | Random pads flash in random colours and fade |
| Comet | A tail round the pad edge; bank lights scan A–H and back |
| Levels | Pad columns bounce green, yellow, red; Tap Tempo blinks at 120 BPM |
| Finale | Four flashes on the beat, a fading rainbow, then dark |

**Pad FX** opens and closes the page. On it, the left and middle buttons above
the screen go to the previous and next scene and the right one returns to the
synth. A held pad glows white over the show and fades back over 400 ms after
release. Closing the page, Sample, Sample Record or Play turns every light off
and gives the transport lights back to the recorder; quitting during the show
also leaves the surface dark. While the page is open an `led` command lasts
only until the next frame. Remotely, `page lights` starts the show,
`lights 1-10|next|previous` jumps to a scene, and `status` reports `page` and
`light_scene` (0 when the page is closed).

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
