// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <juce_graphics/juce_graphics.h>
#include <cstdint>
#include <xf86drmMode.h>

class Display {
public:
    Display();
    ~Display();
    Display(const Display&) = delete;
    Display& operator=(const Display&) = delete;
    void present(const juce::Image& image);
private:
    int fd = -1;
    uint32_t handle = 0, framebuffer = 0, pitch = 0;
    uint64_t bytes = 0;
    uint32_t active_crtc = 0, connector_id = 0;
    drmModeCrtc* previous = nullptr;
    bool changed = false;
    void* pixels = nullptr;
    void release();
};
