# MPC Sample platform base

This is an independent userspace application base for the MPC Sample AC50.
Applications implement `App` ([src/runtime/app.h](../src/runtime/app.h);
see [APPS.md](APPS.md)) on top of the drivers in `src/drivers/`; the demos
in `src/demos/` and the starter in `src/starter/` are examples. Kernel
drivers, regulators, codec sequencing and hardware ownership remain with
Linux.

## Firmware 1.4

Checked on 1.4.0.7 (stock app `MPC Sample 1.4.0`, SHA-256 `2271252f…`,
kernel `6.18.59-imb-2026-10-26-rt9`, az0x 5.0.21). The stock app's Settings
gained a **DEV ENHANCEMENTS (BETA)** page; the base runs on 1.4 with these
differences from 1.3.0.12:

1.4.0.8 (stock app `1.4.0.8 (d87ac8c12e9)`, SHA-256 `473fb726…`, built
13 November 2026, `BUILD_TAG` `jenkins-ACVS-MPC-Sample-SME6-Release-Yocto-114`)
keeps the same kernel, az0x 5.0.21, sound node (44.1 kHz only), gadget,
surface and MIDI ports, SSH and top-of-card `autorun.sh`; the stock app and
the root file system (new dm-verity root hash) changed. The table holds for
both.

| Area | 1.4.0.7 and 1.4.0.8 |
| --- | --- |
| SSH | **Developer Mode** starts the stock `sshd` on port 22. `/data/ssh/sshd_config.d/allow-access.conf` lets root in with no key or password (`AuthenticationMethods none`). |
| Card autorun | The salted `autorun.check` is gone: a signed autorun is now inMusic's `autorun/*.scriptpkg` (`az01-signed-fs`). With Developer Mode on, the stock app (`ScriptRunner`) looks for `autorun.sh` at the top of each drive, asks *The drive … contains an autorun script. Do you want to run it now?*, and on Yes runs `systemd-run --wait --collect --unit autorun-sh --setenv AZ0X_SCRIPT_SRCDIR=<dir> /bin/sh <drive>/autorun.sh` with an empty environment. It asks each time it starts with the card in. [install/top-autorun.sh](../install/top-autorun.sh) is that file: it runs `autorun/autorun.sh`, as 1.3.0.12 does. |
| Kernel modules | LoadPin enforces (`CONFIG_SECURITY_LOADPIN_ENFORCE`, verity root): modules load only from the read-only root file system. `finit_module` from memory is refused (`LoadPin: kernel-module denied obj="/memfd:sample_base_panel"`), so the base's panel driver, webcam function and JPEG switch (`kernel/`) do not load. The panel stays on `st7796s` (landscape writes, the tearing `panel_driver.h` describes); Screen Mirror reports `failed`. The binary carries a build of each for this kernel too (`-6.18.ko`, vermagic `6.18.59-imb-2026-10-26-rt9 SMP preempt_rt aarch64`) and loads the one whose vermagic names the running kernel (`kernel_module::load`); they are refused the same way until LoadPin is off. `CONFIG_MODULE_UNLOAD` is not set, so a loaded module stays until the MPC restarts. |
| USB webcam | `CONFIG_USB_CONFIGFS_F_UVC` is not set: no `uvc` gadget function exists to fall back on. |
| Audio rate | The sound card's `simple-audio-card,cpu` has `system-clock-frequency = 22579200` with `mclk-fs = 512`; the kernel's simple-card then holds every stream at 22579200 / 512 = 44100 Hz. 48 and 96 kHz are refused (`Set 48000 Hz: Invalid argument`) and the runtime opens 44.1 kHz. |
| Gadget | Same `ac50` gadget (`uac2.audio`, `ncm.network`, `midi.midi`); widening for stems works. |
| Secure boot | **Load Untrusted OS** on the same page turns secure boot off, so the MPC starts an operating system inMusic has not verified. Not used; see below. |

### Load Untrusted OS

Read on 1.4.0.7 without switching it on, then against inMusic's
[MPC Sample 1.4.0 open-source release](https://github.com/inmusic-software/az0x-open-source/tree/mpc-sample/1.4.0)
(commit `c879e12d4fa5c334e7dd456cd396179fff9cb946`, 24 November 2026):

| Public package | SHA-256 |
| --- | --- |
| `linux-imb-6.18.59+git-r0` | `2123c75b243f5d544c4cc3589073a589537a267c5557a5dacd8d337c356c447d` |
| `u-boot-imb-1_2026.07-inmusic-20260930-r0` | `eb48188475d2256de89fa136901a9be518cb8db16bed09344e488f7cef01051c` |

Neither has an untrusted mode:

- U-Boot `2026.07-inmusic-20260930`: `board/inmusic/az07` has no switch, and
  `az07.h` still imports only `serial#` and `ethaddr` from `env1`/`env2`.
  The MPC's U-Boot device tree includes `az0x-akai-keys.dtsi` (key `akai-1`,
  `required = "conf"`), and `SPL_FIT_SIGNATURE` selects `FIT_SIGNATURE`, so
  `bootm` refuses a `kernel.fit` whose configuration inMusic has not signed.
  The environment cannot turn this off (`verify` is not imported).
  `fw_setenv bootargs ...` and `fw_setenv loadpin.enforce 0` cannot disable
  LoadPin either: neither variable is imported or added to the kernel command
  line. `loadpin.enforce=0` must reach the kernel at boot.
- Kernel: LoadPin is the upstream code, and no file reads the switch.

What the switch changes is outside the published sources: the Rockchip
loader, OP-TEE, or a U-Boot other than 20260930 (1.4.0.7 had
`2026.07-inmusic-20260909`; both `uboot1` and `uboot2` on the connected
1.4.0.8 unit also identify as `2026.07-inmusic-20260909`, read without
writing the partitions). What it lets the MPC start is therefore still not known.

| Area | 1.4.0.7 |
| --- | --- |
| Setting | Stock app property `Enhancements.UntrustedOS` (settings key `untrusted_os`). Its text: switching it off turns secure boot back on, after which the MPC "may fall back to firmware update mode". |
| Developer flags | The stock app on 1.4.0.7 holds two obfuscated strings, decoded as `dev-mode` and `beta-features`, in code that also uses a plain `imb-` string and links libubootenv. What that code does with them was not traced. No variable made from them has been seen in the U-Boot environment (`/etc/fw_env.config`: `env1` primary, `env2` backup): on 1.4.0.7, and on 1.4.0.8 also after Developer Mode was turned off and on again, it held only `serial#`. |
| eMMC | GPT: `spl1`/`spl2` 480K, `env1`/`env2` 512K, `uboot1`/`uboot2` 4M, `factory` 64M (p7), `boot` 128M (p8), `recovery` 128M (p9), `rootfs` 1G (p10, dm-verity), `content` 4G (p11), `data` (p12). |
| Boot | 1.3.0's and 1.4.0's U-Boot load `kernel.fit` from `boot` (or `recovery`) and run `bootm $loadaddr#conf-rk3566-az07-ac50`. The kernel command line carries the dm-verity table of `rootfs` with its root hash (no `DM_VERITY_VERIFY_ROOTHASH_SIG`), so a new kernel, device tree or command line can keep inMusic's root file system unchanged. Both import only `serial#` and `ethaddr` from `env1`/`env2` and have no untrusted mode. |
| Kernel | `SECURITY_LOADPIN_ENFORCE` and `SECURITY_LOADPIN_VERITY` on, `/usr/lib/az0x/trusted-verity-digests` empty; `MODULE_SIG`, `MODVERSIONS`, `KEXEC`, lockdown and IMA off. `loadpin.enforce=0` on the command line would let unsigned modules load. |
| Base modules | `KERNEL=6.18 tools/build_kernel_modules.sh` builds them for `6.18.59-imb-2026-10-26-rt9` against inMusic's 1.4.0 kernel release with the 1.4 `/proc/config.gz`. The first builds used inMusic's 6.18.45 tree (`engine-os-g2/5.3`) brought to 6.18.59 with kernel.org's stable increments; that tree differs from the 1.4.0 release only in five `.c` files (`seq_midi.c`, `8250_port.c`, three DSI panel drivers) and the `az04`/`az07` defconfigs, and the modules built against the release are byte-identical. Every symbol they need is exported by the 6.18 tree or the stock `videobuf2` modules. The webcam module carries its own `f_uvc` (6.18's in `kernel/uvc-6.18`), so the kernel's missing `F_UVC` does not matter. |
| Recovery | 1.3.0's and 1.4.0's U-Boot: 2 failed starts go to update mode, 4 to the `recovery` partition, 6 to fastboot over USB; the recovery button on a clean start goes to `recovery`. U-Boot also has a rollback generation number (`AZ0X_GENERATION_NUMBER`), so going back to 1.3.0.12 may be refused (not tried). |

