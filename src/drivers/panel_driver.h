// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

// Which kernel driver runs the panel while an app runs (docs/HARDWARE.md,
// Display). The stock st7796s driver runs it turned to landscape, so every
// write crosses the panel's own refresh and a moving vertical line can be
// seen broken in two. kernel/panel's sample-base-panel driver runs it in its
// own portrait orientation, writing along the refresh. take_over() loads that
// module (inside the binary, src/drivers/panel_module.S) and binds the
// panel's SPI device to it; give_back() returns the device to st7796s for
// the stock app, as install/restore-stock.sh does after a crash. Display's,
// before it opens the DRM device and after it has closed it.
namespace panel_driver {
// True when the panel is on sample-base-panel now (a 240 x 320 portrait
// mode). False leaves it on st7796s, as it was.
bool take_over();
// The panel back on st7796s and the module unloaded. Harmless when it never moved.
void give_back();
}
