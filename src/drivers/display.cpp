// SPDX-License-Identifier: AGPL-3.0-only
#include "drivers/display.h"
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_mode.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <memory>
#include <stdexcept>

static void check(bool ok, const char* what) {
    if (!ok) throw std::runtime_error(std::string(what) + ": " + strerror(errno));
}

Display::Display() {
    try {
        std::unique_ptr<drmModeRes, decltype(&drmModeFreeResources)> resources(nullptr, drmModeFreeResources);
        std::unique_ptr<drmModeConnector, decltype(&drmModeFreeConnector)> connector(nullptr, drmModeFreeConnector);
        drmModeModeInfo mode {};
        for (unsigned index = 0; index < 16 && fd < 0; ++index) {
            std::string path = "/dev/dri/card" + std::to_string(index);
            int candidate = open(path.c_str(), O_RDWR | O_CLOEXEC);
            if (candidate < 0) continue;
            resources.reset(drmModeGetResources(candidate));
            if (resources) for (int n = 0; n < resources->count_connectors && !connector; ++n) {
                std::unique_ptr<drmModeConnector, decltype(&drmModeFreeConnector)> current(drmModeGetConnector(candidate, resources->connectors[n]), drmModeFreeConnector);
                if (!current || current->connection != DRM_MODE_CONNECTED) continue;
                for (int m = 0; m < current->count_modes; ++m) if (current->modes[m].hdisplay == 320 && current->modes[m].vdisplay == 240) {
                    mode = current->modes[m]; connector = std::move(current); break;
                }
            }
            if (connector) fd = candidate;
            else close(candidate);
        }
        check(fd >= 0 && bool(resources) && bool(connector), "Find MPC 320 x 240 panel");
        check(drmSetMaster(fd) == 0, "Acquire display (stop stock app first)");
        uint32_t crtc = 0;
        for (int i = 0; i < connector->count_encoders && !crtc; ++i) {
            auto* encoder = drmModeGetEncoder(fd, connector->encoders[i]);
            if (!encoder) continue;
            for (int n = 0; n < resources->count_crtcs; ++n)
                if (encoder->possible_crtcs & (1u << n)) { crtc = resources->crtcs[n]; break; }
            drmModeFreeEncoder(encoder);
        }
        check(crtc != 0, "Find display controller");
        active_crtc = crtc; connector_id = connector->connector_id;
        previous = drmModeGetCrtc(fd, crtc);
        drm_mode_create_dumb create {};
        create.width = 320; create.height = 240; create.bpp = 32;
        check(drmIoctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &create) == 0, "Allocate screen buffer");
        handle = create.handle; pitch = create.pitch; bytes = create.size;
        check(drmModeAddFB(fd, 320, 240, 24, 32, pitch, handle, &framebuffer) == 0, "Register screen buffer");
        drm_mode_map_dumb map {}; map.handle = handle;
        check(drmIoctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &map) == 0, "Map screen buffer");
        pixels = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, map.offset);
        check(pixels != MAP_FAILED, "Map screen memory");
        memset(pixels, 0, bytes);
        check(drmModeSetCrtc(fd, crtc, framebuffer, 0, 0, &connector->connector_id, 1, &mode) == 0, "Activate screen");
        changed = true;
        pending.assign(size_t(width) * height, 0);
        shown.assign(size_t(width) * height, 0);
        latest.assign(size_t(width) * height, 0);
        worker = std::thread([this] { run(); });
    } catch (...) { release(); throw; }
}

bool Display::present(const juce::Image& image) {
    juce::Image::BitmapData bitmap(image, juce::Image::BitmapData::readOnly);
    if (bitmap.width != width || bitmap.height != height || bitmap.pixelStride != 4)
        throw std::runtime_error("Invalid JUCE screen image");
    presented.fetch_add(1, std::memory_order_relaxed);
    bool same = !first_present;
    for (int row = 0; row < height; ++row) {
        auto* line = bitmap.getLinePointer(row);
        auto* kept = latest.data() + row * width;
        if (memcmp(kept, line, width * 4) != 0) { memcpy(kept, line, width * 4); same = false; }
    }
    first_present = false;
    std::unique_lock<std::mutex> guard(lock);
    if (failure) std::rethrow_exception(failure);
    if (same) return false;
    wake.wait(guard, [&] { return (!queued && !busy) || failure; });
    if (failure) std::rethrow_exception(failure);
    pending = latest;
    queued = true;
    wake.notify_all();
    return true;
}

Display::Stats Display::stats() const {
    Stats s;
    s.presented = presented.load(std::memory_order_relaxed); s.pushed = pushed.load(std::memory_order_relaxed);
    s.pixels = sent_pixels.load(std::memory_order_relaxed);
    s.last_push_us = last_push_us.load(std::memory_order_relaxed); s.max_push_us = max_push_us.load(std::memory_order_relaxed);
    s.full_push_us = full_push_us.load(std::memory_order_relaxed);
    return s;
}

