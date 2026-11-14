// SPDX-License-Identifier: AGPL-3.0-only
#include "drivers/mirror_frame.h"
#include <algorithm>
#include <cstring>
#include <vector>

namespace mirror_frame {

Placement place(unsigned width, unsigned height) {
    Placement p;
    p.scale = std::max(1u, std::min(width / screen_width, height / screen_height));
    p.x = (width - std::min(width, screen_width * p.scale)) / 2 & ~1u; // even: a YUYV pair never spans the edge
    p.y = (height - std::min(height, screen_height * p.scale)) / 2;
    return p;
}

void yuv(uint32_t argb, uint8_t& y, uint8_t& u, uint8_t& v) {
    int r = int(argb >> 16 & 255), g = int(argb >> 8 & 255), b = int(argb & 255);
    y = uint8_t(((66 * r + 129 * g + 25 * b + 128) >> 8) + 16);
    u = uint8_t(((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128);
    v = uint8_t(((112 * r - 94 * g - 18 * b + 128) >> 8) + 128);
}

void to_yuyv(const uint32_t* screen, unsigned width, unsigned height, uint8_t* out, size_t stride) {
    const Placement p = place(width, height);
    const unsigned shown_w = std::min(width, screen_width * p.scale), shown_h = std::min(height, screen_height * p.scale);
    std::vector<uint8_t> row(size_t(width) * 2), ys(screen_width), us(screen_width), vs(screen_width);
    for (unsigned x = 0; x < width; x += 2) { row[x * 2] = 16; row[x * 2 + 1] = 128; row[x * 2 + 2] = 16; row[x * 2 + 3] = 128; } // black
    for (unsigned y = 0; y < p.y; ++y) std::memcpy(out + y * stride, row.data(), row.size());
    for (unsigned y = p.y + shown_h; y < height; ++y) std::memcpy(out + y * stride, row.data(), row.size());
    for (unsigned sy = 0; sy < screen_height && sy * p.scale < shown_h; ++sy) {
        const uint32_t* source = screen + size_t(sy) * screen_width;
        for (unsigned x = 0; x < screen_width; ++x) yuv(source[x], ys[x], us[x], vs[x]);
        // A pair of picture pixels from the screen pixels under each: one Y each, their Cb and Cr averaged (the
        // same pixel twice at an even scale, so the colour stays exact).
        for (unsigned c = 0; c < shown_w; c += 2) {
            unsigned a = c / p.scale, b = (c + 1) / p.scale;
            uint8_t* pair = row.data() + size_t(p.x + c) * 2;
            pair[0] = ys[a]; pair[2] = ys[b];
            pair[1] = uint8_t((us[a] + us[b] + 1) / 2); pair[3] = uint8_t((vs[a] + vs[b] + 1) / 2);
        }
        for (unsigned i = 0; i < p.scale && p.y + sy * p.scale + i < height; ++i)
            std::memcpy(out + size_t(p.y + sy * p.scale + i) * stride, row.data(), row.size());
    }
}

} // namespace mirror_frame
