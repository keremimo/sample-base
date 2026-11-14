# Sources and attribution

## Project licence scope

Standalone app, built-in synth and project tooling: Copyright (C) 2026
Kerem Kilic (Ellic Studio), AGPL-3.0-only, except the adapted MIT privacy
checker identified below. The full AGPL text is in `LICENSE`.

## JUCE

JUCE 9.0.5, Raw Material Software Limited:
https://github.com/juce-framework/JUCE/tree/9.0.5

Pinned commit: `d60b7a78030661574a615b586d9325ba7a7fa7dd`.
The app uses JUCE under AGPLv3. JUCE also offers a commercial licence.
The downloaded checkout retains JUCE's full `LICENSE.md` and `JUCE.spdx.json`
inventory of its third-party dependencies. The build compiles core, events
and graphics, including their image, text-shaping and SVG dependencies.
No JUCE source or downloaded dependency binaries are tracked here.

## Privacy checker

`tools/check_private_files.py` was adapted from the owner's MPC Sample
Plugins project (`mpc-sample-plugins`). Its MIT notice, Copyright (c) 2026
MPC Sample Plugins contributors, is retained in `LICENSES/MPC-Plugins-MIT.txt`.
No sibling checkout is a build dependency.

## Kernel modules

The modules under `kernel/` are GPL-2.0 (text in `LICENSES/GPL-2.0.txt`),
as the kernel they load into; each `.ko` is built from the source beside it
by `tools/build_kernel_modules.sh` against inMusic's MPC Sample kernel
source release (6.12.60, pinned in `docs/HARDWARE.md`).

- `kernel/panel/`: sample-base's panel driver, after inMusic's
  `drivers/gpu/drm/tiny/st7796s.c` (John Keeping), GPL-2.0+.
- `kernel/uvc/`: the USB gadget's webcam function, the kernel's
  `drivers/usb/gadget/function/f_uvc.c`, `uvc_configfs.c`, `uvc_queue.c`,
  `uvc_v4l2.c`, `uvc_video.c` and their headers (Laurent Pinchart and
  others, GPL-2.0+) and `drivers/media/common/uvc.c` as `uvc_common.c`
  (GPL-2.0-or-later), copied from that release with their notices as they
  are; the changes are marked `sample-base:` in `f_uvc.c`.
- `kernel/vepu/`: written for this project, GPL-2.0.

## Fonts and platform

The apps load Liberation Sans from the existing target installation. The
build environment installs the distribution's Liberation font package for
previews; no fonts are copied from the MPC or included in this repository.
ALSA, DRM, libsystemd (sd-bus), FreeType and Fontconfig are linked to the
platform's libraries.
inMusic's kernel source release and its edisksd source were used as interface
documentation (pinned source and checksums in `docs/HARDWARE.md`); no
proprietary stock application code or assets are included.

The synth uses only mathematical oscillators and envelopes written for this
project. It has no external DSP engine, sampled audio or factory-preset data.