void Display::run() {
    std::unique_lock<std::mutex> guard(lock);
    while (true) {
        wake.wait(guard, [&] { return queued || stopping; });
        if (stopping) return;
        queued = false; busy = true;
        guard.unlock();
        std::exception_ptr error;
        try { push(); } catch (...) { error = std::current_exception(); }
        guard.lock();
        busy = false;
        if (error) failure = error;
        wake.notify_all();
        if (error) return;
    }
}

namespace {
// Rows y0..y1 and columns x0..x1 of the screen, inclusive.
struct Band { int y0, y1, x0, x1; };
// Bands closer than this many rows go out as one: each push costs a commit
// and a command header on the panel, so a few spare rows are cheaper.
constexpr int band_gap = 8;
constexpr size_t most_bands = 4;
}

void Display::push() {
    using Clock = std::chrono::steady_clock;
    auto start = Clock::now();
    std::vector<Band> bands;
    bool whole = first;
    if (whole) bands.push_back({0, height - 1, 0, width - 1});
    else for (int y = 0; y < height; ++y) {
        const uint32_t* now = pending.data() + y * width;
        const uint32_t* was = shown.data() + y * width;
        if (memcmp(now, was, width * 4) == 0) continue;
        int left = 0, right = width - 1;
        while (now[left] == was[left]) ++left;
        while (now[right] == was[right]) --right;
        if (!bands.empty() && y - bands.back().y1 <= band_gap) {
            auto& band = bands.back();
            band.y1 = y; band.x0 = std::min(band.x0, left); band.x1 = std::max(band.x1, right);
        } else bands.push_back({y, y, left, right});
    }
    // Too many bands: join the two neighbours with the smallest gap.
    while (bands.size() > most_bands) {
        size_t join = 0;
        for (size_t n = 1; n + 1 < bands.size(); ++n)
            if (bands[n + 1].y0 - bands[n].y1 < bands[join + 1].y0 - bands[join].y1) join = n;
        auto& a = bands[join]; const auto& b = bands[join + 1];
        a.y1 = b.y1; a.x0 = std::min(a.x0, b.x0); a.x1 = std::max(a.x1, b.x1);
        bands.erase(bands.begin() + long(join) + 1);
    }
    if (bands.empty()) return;
    uint64_t count = 0;
    for (const auto& band : bands) {
        size_t span = size_t(band.x1 - band.x0 + 1) * 4;
        for (int y = band.y0; y <= band.y1; ++y) {
            const uint32_t* source = pending.data() + y * width + band.x0;
            memcpy(static_cast<char*>(pixels) + size_t(y) * pitch + size_t(band.x0) * 4, source, span);
            memcpy(shown.data() + y * width + band.x0, source, span);
        }
        drmModeClip clip {uint16_t(band.x0), uint16_t(band.y0), uint16_t(band.x1 + 1), uint16_t(band.y1 + 1)};
        check(drmModeDirtyFB(fd, framebuffer, &clip, 1) == 0, "Flush screen");
        count += uint64_t(band.x1 - band.x0 + 1) * uint64_t(band.y1 - band.y0 + 1);
    }
    first = false;
    auto us = uint32_t(std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - start).count());
    pushed.fetch_add(1, std::memory_order_relaxed); sent_pixels.fetch_add(count, std::memory_order_relaxed);
    last_push_us.store(us, std::memory_order_relaxed);
    if (us > max_push_us.load(std::memory_order_relaxed)) max_push_us.store(us, std::memory_order_relaxed);
    if (count == uint64_t(width) * height) full_push_us.store(us, std::memory_order_relaxed);
}

void Display::release() {
    if (worker.joinable()) {
        { std::lock_guard<std::mutex> guard(lock); stopping = true; }
        wake.notify_all();
        worker.join();
    }
    if (fd >= 0 && changed) {
        if (previous && previous->mode_valid)
            drmModeSetCrtc(fd, previous->crtc_id, previous->buffer_id, previous->x, previous->y, &connector_id, 1, &previous->mode);
        else drmModeSetCrtc(fd, active_crtc, 0, 0, 0, nullptr, 0, nullptr);
    }
    if (previous) drmModeFreeCrtc(previous);
    previous = nullptr; changed = false;
    if (pixels && pixels != MAP_FAILED) munmap(pixels, bytes);
    if (framebuffer) drmModeRmFB(fd, framebuffer);
    if (handle) { drm_mode_destroy_dumb destroy {}; destroy.handle = handle; drmIoctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy); }
    if (fd >= 0) { drmDropMaster(fd); close(fd); }
    pixels = nullptr; framebuffer = handle = 0; fd = -1;
}
Display::~Display() { release(); }