On the connected 1.4.0.8 unit, root's
`sysctl -w kernel.loadpin.enforce=0` returned `Invalid argument`; a second
read still returned `1`. The boot log shows `dm-0` read-only and pinning
engaged when PID 1 loads `ipv6.ko`. The running `/proc/config.gz` confirms
`SECURITY_LOADPIN`, `SECURITY_LOADPIN_ENFORCE` and `SECURITY_LOADPIN_VERITY`
on, with `KEXEC`, `KEXEC_FILE`, `DEVMEM`, `MODULE_UNLOAD` and `MODULE_SIG`
off. Root cannot turn off enforcement through the sysctl after this pin;
there is no `/dev/mem` or kexec route in this build. The upstream
[LoadPin documentation](https://docs.kernel.org/admin-guide/LSM/LoadPin.html)
describes the boot argument and the read-only pin restriction.

The smallest own boot image: the stock 1.4 kernel, a device tree whose sound
node has no `system-clock-frequency` (48 and 96 kHz), and `loadpin.enforce=0`,
packed as `kernel.fit` for `boot` only. `recovery`, `spl1`/`spl2` and
`uboot1`/`uboot2` stay stock; the bootloaders sit under the chip's own secure
boot and must not be written. The 1.4 sources do not show what untrusted
mode accepts. The installed 20260909 U-Boot differs from the published
20260930 source; neither the untrusted-mode boot path nor an own boot image
has been tested.

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
`sound/soc/soc-pcm.c` and `sound/soc/soc-dapm.c` (`pmdown_time`, the power-down sequence),
`drivers/gpu/drm/tiny/st7796s.c` and
`drivers/power/supply/generic-adc-mains.c`. Storage interfaces come from
`edisksd`'s `src/dbus.h`, `src/main.c` and `src/filesystem.c`.

Only public interface facts are used here. No downloaded driver source,
firmware, private device data, factory banks or captures are committed.
The kernel sources remain under their own upstream licences.

## Interfaces supplied by the base

| Area | Public wiring | Application interface |
| --- | --- | --- |
| Audio output and capture | `I2S` card, AK4558 codec, stereo, 44.1, 48 or 96 kHz | `AudioDevice` gives a fixed 128-frame stereo S32 callback with input and output blocks; partial I/O and underruns are handled outside the callback; it sets the codec's clocks only while the codec is powered down behind the board's output mute, and keeps the built-in speaker off while the codec powers up ("Opening and closing without a click") |
| Microphone, line input, speaker and jacks | DAPM pin switches `Builtin Microphone`, `TRS`, `Builtin Speaker`; headphone/line-out detection | `HardwareMixer` lists live ALSA controls, types, ranges, enum labels and jack values; validates changes and restores changed values on normal exit |
| Jack detection | `hp-det-gpio` GPIO3_B0 and `out-det-gpio` GPIO3_B1 on the I2S simple-audio-card, debounced by the kernel | `JackDetect` follows the `Headphones Jack` and `Line Out Jack` card controls through ALSA control events; the runtime calls `App::jack()` on every plug and unplug |
| Surface inputs and outputs | UART3, 4,000,000 baud, `Control Surface` raw MIDI | `HardwareMidi` receives all channel, pressure, system-common, realtime and bounded SysEx messages; queues complete outbound messages |
| External MIDI | UART5, 31,250 baud, `External MIDI` raw MIDI | The same discovered bidirectional MIDI interface; incoming channel messages play the demo synth at their native MIDI pitches |
| USB host MIDI | Kernel USB audio/MIDI driver | Input/output subdevices are discovered and reopened after hotplug; disconnect and read failure panic the demo synth |
| USB audio | Kernel ALSA PCM devices; USB-C device audio from the `UAC2_Gadget` card (`CONFIG_USB_CONFIGFS_F_UAC2`) | Enumerated PCM endpoints can be selected with `--audio-output` and `--audio-input`; `UsbAudioInput` finds the gadget by driver name, waits for the host and delivers its capture stream at the device's rate, the host locked to the AK4558 through the gadget's feedback, or trimmed against clock drift without it; `UsbAudioOutput` sends every app's output, and its stems, to a computer recording from the gadget, its packets sized to the AK4558's pace; no fixed USB card number is assumed |
| Display | SPI3 ST7796S-compatible panel (`vitek,vt024k-zn01`, inMusic's `st7796s` tinydrm driver), natively 240 × 320 portrait, rotation 270 (`MADCTL` `MY` \| `MV`), SPI at 25 MHz; no vsync (the mode has no refresh rate, no tearing-effect line, and no read line: every register reads `ff`). The panel refreshes from its memory a gate line (a screen column) at a time, while the stock driver writes a screen row at a time across them, so a moving vertical line written as the refresh passes shows broken in two | `Display` first moves the panel to sample-base's own driver (`kernel/panel/sample_base_panel.c`, after `st7796s.c`: the same init without `MV`, a 240 × 320 portrait mode), built against the MPC's kernel by `tools/build_kernel_modules.sh`, embedded in the binary and loaded from memory (`src/drivers/panel_driver.h`); it binds `spi3.0` by `driver_override` and gives it back to `st7796s` on exit, and `install/restore-stock.sh` does so after a crash. The buffer is then the screen turned, each write runs along the refresh, and nothing on the screen can tear across a line. It pushes JUCE software images from a worker thread, sending only what changed as damage clips (`display_damage::plan`): a whole screen takes about 54 ms, small changes under 5 ms; `screen_clips` counts the clips. On the stock driver (a build without the module, or one that will not load) a narrow tall change, a moving line, goes a column at a time instead. It attempts to restore the previous CRTC on release |
| Backlight | PWM4, seven brightness indices, maximum index 6 | `Platform` discovers the actual sysfs range, validates explicit changes and restores its original brightness on normal exit |
| Power | `mains-power`, ADC supply voltage, kernel power handling | `Platform` reads actual power-supply attributes; `SystemServices` observes logind sleep/shutdown notifications and requests a clean app exit |
| SD/eMMC/removable storage | Kernel block devices and the existing `edisksd` service | Mounted paths, read-only status and free space; D-Bus volume metadata and explicit mount/unmount requests by service object path |
| Sync input/output | GPIO lines named `CV_CLK_IN` / `CV_CLK_OUT` | Opt-in `ClockIo`: timestamped rising/falling input edges, loss counts and explicit output levels; kernel-owned lines are never taken over |
| USB device connection | Existing USB device controller and gadget configuration | Read-only UDC connection state; the app preserves the existing USB networking/gadget setup |
| SD card access | The gadget's configfs `mass_storage` function (`CONFIG_USB_CONFIGFS_MASS_STORAGE`) and `edisksd` | `CardShare` lends the removable card to a computer as a USB drive beside audio, MIDI and networking, and takes it back (see "SD card access") |
| Thermal/network status | Kernel thermal zones and network interfaces | Read-only temperatures and interface state; no radio, network or thermal policy changes |
| CPU and memory | `/proc/stat` and `/proc/meminfo` | `Platform` reads busy and total CPU time per core and total and available memory; read-only |
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
message length, plus monotonic receipt timestamps. Inputs are opened in
alsa-lib's timestamped read mode (`SND_RAWMIDI_READ_TSTAMP`, kernel framing
since Linux 5.14; the MPC runs 6.12 with alsa-lib 1.2.11), so the stamp is
the kernel's CLOCK_MONOTONIC time as its driver (the UART's serial-generic,
the gadget's f_midi, snd-usb-audio) received the bytes; where that mode is
missing the read time stands in. They are not physical MIDI-wire timestamps.
The MIDI loop sleeps in `HardwareMidi::wait()` (`ppoll` on every open input)
rather than a fixed tick, so input is read as it arrives; an app's timed
output (`App::midi_output`, `MidiOutput::wake_at`) sets how long it may sleep,
and `Session::wake_midi()` (an eventfd in the same `ppoll`) ends the sleep from
any thread.
The USB-C gadget carries MIDI: the stock configuration links `midi.midi`, the
port is `f_midi` (driver `MIDI Gadget`) on the MPC and "MPC Sample" on a
computer. A computer with no MIDI app open does not read it, so what is sent
meanwhile waits in the gadget and arrives in one burst when an app opens the
port. Surface messages and musical MIDI are kept separate. Device
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
python3 device.py ctl jacks
python3 device.py ctl usb-output
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

`card-share` reports SD card access as JSON; `card-share on` lends the card
to the computer, `card-share off` takes it back once the computer has
ejected it, and `card-share force` takes it back even if the computer still
holds it (see "SD card access").

`clock-in on|off` opts into GPIO input edge reporting. `clock-out 0|1` opts
into ownership of the dedicated output and sets its level; normal cleanup
returns it low and releases the line. These commands do not change the
undocumented `CV_SEL` or `CVCLK_RST` mux/reset lines. Event timestamps are
kernel monotonic nanoseconds. The service loop is not a musical clock
scheduler, and output timing/electrical routing requires hardware validation.
An app may drive the output from the MIDI thread (`MidiOutput::sync`, timed by
its own schedule); `ClockIo` takes a short lock for that. Melodee (melodee-sample)
drove its CV clock at up to 192 pulses a quarter this way; the jack's voltage
has not been measured.

`--audio-output NAME --audio-input NAME|off` selects another ALSA PCM. This
base requires stereo S32 and 128-frame periods, at 44.1, 48 or 96 kHz with
two to eight periods queued (`AudioFormat`; 44.1 kHz and three by default).
An app asks for its format with `App::audio_format()`; the runtime reopens
the device when that changes (about a second of silence at the headphones
and line out, below) and falls back to the default when the PCM refuses it.
The runtime opens the device with the format `audio_format()` gives as it
starts, so an app that keeps its format in a settings file reads that file
in its constructor rather than reopening a moment later. The AK4558 takes
all three rates: its driver sets
normal speed for 44.1 / 48 kHz and double speed for 96 kHz (`DFS`), with the
card's fixed 512 fs master clock (24.576 MHz at 48 kHz, read from the clock
tree; 96 kHz opens and runs without errors, its sound not yet checked by ear).
The device tree's `simple-audio-card,convert-rate = 44100` does nothing on
this card (plain `simple-audio-card` has no DPCM back end). Unsupported
playback settings fail explicitly; optional capture failure is reported by
`capture_active=0`. `status` adds `audio_rate`, `audio_periods`,
`audio_latency_frames` and `audio_reopens`.

