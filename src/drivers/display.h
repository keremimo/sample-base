// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <juce_graphics/juce_graphics.h>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <mutex>
#include <thread>
#include <vector>
#include <xf86drmMode.h>

// The 320 x 240 SPI panel. The panel has no vsync the kernel can report (its
// mode has no refresh rate and no tearing-effect line is wired), so the only
// timing it gives is how long a push over SPI takes. A worker thread does the
// push: present() hands it a frame and returns, and waits first only while
// the previous frame is still going out. The screen thread therefore draws
// one frame while the panel receives the last, at whatever rate the panel
// takes. Only the rows and columns that changed are sent.
class Display {
public:
    static constexpr int width = 320, height = 240;
    struct Stats {
        uint64_t presented = 0; // Frames handed to present(), changed or not
        uint64_t pushed = 0;    // Frames that changed and went to the panel
        uint64_t pixels = 0;    // Pixels sent, 2 bytes each over SPI
        uint32_t last_push_us = 0, max_push_us = 0, full_push_us = 0; // full: the latest whole-screen push
    };
    Display();
    ~Display();
    Display(const Display&) = delete;
    Display& operator=(const Display&) = delete;
    // Screen thread: queues `image` (ARGB, 320 x 240) for the panel. Blocks
    // only until the previous frame has gone out. Returns false, without
    // waiting, when `image` matches the last frame queued. Throws when a
    // push failed.
    bool present(const juce::Image& image);
    Stats stats() const;
private:
    int fd = -1;
    uint32_t handle = 0, framebuffer = 0, pitch = 0;
    uint64_t bytes = 0;
    uint32_t active_crtc = 0, connector_id = 0;
    drmModeCrtc* previous = nullptr;
    bool changed = false;
    void* pixels = nullptr;

    // Worker hand-off: `pending` is the next frame, `shown` what the panel holds.
    std::vector<uint32_t> pending, shown, latest; // latest: the last frame queued, screen thread only
    std::mutex lock;
    std::condition_variable wake;
    bool queued = false, busy = false, stopping = false, first = true;
    bool first_present = true; // Screen thread only
    std::exception_ptr failure;
    std::thread worker;
    std::atomic<uint64_t> presented {0}, pushed {0}, sent_pixels {0};
    std::atomic<uint32_t> last_push_us {0}, max_push_us {0}, full_push_us {0};

    void run();
    void push(); // Worker: sends what differs between `pending` and `shown`.
    void release();
};
