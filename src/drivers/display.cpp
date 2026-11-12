// SPDX-License-Identifier: AGPL-3.0-only
#include "drivers/display.h"
#include "drivers/display_damage.h"
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
    s.pixels = sent_pixels.load(std::memory_order_relaxed); s.clips = sent_clips.load(std::memory_order_relaxed);
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

void Display::push() {
    using Clock = std::chrono::steady_clock;
    auto start = Clock::now();
    std::vector<display_damage::Rect> rects;
    if (first) rects.push_back({0, 0, width - 1, height - 1});
    else rects = display_damage::plan(pending.data(), shown.data(), width, height);
    if (rects.empty()) return;
    uint64_t count = 0;
    for (const auto& rect : rects) {
        size_t span = size_t(rect.width()) * 4;
        for (int y = rect.y0; y <= rect.y1; ++y) {
            const uint32_t* source = pending.data() + y * width + rect.x0;
            memcpy(static_cast<char*>(pixels) + size_t(y) * pitch + size_t(rect.x0) * 4, source, span);
            memcpy(shown.data() + y * width + rect.x0, source, span);
        }
        drmModeClip clip {uint16_t(rect.x0), uint16_t(rect.y0), uint16_t(rect.x1 + 1), uint16_t(rect.y1 + 1)};
        check(drmModeDirtyFB(fd, framebuffer, &clip, 1) == 0, "Flush screen");
        count += uint64_t(rect.area());
    }
    first = false;
    auto us = uint32_t(std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - start).count());
    pushed.fetch_add(1, std::memory_order_relaxed); sent_pixels.fetch_add(count, std::memory_order_relaxed);
    sent_clips.fetch_add(rects.size(), std::memory_order_relaxed);
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