### Opening and closing without a click

Headphones and line out pass through the board's analog mute, the
`simple-audio-mute` aux device (`mute-gpios` GPIO4_C0, inMusic's
`sound/soc/codecs/simple-mute.c`): DAPM shuts it before powering the
codec's output path down (then waits `mute-delay-ms`, 50 ms) and opens it
`unmute-delay-ms`, 1000 ms, after powering it up. The AK4558 driver sets
`use_pmdown_time`, so the kernel powers the path down only `pmdown_time`
(5000 ms) after a playback stream ends, and the mute stays open meanwhile.
A stream opened at another rate inside those five seconds sets a new master
clock (22.5792 MHz for 44.1 kHz, 24.576 MHz for 48 kHz) on a codec that is
powered and unmuted, and the jacks click. That happened at every start
while an app's rate differed from the one before it: the stock app's
44.1 kHz, or the 44.1 kHz default an app opened before reopening at its
saved rate.

The built-in speaker has no such mute. Its amplifier is a
`simple-audio-amplifier` (GPIO `SPKR_EN`) behind the `Builtin Speaker` pin,
and DAPM enables it as soon as the DACs are up. Traced through
`/sys/kernel/debug/gpio`, `SPKR_EN` rose 20 ms after the codec left reset
(`nCODEC_RESET`), while the codec's outputs still settle, so every power-up
clicked in the speaker; with headphones in too, for the moment before the app
switched the speaker off.

`AudioDevice` therefore sets the codec's clocks only while it is powered
down, and keeps the speaker off while it powers up
([src/drivers/codec_power.h](../src/drivers/codec_power.h)):

| Moment | What it does |
| --- | --- |
| Close | After the stream stops, its hardware setup is freed (`snd_pcm_hw_free()`) with the link's `pmdown_time` (`/sys/class/sound/card<N>/device/<link>/pmdown_time`, the link named by the PCM's id) at 0 for that moment and then restored: DAPM shuts the mute, waits 50 ms, powers the DACs down and holds the codec in reset (`PDN`) at once. Every close does this, a reopen and leaving the app alike, so the next stream, at any rate, starts a codec that is off. |
| Open | When the codec is still powered (the link's `dapm_widget` reads `PM State` other than `Off`) at another rate, the stream another program left, the device first takes it over at that rate: the master clock's rate (`i2s1_mclkout_tx` in debugfs, `/sys/kernel/debug/clk/i2s1_mclkout_tx/clk_rate`, ÷ 512) names it, and preparing a stream cancels the kernel's pending power-down without touching the clock. That setup is then freed as at a close, and the wanted rate is set on a codec that is off. |
| Speaker | Unless the codec already runs at the wanted rate, the device first switches `Builtin Speaker Switch` off (`SpeakerHold`), so preparing the stream powers the codec up without the speaker's amplifier. The runtime's service thread switches it back on 1000 ms after the stream is prepared, as the mute opens the jacks; an app's speaker change meanwhile (`HardwareMixer::speaker()`, `mixer-set`) waits until then, the last one counting. |

All three outputs then stay silent for about a second after a stream
starts: the jacks behind the mute, the speaker behind its switch. Leaving
the app closes the device before the mixer restores its controls, so the
restored speaker switch does not start the amplifier for a moment before
the codec powers down; the restored value is the switch as it was before
the first hold. A PCM without an ASoC codec link (USB, `null`, a host
build), or a codec whose clock rate cannot be read, is opened and closed
as before. A sample-base killed while `pmdown_time` reads 0 leaves it there;
restore-stock.sh puts the kernel's 5000 back. One killed during the speaker's
second leaves the speaker switch off until an app sets it or the MPC restarts.

The USB-C audio gadget follows the device's rate: when it changes, the
service thread rewrites the UAC2 function's `p_srate` and `c_srate` (equal,
the gadget shares one clock) and rebinds the gadget as the stems widening
does (`set_uac2_rate()`, `drivers/usb_gadget.h`), so the computer sees the
MPC at 48 or 96 kHz too; USB audio, MIDI and SSH drop for about a second.
Never while the card is lent; `--usb-output off` leaves the gadget alone.
Checked on the MPC with the Mac: 18 channels at 96 kHz bind (the full-speed
descriptor warns of a 6984-byte packet, which high speed never uses) and
record with no underruns; restore-stock.sh puts the original rate back.
There is no implicit resampling of the main stream. USB-C audio in and out
are separate streams with their own rate conversion, described below.

