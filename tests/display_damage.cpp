// SPDX-License-Identifier: AGPL-3.0-only
#include "drivers/display_damage.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <vector>

using display_damage::Rect;

namespace {
constexpr int width = 320, height = 240;
constexpr uint32_t dark = 0xff101010, light = 0xffe0e0e0, mid = 0xff606060;

struct Frame {
    std::vector<uint32_t> px = std::vector<uint32_t>(size_t(width) * height, dark);
    void fill(int x0, int y0, int x1, int y1, uint32_t c) {
        for (int y = y0; y <= y1; ++y) for (int x = x0; x <= x1; ++x) px[size_t(y) * width + x] = c;
    }
};

// Every pixel that differs is sent, and nothing outside the screen.
void covers(const Frame& now, const Frame& was, const std::vector<Rect>& rects) {
    std::vector<bool> sent(size_t(width) * height);
    for (const auto& r : rects) {
        assert(r.x0 >= 0 && r.y0 >= 0 && r.x1 < width && r.y1 < height && r.x0 <= r.x1 && r.y0 <= r.y1);
        for (int y = r.y0; y <= r.y1; ++y) for (int x = r.x0; x <= r.x1; ++x) sent[size_t(y) * width + x] = true;
    }
    for (size_t n = 0; n < sent.size(); ++n) assert(now.px[n] == was.px[n] || sent[n]);
}

std::vector<Rect> plan(const Frame& now, const Frame& was) {
    auto rects = display_damage::plan(now.px.data(), was.px.data(), width, height);
    covers(now, was, rects);
    return rects;
}
}

int main() {
    { // nothing changed: nothing sent
        Frame a;
        assert(plan(a, a).empty());
    }
    { // a play head moves right over a waveform: a column at a time, its new place first, each set right to left
        Frame was, now;
        was.fill(0, 50, 319, 149, mid); now.fill(0, 50, 319, 149, mid);
        was.fill(100, 46, 101, 149, light);
        now.fill(103, 46, 104, 149, light);
        auto rects = plan(now, was);
        assert(rects.size() == 4);
        for (const auto& r : rects) assert(r.width() == 1 && r.y0 == 46 && r.y1 == 149);
        assert(rects[0].x0 == 104 && rects[1].x0 == 103 && rects[2].x0 == 101 && rects[3].x0 == 100);
    }
    { // moving left (reversed): still its new place first
        Frame was, now;
        was.fill(103, 46, 104, 149, light);
        now.fill(100, 46, 101, 149, light);
        auto rects = plan(now, was);
        assert(rects.size() == 4 && rects[0].x0 == 101 && rects[1].x0 == 100 && rects[2].x0 == 104 && rects[3].x0 == 103);
    }
    { // a head and a meter far off in the same rows: two pushes, not one as wide as both
        Frame was, now;
        was.fill(100, 50, 101, 149, light); now.fill(102, 50, 103, 149, light);
        was.fill(300, 60, 305, 70, light); now.fill(300, 64, 305, 70, light); // the meter falls: only its top rows change
        auto rects = plan(now, was);
        int head = 0, meter = 0;
        for (const auto& r : rects) {
            if (r.x1 <= 110) { assert(r.width() == 1); ++head; }
            else { assert(r.x0 == 300 && r.x1 == 305 && r.y0 == 60 && r.y1 == 63); ++meter; }
        }
        assert(head == 4 && meter == 1);
    }
    { // text: wide and short, one rectangle as tight as the change
        Frame was, now;
        now.fill(20, 10, 180, 20, light);
        auto rects = plan(now, was);
        assert(rects.size() == 1 && rects[0].x0 == 20 && rects[0].x1 == 180 && rects[0].y0 == 10 && rects[0].y1 == 20);
    }
    { // a short narrow change (a dot): not a line, one rectangle
        Frame was, now;
        now.fill(40, 40, 43, 45, light);
        auto rects = plan(now, was);
        assert(rects.size() == 1 && rects[0].width() == 4 && rects[0].height() == 6);
    }
    { // a whole screen
        Frame was, now;
        now.fill(0, 0, width - 1, height - 1, light);
        auto rects = plan(now, was);
        assert(rects.size() == 1 && rects[0].area() == long(width) * height);
    }
    { // scattered changes: a handful of rectangles at most, lines cut into a bounded number of columns
        std::srand(7);
        for (unsigned round = 0; round < 40; ++round) {
            Frame was, now;
            for (unsigned n = 0; n < 60; ++n) {
                int x = std::rand() % width, y = std::rand() % height;
                int w = 1 + std::rand() % 12, h = 1 + std::rand() % 60;
                now.fill(x, y, std::min(width - 1, x + w), std::min(height - 1, y + h), std::rand() % 2 ? light : mid);
            }
            auto rects = plan(now, was); // at most most_rects, one of them cut into at most line_columns columns
            assert(rects.size() <= display_damage::most_rects - 1 + size_t(display_damage::line_columns));
        }
    }
    std::puts("display damage ok");
    return 0;
}
