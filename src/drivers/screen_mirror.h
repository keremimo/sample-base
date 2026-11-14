// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include "drivers/usb_gadget.h"
#include <cstdint>
#include <memory>
#include <string>

namespace juce { class Image; }

// Screen Mirror: the screen as a USB webcam over USB-C, so a computer (or a
// tablet that takes USB cameras) shows it in any camera app. The MPC offers
// one picture size, the one asked for: 320 x 240 is the screen itself
// (YUYV), the larger sizes are the screen scaled by a whole number
// (drivers/mirror_frame.h) and made JPEG by the RK3566's encoder
// (drivers/jpeg_encoder.h); 30 pictures a second, a picture made only when
// the screen changed.
//
// Its own thread does the work: loads kernel/uvc's webcam function
// (sample_base_uvc, from inside the binary) and, for JPEG, kernel/vepu's
// encoder switch (sample_base_vepu, never unloaded), links the webcam into
// the USB gadget (gadget_add_mirror(); the gadget rebinds, so USB audio, MIDI
// and SSH drop for about a second), answers the computer and sends the
// pictures. Turning it off unlinks the webcam and unloads its module. The
// webcam steps out while the card is lent over USB (gadget_add_storage())
// and comes back after. When the app stops, the webcam stays in the gadget
// and refuses the computer (kernel/uvc's change), so a restart does not
// rebind; install/restore-stock.sh takes it out for the stock app.
struct ScreenMirrorMode {
    bool on = false;
    unsigned width = 1280, height = 720;
    bool operator==(const ScreenMirrorMode& o) const { return on == o.on && width == o.width && height == o.height; }
    bool operator!=(const ScreenMirrorMode& o) const { return !(*this == o); }
};

enum class MirrorState : uint8_t {
    Off,        // no webcam in the gadget
    Starting,   // modules, encoder, gadget
    Waiting,    // offered; no computer is showing it
    Streaming,  // a computer takes the pictures
    CardShared, // stepped out while the card is lent
    Failed,     // see error; tried again after a change of mode
};
const char* mirror_state_name(MirrorState state);

struct ScreenMirrorStatus {
    MirrorState state = MirrorState::Off;
    MirrorFormat format;    // what the computer is offered
    bool fallback = false;  // a larger size was asked for and no encoder came up: 320 x 240 instead
    uint64_t frames = 0;    // pictures sent since start
    std::string error;
};

class ScreenMirror {
public:
    // The sizes offered (the app's setting picks one): 320 x 240 as YUYV, the rest as MJPEG.
    static constexpr unsigned size_count = 4;
    static constexpr unsigned widths[size_count] {320, 640, 1280, 1280}, heights[size_count] {240, 480, 720, 960};

    // `initial`: the app's mode at start, so that a restart keeps a webcam
    // already linked for it (no rebind) instead of taking it out first.
    explicit ScreenMirror(const ScreenMirrorMode& initial);
    ~ScreenMirror();
    ScreenMirror(const ScreenMirror&) = delete;
    ScreenMirror& operator=(const ScreenMirror&) = delete;

    // Any thread: what the app wants. Acted on within a few tenths of a second.
    void set_mode(const ScreenMirrorMode& mode);
    // Screen thread: the picture the panel now shows (320 x 240 ARGB), after
    // a change; wants_frame() asks for one when the mirror has none yet.
    bool wants_frame() const;
    void offer(const juce::Image& screen);
    ScreenMirrorStatus status() const;

private:
    struct Worker;
    std::unique_ptr<Worker> worker;
};