## USB-C direct monitoring

An app takes USB-C in a block at a time (`UsbAudioInput::pull()`, its ring
held at 288 frames at 48 kHz) and the codec output then queues each block
behind two or more periods (256 to 384 frames at two periods): a
computer's instrument played from the MPC comes back about 11 ms after it
arrives. `DirectMonitor` ([src/drivers/direct_monitor.h](../src/drivers/direct_monitor.h))
skips both, as an audio interface's direct monitoring does: the USB-C input
thread adds each 64-frame period it reads into the AK4558's playback buffer
96 frames ahead of where the I2S DMA reads, on top of what the app rendered
there, about 3 ms in all.

| Part | How |
| --- | --- |
| Buffer | The codec's playback PCM opens with mmap access (`SND_PCM_ACCESS_MMAP_INTERLEAVED`, written with `snd_pcm_mmap_writei()`, as `writei` before); `AudioDevice` lends the buffer while `run()` runs and takes it back (waiting out a write in progress) before it goes. Other outputs, and a PCM that refuses mmap, stay read/write without it. |
| Position | The Rockchip I2S DMA reports its position to the frame (dmaengine residue; `hw_ptr` moved 316 to 496 frames between reads at 48 kHz, never in whole periods). After each block the audio thread reads `snd_pcm_avail()` and publishes the frames written and played with the time; the input thread extrapolates on the device's rate. |
| Pace | The computer sends at the AK4558's pace through `Capture Pitch` (above), so each period lands where the last ended, but the lock lets it wander by tens of frames over seconds. More than 16 frames off 96, a period repeats or leaves out one frame (`nudges()`: on the MPC with a Mac, one every 5 to 10 s once settled); out of 32 to 192 frames ahead it starts again at 96 (`slips()`, a click; none in a minute with the nudges, one in 3.6 minutes without). Never past what the app has written: that would be written over. |
| Control | `DirectMonitor::set(on, left, right)` (or one gain) from any thread; one process-wide instance, `direct_monitor()`. The input thread eases to the gains over 5 ms, and to silence after `on` goes false, so neither the app's volume steps nor turning it on and off click. Dry: an app that also plays USB-C in through its own mix leaves it out while this is on (Melodee gives the level and pan its mix would have, before effects). |

## USB-C audio output

A computer recording from the MPC over USB-C gets whatever the app renders.
After every `App::render()` the runtime hands the 128-frame block to
`UsbAudioOutput` ([src/drivers/usb_audio.h](../src/drivers/usb_audio.h)),
so the computer hears what the jacks and speaker play, after the app's own
volume (in the demos, the main volume knob). Apps need no code for it. It is
on by default; `--usb-output off` starts without it.

The device's gadget configuration on 1.3.0.12
(`/sys/kernel/config/usb_gadget/ac50/functions/uac2.audio`, read on the MPC)
has `p_chmask=3`, `p_ssize=4`, `p_srate=44100` and `shared_clock=1`, on the
card with ID `UAC2Gadget`; the stock service builds it from
`/usr/lib/az0x-usb-gadget/ac50.scheme`, linking `uac2.audio`, `ncm.network`
and `midi.midi` into configuration `c.1` in that order. As built, the
computer can record only stereo S32_LE at 44.1 kHz. The kernel's `u_audio.c` gives the rest: the gadget's
playback PCM is the stream to the computer, and each USB packet takes
rate × pitch / interval frames from its ALSA buffer. The PCM-interface
control `Playback Rate` reads 0 until the computer starts recording.

