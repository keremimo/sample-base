// SPDX-License-Identifier: AGPL-3.0-only
// Screen Mirror's picture: whole-number scales placed and centred, BT.601
// video-range YUYV, black around the screen, a colour exact at an even scale
// and its Cb/Cr shared with the neighbour where a pair spans two pixels.
#include "drivers/mirror_frame.h"
#include <cassert>
#include <cstdint>
#include <vector>

using namespace mirror_frame;

static bool pair_is(const std::vector<uint8_t>& out, unsigned width, unsigned x, unsigned y, uint8_t y0, uint8_t u, uint8_t y1, uint8_t v) {
    const uint8_t* p = out.data() + (size_t(y) * width + x) * 2;
    return p[0] == y0 && p[1] == u && p[2] == y1 && p[3] == v;
}

int main() {
    auto p = place(1280, 720);
    assert(p.scale == 3 && p.x == 160 && p.y == 0);
    p = place(640, 480); assert(p.scale == 2 && p.x == 0 && p.y == 0);
    p = place(1280, 960); assert(p.scale == 4 && p.x == 0 && p.y == 0);
    p = place(320, 240); assert(p.scale == 1 && p.x == 0 && p.y == 0);

    uint8_t y = 0, u = 0, v = 0;
    yuv(0xff000000u, y, u, v); assert(y == 16 && u == 128 && v == 128);
    yuv(0xffffffffu, y, u, v); assert(y == 235 && u == 128 && v == 128);
    yuv(0xffff0000u, y, u, v); assert(y == 82 && u == 90 && v == 240);

    std::vector<uint32_t> screen(size_t(screen_width) * screen_height, 0xff000000u);
    screen[0] = 0xffff0000u;                                        // red top left
    screen[size_t(screen_height - 1) * screen_width + screen_width - 1] = 0xffffffffu; // white bottom right

    // 1280 x 720: three times, 160 columns of black each side.
    std::vector<uint8_t> out(1280u * 720 * 2, 0);
    to_yuyv(screen.data(), 1280, 720, out.data(), 1280 * 2);
    for (unsigned row : {0u, 1u, 2u}) {
        assert(pair_is(out, 1280, 0, row, 16, 128, 16, 128) && pair_is(out, 1280, 158, row, 16, 128, 16, 128));
        assert(pair_is(out, 1280, 160, row, 82, 90, 82, 240));      // the red pixel's first two columns
        assert(pair_is(out, 1280, 162, row, 82, 109, 16, 184));     // its third and the black beside it share a colour
    }
    assert(pair_is(out, 1280, 160, 3, 16, 128, 16, 128));          // the row below is black again
    for (unsigned row : {717u, 718u, 719u}) {
        assert(pair_is(out, 1280, 1116, row, 16, 128, 235, 128));
        assert(pair_is(out, 1280, 1118, row, 235, 128, 235, 128));
        assert(pair_is(out, 1280, 1120, row, 16, 128, 16, 128));    // past the screen
    }

    // 640 x 480: twice, every pair one screen pixel: the colour exact.
    std::vector<uint8_t> two(640u * 480 * 2, 0);
    to_yuyv(screen.data(), 640, 480, two.data(), 640 * 2);
    assert(pair_is(two, 640, 0, 0, 82, 90, 82, 240) && pair_is(two, 640, 0, 1, 82, 90, 82, 240));
    assert(pair_is(two, 640, 2, 0, 16, 128, 16, 128) && pair_is(two, 640, 638, 479, 235, 128, 235, 128));

    // A stride wider than the picture: the padding is left alone.
    std::vector<uint8_t> padded(size_t(700) * 2 * 480, 7);
    to_yuyv(screen.data(), 640, 480, padded.data(), 700 * 2);
    assert(padded[640 * 2] == 7 && padded[size_t(700) * 2 + 0] == 82);
    return 0;
}
