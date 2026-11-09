// SPDX-License-Identifier: AGPL-3.0-only
#include "display.h"
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_mode.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
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
    } catch (...) { release(); throw; }
}

void Display::present(const juce::Image& image) {
    juce::Image::BitmapData bitmap(image, juce::Image::BitmapData::readOnly);
    if (bitmap.width != 320 || bitmap.height != 240 || bitmap.pixelStride != 4)
        throw std::runtime_error("Invalid JUCE screen image");
    for (int row = 0; row < 240; ++row)
        memcpy(static_cast<char*>(pixels) + row * pitch, bitmap.getLinePointer(row), 320 * 4);
    check(drmModeDirtyFB(fd, framebuffer, nullptr, 0) == 0, "Flush screen");
}

void Display::release() {
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