| Stage | What happens |
| --- | --- |
| Waiting | Every 250 ms the output thread looks for the gadget and reads `Playback Rate`, and between those checks it waits on the gadget's control events: f_uac2 announces `Playback Rate` as the computer starts or stops streaming, and the thread checks at once. While it is 0 nothing is opened, and `push()` drops blocks rather than queueing audio for a recording that has not started. |
| Opening | At the computer's rate: periods of up to 256 frames (u_audio.c allows 4,096 bytes a period, 56 frames of 18 S32 channels), a four-period buffer (224 frames, 4.7 ms at 48 kHz with 18 channels), started once the first full buffer is written. The gadget's `Playback Pitch 1000000` is set to 1000000 and the stream locked (Drift). A rate change reopens; the rate returning to 0 closes and leaves the pitch at 1000000. |
| Hand-off | The audio thread converts each block to float and pushes it into an 8,192-frame lock-free ring. The output thread primes it to its target, sending silence until then and again if it runs dry: locked, one 128-frame block, one period and 2 ms (280 frames at 48 kHz with 18 channels); otherwise 1,024 frames. |
| Conversion | Locked at the device's own rate, none: the frames are encoded to the gadget's sample format untouched. At another rate, or unlocked, the band-limited converter the USB-C input uses in the other direction (Rate conversion, below) converts them first. |
| Drift | The AK4558's clock and the computer's USB clock differ by tens of parts per million, enough to empty or fill the ring within minutes even at the same nominal rate. `Playback Pitch` sets the frames in each packet the gadget sends, in millionths of the nominal rate, and the computer records whatever arrives (the gadget's input endpoint is asynchronous), so the gadget sends at the AK4558's pace: after every 250 ms of the computer's audio, the ring's average fill sets the pitch through `PitchLock` ([src/dsp/frame_ring.h](../src/dsp/frame_ring.h)) as on the input, its sign turned, a ring above its target sending faster. Against a Mac at 48 kHz the pitch settled between −18 and −57 ppm (the input's `Capture Pitch` read −21 to −36), the ring never under 192 frames, with no underruns. Without the control (`--usb-output` naming another PCM), or once the ring strays more than a third off its target for a second after 2 seconds' settling, the output unlocks: the pitch goes back to 1000000 and the conversion is trimmed instead (`drift_trim()`): 2 ppm per frame away from 1,024, at most ±0.2 %, settling in about 11 seconds, a 100 ppm drift 50 frames off 1,024. |
| Faults | A playback underrun (the thread was starved) re-prepares the stream and counts in `xruns`. Any other error closes it until the next check. |

Until the playback PCM runs, u_audio.c sends every packet at the size it was queued with: rate / 8000 frames rounded down, 5 at 44.1 kHz (40 kHz to the computer) and exactly 6 at 48 kHz. The gadget has one clock for both directions (`shared_clock`), and a Mac or an iPhone times a stream it starts by these packets. With the stream here opening up to 250 ms after the computer started, a Mac at 44.1 kHz then wrote part of each of its 512-frame buffers too late on about half the starts, and up to 45 % of what it sent arrived as silence, in place; at 48 kHz never (issue 74, measured 2026-11-28 with a Mac). Waking on the control event closes that gap. The computer selects the stream a while before it takes the first packet, so the output thread sends silence and accepts audio, and starts measuring the ring for its lock, only from the first room after the stream started; accepting at once left the ring about 200 frames over its target and the lock gave up.

### Stems

Beside the main mix, a computer can record each of an app's 16 tracks on its
own channel. The stock gadget is stereo, so at start-up, before anything
opens audio or MIDI, the runtime widens it (`widen_uac2_playback()` in
[src/drivers/usb_gadget.h](../src/drivers/usb_gadget.h)):

| Step | What happens |
| --- | --- |
| Find | The first `uac2.*` function under `/sys/kernel/config/usb_gadget/*/functions`. Without one nothing changes. |
| Compare | Its `p_chmask` against 18 channels (`0x3ffff`). Already there (every later start until a reboot): nothing is touched. |
| Save | The first time, `/run/sample-base-uac2-p_chmask` gets the function's directory and its original mask. |
| Rebind | `UDC` is emptied and every function link taken out of the configuration (f_uac2 refuses a new mask, `EBUSY`, while any configuration links it, bound or not), `p_chmask` written, the links made again in their old order (configfs lists them newest first) to each function's absolute directory (configfs resolves a relative target from the caller's working directory, not the link's) and `UDC` given back its controller. The Mac sees the MPC leave and come back; USB networking (SSH) and USB MIDI drop for about a second. |
| Restore | `install/restore-stock.sh` puts the saved mask back the same way before it starts the stock app, which expects stereo. A swap between apps keeps the wide gadget. |

The kernel (`f_uac2.c`, `UAC2_CHANNEL_MASK`) allows at most 27 channels a
direction, too few for 16 stereo tracks and the main mix (34), so each stem
is mono. Bandwidth is not the limit: 18 channels of S32 at 44.1 kHz is 432
bytes a microframe, under high speed's 1,024. The channel order is fixed:

| Channels | Carry |
| --- | --- |
| 1–2 | The main mix, exactly as before, or `App::usb_mix()` |
| 3–18 | Track 1–16 stems from `App::stems()`, silent for apps without stems |

The stems share the main mix's ring, pitch, conversion and trim, so all
18 stay sample-aligned. The converter passes over a stem that has been
exactly silent for its whole kernel. The gadget names channels by USB spatial position
(front left, front right, centre, LFE, …); a DAW lists them as inputs 1–18.
`--usb-stems off` leaves the gadget as the stock service built it.
`usb-output` reports `stems`, the stems the open stream carries.

`--usb-output NAME` writes any other ALSA PCM instead, for example an audio
interface on the USB host port; without a rate control it opens at 44.1 kHz,
and without the pitch control it is converted and trimmed.

```sh
python3 device.py ctl usb-output
python3 device.py ctl usb-output off
python3 device.py ctl usb-output on
```

`usb-output` returns JSON with the state (`stopped`, `missing`, `waiting`,
`running` or `failed`), the requested and opened device, rate, channels, stems, bits,
frames buffered, the current trim in ppm, `locked`, `pitch_ppm` (how much
faster than nominal the gadget sends), the `target` fill, `lowest` (the
least the ring held as the thread took frames, since the stream settled),
ring overflows and underruns, PCM xruns and opens. `usb-output on` restarts with the `--usb-output` PCM, or the
gadget when that was `off`. `status` reports `usb_output=STATE`.

Locked at 48 kHz, audio leaves the MPC about 11 ms after the app renders it
(280 frames in the ring, up to 224 in the gadget's buffer, 1 ms of USB
requests), about 6 ms after the jacks; through the 1,024-frame ring and the
converter it took about 28 ms. If the
computer plays its input straight back to the MPC while an app monitors
USB-C input, the audio loops; the demos never monitor USB-C input.

### Rate conversion

Both USB-C directions convert with `SincResampler`
([src/dsp/frame_ring.h](../src/dsp/frame_ring.h)): each output frame is the
input under a Kaiser-windowed sinc (β 9.2, 32 zero crossings a side, read
from a shared 68 KB table of 256 points a crossing with linear interpolation
between them, and a copy of it by place within a crossing) centred on its
position. Everything up to 0.907 of the lower
rate's Nyquist frequency, 20 kHz at 44.1 kHz and 21.8 kHz at 48, passes flat
to ±0.0002 dB, and what the conversion folds or images back below that edge
stays at least 90 dB under the signal (91.8 dB at worst, near 19.9 kHz), for
any pair of 44.1, 48 and 96 kHz and wherever between two input frames the
drift trim puts the read position. As in dedicated asynchronous rate
converters, the −6 dB point is the lower Nyquist frequency, so the only
content left partly folded lies between 20 and 24.1 kHz at 44.1 kHz and
folds to above 20 kHz. Converting
down, the kernel stretches over more input frames (35 a side for 48 to
44.1 kHz, 70 for 96 to 44.1) so that the band follows the output's rate.
Each output frame is divided by its weights' sum, so DC passes exactly, and
at equal rates with no trim every frame passes bit for bit, 32 frames
(0.7 ms) late.

The linear interpolation it replaces lost up to cos(π f / rate) as the trim
swept its read position between frames: at a 50 ppm trim, 15 kHz swung
between −0.4 and −6.3 dB every 0.45 s and 20 kHz fell to −16 dB, and from
48 to 44.1 kHz a 10 kHz tone left an alias 24 dB down at 6.1 kHz. It stays
in frame_ring.h as `LinearResampler` for code that converts on the audio
thread.

Each output frame works out its weights once, then takes one NEON dot
product per channel over that channel's own history; a channel exactly
silent for the whole kernel is skipped. Unstretched (at equal rates and
converting up) every tap sits at the same place within its crossing, so the
weights are read from the copy by place, in a row. On the MPC's Cortex-A55,
18 channels take this share of one core, against the converter before it,
which weighed every channel tap by tap in one interleaved history (its 44 %
at 48 kHz is what the output thread used while it trimmed every frame):

| Converting | Before | 2 channels sounding | All 18 sounding |
| --- | --- | --- | --- |
| 48 to 48 kHz, trimmed | 44 % | 5.5 % | 14 % |
| 44.1 to 48 kHz | 45 % | 6.1 % | 15 % |
| 48 to 44.1 kHz | 46 % | 14 % | 24 % |
| 96 to 44.1 kHz | 90 % | 25 % | 47 % |

Locked at equal rates nothing is converted: with a Mac recording all 18
channels at 48 kHz the output thread uses 4.4 % of a core, waking for each
56-frame period, against 47 % before.

## SD card access

The stock app's Project > SD Card Access offers the microSD card to a
computer as a USB drive. It does so by switching the whole gadget to
`/usr/lib/az0x-usb-gadget/ac50-sd-access.scheme` (`az0x-usb-gadget switch
sd-access`): one `mass_storage` function and a different product ID, so
audio, MIDI and USB networking are gone until it switches back. The base
does it beside them instead, so a computer keeps recording stems and SSH
keeps working ([src/drivers/card_share.h](../src/drivers/card_share.h),
[src/drivers/usb_gadget.h](../src/drivers/usb_gadget.h)). Apps call it on
the service thread through `Hardware::card`; the remote through
`card-share`.

| Step | What happens |
| --- | --- |
| Find | The card is the first removable `flash_sd` file system edisksd reports (its `Removable`, `Media`, `SysPath` and `Mountpoints` properties); its disk is the parent in `SysPath` (`mmcblk1`). |
| Unmount | `sync`, then edisksd's `Unmount` on each mounted file system of that disk. If one refuses (a file open, a program running from it) nothing is lent and the card is mounted again. The kernel's mount table is checked as well: the card is never offered while anything on it is mounted on the MPC. |
| Offer | A `mass_storage.sd` function (one removable, read-write LUN, `lun.0/file` = `/dev/mmcblk1`) is made and linked into `c.1`, the gadget unbound and bound again (about a second without USB). `/run/sample-base-card-share` names the disk and the links' old order first, so a crash still takes the card back. |
| Lent | `connected` follows the controller's state (`configured`), `ejected` the LUN's file going empty when the computer ejects the drive. |
| Take back | `lun.0/file` emptied. While the computer has the drive mounted it locks the medium (PREVENT MEDIUM REMOVAL) and the kernel refuses with `EBUSY`: `card-share off` says to eject it on the computer first. `force` writes `forced_eject` instead. The function is unlinked and removed, the links go back in their old order, the gadget rebinds. |
| Mount again | The block device is flushed and its partitions read again (the computer may have repartitioned or formatted it), then edisksd mounts each file system of the disk, for up to five seconds. `returns` counts each homecoming, so apps rescan. |

**The controller's FIFOs decide the link order.** The RK3566's DWC3 hands
IN endpoints out in link order, and its transmit FIFOs 1–5 hold 3104 bytes
each but FIFO 6 only 336 (`GTXFIFOSIZ(6) = 0x07d6002a`, read in debugfs),
less than one 512-byte high-speed bulk packet. Linked last, the drive's bulk
IN landed there and never sent a block: macOS saw the interface and hung
probing it. Linking the network function after the drive puts NCM's 16-byte
notify endpoint on FIFO 6 instead; the audio and MIDI interfaces keep their
numbers, NCM moves after the drive while it is lent and comes back after.
Checked on a Mac: the card mounts, files written there are on the card
afterwards, the computer's lock refuses a plain take-back, SSH, the 18
channel audio device and the MIDI port stay up throughout.

Leaving the app takes the card back at once, forced. A new start takes back
a card an earlier run left lent, and `install/restore-stock.sh` does the
same before the stock app starts. The stock app's own SD Card Access is not
touched.

## Screen Mirror (the screen as a USB webcam)

Screen Mirror offers the screen to a computer as a USB camera
([src/drivers/screen_mirror.h](../src/drivers/screen_mirror.h)). macOS,
Windows and Linux use it without a driver; so do iPads with USB-C (iPadOS
17 and later). iPhones do not take USB cameras. An app turns it on and picks
the size through `App::screen_mirror()`; the runtime does the rest on its
own thread.

| Size | Scale | 30 fps | 60 fps |
| --- | --- | --- | --- |
| 320 × 240 | 1 | YUYV, 4.6 MB/s | YUYV, 9.2 MB/s |
| 640 × 480 | 2 | YUYV, 18.4 MB/s | MJPEG |
| 1280 × 720 | 3, black at the sides | MJPEG | MJPEG |
| 1280 × 960 | 4 | MJPEG | MJPEG |

Every size is the screen scaled by a whole number, so each screen pixel
stays a sharp square ([src/drivers/mirror_frame.h](../src/drivers/mirror_frame.h));
the computer's own window may still scale the picture. YUYV is exact to
the pixel (at an even scale each YUYV pair is one screen pixel, so its
colour stays exact) and is used wherever one endpoint carries it; JPEG is
quality 95. Larger sizes were tried and taken out: 1920 × 1440 needed 34 ms
a picture (encoder 26 ms, CPU 8 ms), more than a frame at 60 fps.

**Making the pictures.** A thread of its own makes each picture when the
screen changed, at most once a frame of the stream (a 60 fps screen feeds a
30 fps stream every other picture): YUYV at once, JPEG through the encoder
with two slots, the CPU filling one (1.1 ms at 640 × 480, 2.3 ms at
1280 × 960, writing uncached memory) while the encoder makes the other
(3.0 and 11.4 ms). It keeps the last four pictures, each with the time the
screen was drawn. Measured on the MPC (2026-11-14): the screen thread draws
about 60 pictures a second while a project plays, and a JPEG of a playing
project is up to about 340 kB at 1920 × 1440 (a still settings page 55 kB
at 1280 × 720).

**The steady buffer.** The streaming thread ticks at the stream's rate and
at every tick sends the newest picture drawn at least two frames before
(33 ms at 60 fps, 67 at 30), so the pictures go out evenly spaced in the
screen's own time however unevenly the screen thread drew them or the
maker made them, and a picture that costs more than a frame to make never
holds a tick up; with nothing new the last picture goes again. The cost is
those two frames of delay and four kept pictures. `status` says how it
goes (`mirror_frames=` sent, `mirror_late=` ticks with no picture sent,
`mirror_bad=` pictures the kernel dropped); the service log has only what
went wrong (a failure, no encoder). Measured with a
project playing (2026-11-14): 1280 × 960 at 60 fps sent 60.0 pictures a
second, each made 16 ms after the screen was drawn (21 at most), 81 kB a
picture, no tick late, the longest gap between buffers 19 ms, no audio
dropouts.

**USB bandwidth.** USB 2.0 keeps 6000 bytes of each 125 µs microframe for
isochronous and interrupt endpoints, booked when a stream starts. The
18-channel audio to the computer takes 504 bytes at 48 kHz (936 at
96 kHz), the computer's stereo to the MPC 56 (104), its feedback 4; the
webcam books 1024 bytes for 320 × 240 YUYV at 30 fps, 2048 at 60, and 3072
(the most one endpoint carries, 24.6 MB/s) for 640 × 480 YUYV and every
JPEG (`MirrorFormat::packet_bytes()`): about 4100 of the 6000 at 96 kHz. A
webcam that did not fit would fail to start rather than take from the
audio, and microframes it leaves empty go to the bulk traffic (the network,
the card). With one packet a microframe (8.2 MB/s), JPEG of a playing
project at 1920 × 1440 (340 kB) reached only 23 fps; with three it holds
30. Uncompressed above that is beyond one endpoint (640 × 480 YUYV at
60 fps would need 37 MB/s), so those are JPEG.

**The webcam function.** The MPC's kernel has the gadget's UVC function
switched off (`# CONFIG_USB_CONFIGFS_F_UVC is not set`), though V4L2 and
videobuf2 are there. `kernel/uvc/` is the function's source from inMusic's
6.12.60 release (`drivers/usb/gadget/function/f_uvc.c` and its `uvc_*.c`,
`drivers/media/common/uvc.c`), built as `sample_base_uvc.ko` with
`tools/build_kernel_modules.sh` and loaded from memory after `modprobe`
brings in videobuf2. One change, marked in `f_uvc.c`: upstream holds the
whole gadget off the bus (`bind_deactivated`) until a process opens the
video device, so audio, MIDI and SSH over USB would go each time the app
stops or crashes. Here the gadget stays on, and the function refuses the
computer's requests (a stall, at once) while no process answers.

The function is `uvc.screen` (`gadget_add_mirror()`,
[src/drivers/usb_gadget.h](../src/drivers/usb_gadget.h)): one format and one
frame, the size and rate asked for (333333 or 166666 × 100 ns); no
interrupt endpoint; an isochronous streaming endpoint of 1024-byte packets,
one to three a microframe (up to 24.6 MB/s, which the controller's 3104-byte
FIFO holds: 3 × (1024 + 8) + 8). It is linked after the UAC2 function and before
USB networking: the interfaces are audio (0–2), webcam (3–4), MIDI, then
NCM, and the endpoints IN 1–2 audio, 3 the webcam, 4 MIDI, 5–6 NCM, so
NCM's notify endpoint keeps the 336-byte FIFO 6 (above). macOS knows the
audio device by its interface numbers, which do not move; it finds the MIDI
port and network interface again under their new numbers. The process that
opens the video device answers PROBE and COMMIT (f_uvc numbers the
interfaces 0 and 1 in its events, whatever the gadget made them), and on
STREAMON queues the pictures.

**The card shared.** A drive linked beside all that would need a seventh IN
endpoint, and FIFO 7 has no room at all (`GTXFIFOSIZ(7) = 0x08000000`): the
webcam steps out while the card is lent (`gadget_add_storage()`) and comes
back with the card (`gadget_remove_storage()` relinks it from the saved
order). The state reads `card-shared` meanwhile.

**The JPEG encoder.** The RK3566 has the RK3568's VEPU (`video-codec@fdee0000`,
`rockchip,rk3568-vepu`, JPEG only), and the MPC's kernel ships `hantro-vpu`
for it, but the device tree leaves the node `disabled`. Its IOMMU
(`iommu@fdee0800`) is on. The kernel has no `CONFIG_OF_OVERLAY` (no overlay
at run time), and hantro refuses a node that is not available, so
`kernel/vepu/sample_base_vepu.c` sets the node's `status` to `okay` in the
live tree (memory only) and creates its platform device; `modprobe
hantro-vpu` then registers `rockchip,rk3568-vepu-enc`, a V4L2 mem2mem
device (YUYV in, a whole JPEG out; [src/drivers/jpeg_encoder.h](../src/drivers/jpeg_encoder.h)).
The module has no exit: removing the device (`rmmod`) hung the kernel and
the MPC rebooted itself (2026-11-14, no log kept). It stays until the next
boot. When no encoder comes up, a JPEG size falls back to 640 × 480 YUYV
(`mirror_size=640x480-fallback` in `status`).

Turning Screen Mirror on or off, or another size or rate, rebinds the
gadget like a rate change (about a second without USB). `status` reports
`mirror=` (`off`, `starting`, `waiting`, `streaming`, `card-shared`,
`failed`), `mirror_size=`, `mirror_fps=`, `mirror_frames=`, `mirror_late=`,
`mirror_bad=` and `mirror_held=`; the service log has a `Screen mirror:`
line only for a failure. Checked with QuickTime and OBS on a Mac.

**Never under a computer showing it.** Unplugging the camera while an app
shows it made QuickTime report an error and close its window, and OBS
crash (2026-11-14). So nothing takes the webcam out of the gadget while a
computer streams it: `/run/sample-base-mirror-live` is there from the
computer's STREAMON to its STREAMOFF (or its going), and while it is

- turning Screen Mirror off or another size waits (`mirror_held=1`) and
  happens once the computer stops the stream (the app closes the camera);
- `CardShare::lend()` refuses with "the camera is in use on the computer"
  before anything is unmounted (`gadget_add_storage()` refuses too);
- the USB-C audio rate does not follow the device (`follow_usb_rate()`);
  USB-C audio converts meanwhile and the rate follows after;
- `install/restore-stock.sh` leaves the webcam in the gadget (until a
  reboot, or the next app turns it off).

When the app stops mid-stream (a restart, a crash), the webcam stays: the
computer's picture stands still, and when the app runs again kernel/uvc
gives the new process a STREAMON of its own (`uvc_v4l2_subscribe_event()`),
so it sends again without the computer asking; that STREAMON answers no
request of the computer's (`delayed_status`, so composite has nothing to
warn about), and a process that leaves while the computer waits on it
answers first. A webcam an earlier run left is taken as it is
(`gadget_mirror_format()`), so a restart with the same size never rebinds.

