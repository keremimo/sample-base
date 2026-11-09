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
        fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
        check(fd >= 0, "Open display");
        check(drmSetMaster(fd) == 0, "Acquire display (stop stock app first)");
        std::unique_ptr<drmModeRes, decltype(&drmModeFreeResources)> resources(drmModeGetResources(fd), drmModeFreeResources);
        check(bool(resources), "Read display resources");
        std::unique_ptr<drmModeConnector, decltype(&drmModeFreeConnector)> connector(nullptr, drmModeFreeConnector);
        for (int i = 0; i < resources->count_connectors; ++i) {
            connector.reset(drmModeGetConnector(fd, resources->connectors[i]));
            if (connector && connector->connection == DRM_MODE_CONNECTED && connector->count_modes) break;
            connector.reset();
        }
        check(bool(connector), "Find connected panel");
        drmModeModeInfo mode = connector->modes[0];
        if (mode.hdisplay != 320 || mode.vdisplay != 240)
            throw std::runtime_error("Expected MPC Sample 320 x 240 panel");
        uint32_t crtc = 0;
        for (int i = 0; i < connector->count_encoders && !crtc; ++i) {
            auto* encoder = drmModeGetEncoder(fd, connector->encoders[i]);
            if (!encoder) continue;
            for (int n = 0; n < resources->count_crtcs; ++n)
                if (encoder->possible_crtcs & (1u << n)) { crtc = resources->crtcs[n]; break; }
            drmModeFreeEncoder(encoder);
        }
        check(crtc != 0, "Find display controller");
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
    if (pixels && pixels != MAP_FAILED) munmap(pixels, bytes);
    if (framebuffer) drmModeRmFB(fd, framebuffer);
    if (handle) { drm_mode_destroy_dumb destroy {}; destroy.handle = handle; drmIoctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy); }
    if (fd >= 0) { drmDropMaster(fd); close(fd); }
    pixels = nullptr; framebuffer = handle = 0; fd = -1;
}
Display::~Display() { release(); }
