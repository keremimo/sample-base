// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <cstddef>
#include <cstdint>

// The screen as Screen Mirror's webcam picture (drivers/screen_mirror.h):
// scaled up by a whole number, so each screen pixel becomes a sharp square of
// picture pixels, centred on black, as YUYV (4:2:2, BT.601 video range, what
// UVC hosts read YUY2 and MJPEG as). Each screen pixel is converted once and
// a picture row is written once to `out` per screen row it repeats, `out`
// being the hardware's (uncached) buffer.
namespace mirror_frame {
constexpr unsigned screen_width = 320, screen_height = 240;

// The largest whole scale at which the screen fits width x height, and the
// screen's top left corner there. 1280 x 720: 3, at (160, 0).
struct Placement { unsigned scale = 1, x = 0, y = 0; };
Placement place(unsigned width, unsigned height);

// `screen` is screen_width x screen_height words, 0xAARRGGBB (alpha unused).
// `out` holds height rows of `stride` bytes, width x 2 of them used; width
// must be even.
void to_yuyv(const uint32_t* screen, unsigned width, unsigned height, uint8_t* out, size_t stride);

// One colour as Y, Cb, Cr (the same sums to_yuyv() uses).
void yuv(uint32_t argb, uint8_t& y, uint8_t& u, uint8_t& v);
}