## File Sharing (the card as a network drive)

File Sharing serves the SD card over SMB 2 and 3 to the computer on the USB-C
network ([src/drivers/file_share.h](../src/drivers/file_share.h)). The card
stays mounted on the MPC: the app and the computer use the same exFAT file
system at once, unlike [SD card access](#sd-card-access). An app turns it on
through `App::file_sharing()`.

The MPC's kernel is built without an SMB server and the root file system has
no Samba. `kernel/ksmbd/` is the kernel's own server, `fs/smb/server` from
the 6.12.60 release, built out of tree as `sample_base_smb.ko` together with
`cifs_arc4.c` and `nls_ucs2_utils.c` (`CIFS` and `NLS_UCS2_UTILS` are off).
The crypto it asks for by name (`md5`, `cmac`, `ccm`, `gcm`) and `nls_utf8`
are stock modules. On firmware 1.4 every base module is refused by LoadPin
(above), and this one with them; File Sharing reports `failed` there. The
6.18 build (`kernel/ksmbd-6.18/`, with `lib/crypto/arc4.c`; 6.18's ksmbd
calls `libmd5` directly) has not been loaded on an MPC.

ksmbd moves the files itself and asks a process for everything else over
generic netlink (`SMBD_GENL`); the runtime is that process, on a thread of
its own:

- **Start**: port 445 on the gadget's network interface only (`usb0`),
  signing when the computer asks for it, a 10 s heartbeat. A new run finds
  the server running and takes it over, so a computer keeps its connection
  across a restart; when no run answers, ksmbd's heartbeat stops the server
  10 to 20 s later (from its source; not timed here). A server reset under
  a run (`kill_server` from elsewhere; its `stats` say `startup`) is started
  again within a second.
- **Logins**: the name `guest` (or none) is a guest. Any other name is a user
  with an empty password, checked by the server; its session is signed.
  macOS refuses a named login that the server answers as a guest
  (`smbutil`: "Authentication error"); Windows 11 turns guest logons away
  by default (Microsoft's documentation; Windows not tried here).
- **Shares**: `SD Card` is the card's mount point, read-write, files made as
  root (exFAT has no owners). The share's top folder is where the card is
  mounted, so it needs ksmbd's `CROSSMNT` flag: without it the server
  lists the empty `/media/<uuid>` folder under the mount. `IPC$` carries
  srvsvc's share list (NetrShareEnum and NetrShareGetInfo,
  [src/drivers/smb_pipe.h](../src/drivers/smb_pipe.h)); other pipes
  (macOS asks for `mdssvc` and `MsFteWds`) are refused by ksmbd itself.
- **Connections**: while a computer has `SD Card` open, SD card access
  refuses to lend the card ("the card is open on the computer over the
  network"); lending stops the server first in any case
  (`/sys/class/ksmbd-control/kill_server`, which closes every connection).
  The server stops too when the card's mount point changes under an open
  connection, and when the computer has been unplugged for 5 s.
  `restore-stock.sh` stops it for the stock app. The module is never
  unloaded.

Measured on 1.3.0.12 from macOS (`smbutil`, `mount_smbfs`): SMB 3.1.1, the
share list as guest and by name, a 32 MB file written in 2.4 s and read back
in 1.0 s with the same SHA-256.

## Jack detection

Headphones and line out are the only jacks the AC50 can tell are plugged.
The device tree gives the I2S card `simple-audio-card,hp-det-gpio`
(GPIO3_B0) and `simple-audio-card,out-det-gpio` (GPIO3_B1), both active
high. The kernel's `simple_util_init_jack()` (`simple-card-utils.c`) makes
each a jack: it debounces the GPIO for 150 ms, the ASoC jack core powers the
`Headphones` or `Line Out Jack` DAPM pin to match, and the state is reported
as the read-only boolean card control `Headphones Jack` or `Line Out Jack`.
Line in (`TRS`), the sync jacks and the MIDI jacks have no detection switch.

Line out is two separate jacks, L and R, but the device tree has only one
line-out detect line. Measured on the device: a cable in L alone, in R
alone, or in both reads plugged, and line out reads empty only once both are
out. The two jacks' switches therefore act as one, and `Line Out Jack` (and
`Jack::LineOut`) means "L, R or both are plugged in". Nothing can tell which
jack is used, so an app cannot decide from detection whether to send a mono
sum to a single cable.

`JackDetect` ([src/drivers/jacks.h](../src/drivers/jacks.h)) opens its own
control handle on the card and subscribes to its events, so the mixer's
handle never fills with events and the controls are read only when the card
reports something. The service thread polls it, without blocking, on every
pass (a few milliseconds apart). It reports every jack whose state differs
from the last one reported:

| Stage | What happens |
| --- | --- |
| Startup | Both controls are read when the runtime opens the hardware. The first poll reports each jack with `initial` set, before the app's first `service()`. |
| Plug or unplug | The kernel queues a control event. The next poll drains every queued event, reads both jacks once and reports the jack that changed, about 150 ms after the contact moved. An event that changes no jack, such as a mixer write, reports nothing. |
| Control gone | A jack whose control cannot be read becomes unknown (-1) without a report. When it returns it is reported with `initial` set again, not as a plug. |
| Card gone | A failed event read closes the handle; both jacks are unknown and the card is reopened at most once a second. |
| No events | If the card refuses an event subscription, both jacks are re-read once a second instead. |

The runtime stores each state where every thread can read it,
`Session::plugged(Jack)`, then calls `App::jack(event, hardware)` on the
service thread. `Hardware::jacks` is a read-only view for `service()` and
`command()`. `status` reports `headphones=` and `line_out=` (1 plugged in, 0
empty, -1 unknown), and the `jacks` command returns JSON:

```sh
python3 device.py ctl jacks
```

`available` and `events` say whether the card is open and subscribed; then,
for `headphones` and `line_out`, the control name, `plugged` (1, 0 or -1),
`changes` (plugs and unplugs since startup, not counting initial states)
and `timestamp_ns`, the monotonic time of the last report. Event timestamps
are when sample-base read the control, not the GPIO edge.

Nothing is rerouted on a plug: the kernel already powers the jack's pin, and
the built-in speaker stays as `Builtin Speaker Switch` was left. One reading
on the device found it off with headphones plugged in. An app that wants the
speaker to follow the jacks sets that switch in `jack()`; for the second
after the codec powers up the change waits ("Opening and closing without a
click").

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
and reads 64-frame periods from a 2,048-frame buffer on its own thread. An
8,192-frame lock-free ring hands the first two channels, as stereo float at
the device's rate, to the audio thread, which waits for the ring's target
fill before using it and again after running dry.

The host and the AK4558 have independent clocks. In asynchronous mode (the
gadget's `c_sync` is `async`, with `fb_max` 5) f_uac2 gives the host a
feedback endpoint and the capture PCM a `Capture Pitch 1000000` control: how
many frames a microframe the host is asked to send, in millionths of the
nominal rate (u_audio.c takes 750000 up to 1000000 plus 1000 per `fb_max`).
`UsbAudioInput` sets it to 1000000 as it opens. After every 250 ms of the
host's audio, counted in its frames, the ring's average fill meanwhile sets it
through `PitchLock` ([src/dsp/frame_ring.h](../src/dsp/frame_ring.h)): 5 ppm
a frame away from the target plus the integral of the same over 20 seconds,
within ±0.2 %. The host then sends at the AK4558's pace, so the ring is held
at one audio block, one period and 2 ms (280 frames at 44.1 kHz, 288 at 48,
384 at 96) instead of 1,024, and at equal rates (the gadget follows the
device's rate) the frames pass untouched; at others the output's
band-limited converter (USB-C audio output, Rate conversion) converts them at
a fixed ratio. Closing sets the pitch back to 1000000 for whoever streams
next. In the test fixture a host 48 or 500 ppm off, fast or slow, settles
with the pitch averaging its drift the other way and never runs the ring dry.

A host that ignores the feedback, or one far beyond the ±500 ppm USB 2.0
allows a high-speed host, leaves the ring drifting off its target. After 2
seconds' settling, four checks in a row more than a third of the target away
unlock the input: the pitch goes back to 1000000, and from then on the stream
is converted, the ring primes to 1,024 frames, and the output's
`drift_trim()` trims the conversion: a ring above 1,024 frames is fed slower,
one below it faster, settling ppm / 2 frames off 1,024 within about 11
seconds. Without the control (another PCM, or a gadget in adaptive mode) the
input trims this way from the start. Untrimmed, a host 15 to 48 ppm slow (the
drift the output measured against a Mac) would empty the ring every 26 to 8
minutes, each time a 23 ms re-prime, and a host as fast would fill it over
about an hour, latency creeping from 23 to 186 ms until every overflow
dropped frames with a click. `UsbAudioInput` reports `locked`, `pitch_ppm`,
`trim_ppm`, the `target` fill and `lowest`, the least the ring held as new
frames came in since the stream settled (in the recorder state's `usb`).
Whether the
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

## System dashboard

The demo's System page (`src/demos/system/`, the **Knob FX** button) shows
the MPC's live state from what the base already reads. Nothing on it is
estimated: a value the device does not report shows `n/a`.

| Panel | What it shows | Where it comes from |
| --- | --- | --- |
| Health | CPU load, each core's load and 24 seconds of history; memory used of total; CPU and GPU temperatures; supply voltage, type and `online`; power key state and presses; backlight level of its maximum | `Platform::cpu_times()` (`/proc/stat`), `memory()` (`/proc/meminfo`, `MemAvailable`), `temperatures()` (the `cpu-thermal` and `gpu-thermal` zones), `supplies()` (`voltage_now`), `backlight()`; `PowerButton` |
| Audio | Audio engine running time, output and input peaks, capture state, playback and capture recoveries; USB-C device controller state; USB-C audio in and out: state, rate, underruns, xruns, the output's drift trim and the input's opens | `Session::audio`; `Platform::usb_state()`; `UsbAudioInput::stats()` and `UsbAudioOutput::stats()` |
| Ports | Each open MIDI port, which directions are open and whether it is the surface; dropped sends and input errors; whether headphones and line out are plugged in | The MIDI thread's port scan (`HardwareMidi::open_ports()`); `JackDetect::state()` (see Jack detection) |
| Storage | Each mounted block device once (bind mounts of it are left out): path, file system, read-only, free of total | `Platform::storage()`: `/proc/self/mountinfo` and `statvfs()` |

The demo's service hook reads all of it every 500 ms while the page is open,
storage every 2 seconds, and nothing while it is closed. Each opening starts
the CPU meter afresh, so the first load shows half a second later. The
service thread publishes a copy under a lock that only it and the screen
thread take; the screen copies it once per frame and reads the audio
engine's atomics directly. The supply is a voltage: the AC50 has no battery
gauge, so no percentage is shown.

`Hardware` gives every app the same sources: the mixer and platform as
before, plus read-only views of the power key, the jacks, `UsbAudioOutput`
and the MIDI ports as last scanned.

| Control | Action |
| --- | --- |
| Knob FX | Shows or hides the system page; lit while it shows |
| Left button above the screen, wheel left | Previous panel |
| Middle button above the screen, wheel right | Next panel |
| Right button above the screen | Back to the synth page |

The same functions are available remotely:

```sh
python3 device.py ctl page system
python3 device.py ctl system storage
python3 device.py ctl system next
python3 device.py ctl system
```

`system next|previous|health|audio|ports|storage` opens the page at that
panel. `system` alone returns JSON with the panel, the number of reads so far
and the latest readings: `cpu` (total, then each core, 0–1; empty until two
samples), `memory_total_kb`, `memory_available_kb`, `cpu_millidegrees`,
`gpu_millidegrees` (null without the zone), `supply`, `power_key`,
`backlight`, `udc`, `usb_in`, `usb_out`, `midi`, `jacks` (1 plugged in, 0
empty, -1 unknown) and `storage`. `status` reports `page=system`.

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
| Level meters right of the screen | Channel-1 CC 83 left strip, 84 right | A mask of the five LEDs (green, green, green, yellow, red from the bottom), bit 0 the bottom: 2 lit only the second, 3 the bottom two, 127 all |

The white/red buttons are Sample, SEQ, Pad FX, Knob FX, Erase, Chop, Mute,
Loop, 16 Levels, Sample Select and Tap Tempo. Shift, Minus, Plus, the three
buttons above the screen and wheel Push have no light. Stock clears CC
83–86 at startup; 83 and 84 are the meter strips (a chase of the four and
then masks, read by eye on 2026-11-11). 85 and 86 lit nothing, so the base
does not send them. The base draws no level on the meters itself; an app
sets them with `SurfaceLights::meter()`.

`SurfaceLights` keeps the requested state, which any thread may change, and
the MIDI thread sends only the lights that changed, at most 48 messages per
5 ms pass. Nothing is sent for a light the application has never set, so
startup leaves the stock app's last lights in place, apart from the four
transport lights the demo's recorder sets and the light show. The control commands
are `led pad 1-16 R G B`, `led fader R G B`, `led bank A-H on|off`,
`led button NAME off|dim|bright|red-dim|red-bright` (names as in
`src/surface/controls.h`, ignoring case and punctuation, so `tap-tempo` or `TapTempo`),
`led meter L|R 0-31` (the strip's mask) and `led off`.

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
belong to their kernel drivers and ALSA routing; `JackDetect` only reads
what detection reports. The application does not
write those GPIOs or codec registers directly. It does not rebuild the kernel
or replace system services. Its changes to the USB gadget are the USB-C
audio channel count for stems and, during SD card access, the mass-storage
function; the recovery script undoes both.

All new hardware-facing behavior still requires physical qualification.
See [VALIDATION.md](VALIDATION.md) for the offline tests and remaining checks.
