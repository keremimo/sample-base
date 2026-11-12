// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

// What Display sends the panel between two frames, and in what order. The
// panel (docs/HARDWARE.md, Display) is a 240 x 320 portrait one turned to
// landscape (MADCTL MY | MV): it refreshes itself from its own memory a
// screen column at a time, at its own rate, with no tearing-effect line,
// while a push writes its rectangle a screen row at a time. A tall
// rectangle written as the refresh passes over it shows its top rows new
// and its bottom rows old, so a moving line (a play head) breaks in two.
//
// So: rows that changed are gathered in bands; in each band the columns that
// changed in clusters, each its own push unless the gap to the next costs
// less to send than a push does; and a cluster narrow and tall enough to be a
// line goes a column at a time, columns gaining light first and each set right
// to left. A column is then whole on the panel whenever the refresh meets it,
// and a refresh caught between columns sees a line in both places (its new
// place drawn, its old not yet gone) rather than broken or missing.
namespace display_damage {

struct Rect {
    int x0, y0, x1, y1; // inclusive
    int width() const { return x1 - x0 + 1; }
    int height() const { return y1 - y0 + 1; }
    long area() const { return long(width()) * height(); }
};

// Rows closer than this go out in one band: the gap costs less than a push.
constexpr int band_gap = 8;
// What a push costs besides its pixels (a commit and the window commands), in
// bytes at the panel's 25 MHz: about 0.3 ms.
constexpr long push_bytes = 1024;
// At most this many rectangles a frame before lines are cut into columns;
// past it the joins that add the fewest pixels are made.
constexpr size_t most_rects = 8;
// A cluster at most line_width wide and at least line_height tall is a line;
// at most line_columns columns a frame go one at a time, the rest as rectangles.
constexpr int line_width = 16, line_height = 24, line_columns = 24;

inline int luma(uint32_t p) { return int((p >> 16) & 0xff) * 2 + int((p >> 8) & 0xff) * 5 + int(p & 0xff); }

inline Rect joined(const Rect& a, const Rect& b) {
    return {std::min(a.x0, b.x0), std::min(a.y0, b.y0), std::max(a.x1, b.x1), std::max(a.y1, b.y1)};
}

// `now` and `was` are width x height ARGB frames, a row after another. Every
// pixel that differs is in at least one rectangle; nothing else is promised
// to be left out.
// Without `lines`, no cluster is cut into columns (the writes already run along the refresh).
inline std::vector<Rect> plan(const uint32_t* now, const uint32_t* was, int width, int height, bool lines = true) {
    std::vector<Rect> bands;
    for (int y = 0; y < height; ++y) {
        if (!std::memcmp(now + size_t(y) * width, was + size_t(y) * width, size_t(width) * 4)) continue;
        if (!bands.empty() && y - bands.back().y1 <= band_gap) bands.back().y1 = y;
        else bands.push_back({0, y, width - 1, y});
    }
    std::vector<Rect> rects;
    std::vector<int> top(static_cast<size_t>(width)), bottom(static_cast<size_t>(width));
    for (const auto& band : bands) {
        std::fill(top.begin(), top.end(), -1);
        for (int y = band.y0; y <= band.y1; ++y) {
            const uint32_t* a = now + size_t(y) * width;
            const uint32_t* b = was + size_t(y) * width;
            for (int x = 0; x < width; ++x)
                if (a[x] != b[x]) { if (top[size_t(x)] < 0) top[size_t(x)] = y; bottom[size_t(x)] = y; }
        }
        const long rows = band.height();
        Rect cluster {-1, 0, -1, 0};
        for (int x = 0; x < width; ++x) {
            if (top[size_t(x)] < 0) continue;
            if (cluster.x0 >= 0 && long(x - cluster.x1 - 1) * rows * 2 > push_bytes) { rects.push_back(cluster); cluster.x0 = -1; }
            if (cluster.x0 < 0) cluster = {x, top[size_t(x)], x, bottom[size_t(x)]};
            else { cluster.x1 = x; cluster.y0 = std::min(cluster.y0, top[size_t(x)]); cluster.y1 = std::max(cluster.y1, bottom[size_t(x)]); }
        }
        if (cluster.x0 >= 0) rects.push_back(cluster);
    }
    while (rects.size() > most_rects) {
        size_t a = 0, b = 1;
        long best = -1;
        for (size_t i = 0; i < rects.size(); ++i)
            for (size_t j = i + 1; j < rects.size(); ++j) {
                long added = joined(rects[i], rects[j]).area() - rects[i].area() - rects[j].area();
                if (best < 0 || added < best) { best = added; a = i; b = j; }
            }
        rects[a] = joined(rects[a], rects[b]);
        rects.erase(rects.begin() + long(b));
    }
    std::vector<Rect> out;
    int columns = 0;
    struct Column { int x, y0, y1, gain; };
    std::vector<Column> line;
    for (const auto& r : rects) {
        if (!lines || r.width() > line_width || r.height() < line_height || columns + r.width() > line_columns) { out.push_back(r); continue; }
        columns += r.width();
        line.clear();
        for (int x = r.x0; x <= r.x1; ++x) {
            Column c {x, -1, -1, 0};
            for (int y = r.y0; y <= r.y1; ++y) {
                uint32_t a = now[size_t(y) * width + x], b = was[size_t(y) * width + x];
                if (a == b) continue;
                if (c.y0 < 0) c.y0 = y;
                c.y1 = y;
                c.gain += luma(a) - luma(b);
            }
            if (c.y0 >= 0) line.push_back(c);
        }
        std::stable_sort(line.begin(), line.end(), [](const Column& p, const Column& q) {
            if ((p.gain > 0) != (q.gain > 0)) return p.gain > 0;
            return p.x > q.x;
        });
        for (const auto& c : line) out.push_back({c.x, c.y0, c.x, c.y1});
    }
    return out;
}

} // namespace display_damage
