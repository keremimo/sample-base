// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <juce_graphics/juce_graphics.h>
#include <cstdint>

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
    void* pixels = nullptr;
    void release();
};
