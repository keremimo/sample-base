// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include "drivers/usb_gadget.h"
#include <cstdint>
#include <memory>
#include <string>

namespace juce { class Image; }

// Screen Mirror: the screen as a USB webcam over USB-C, so a computer (or a
// tablet that takes USB cameras) shows it in any camera app. The MPC offers
// one picture size, the one asked for, always the screen scaled by a whole
// number (drivers/mirror_frame.h): 320 x 240 (the screen itself) and
// 640 x 480 as YUYV, every pixel exact; the larger sizes made JPEG by the
// RK3566's encoder (drivers/jpeg_encoder.h), USB 2.0 having no room for them
// uncompressed. 30 pictures a second, a picture made only when the screen
// changed.
//
// Its own thread does the work: loads kernel/uvc's webcam function
// (sample_base_uvc, from inside the binary) and, for JPEG, kernel/vepu's
// encoder switch (sample_base_vepu, never unloaded), links the webcam into
// the USB gadget (gadget_add_mirror(); the gadget rebinds, so USB audio, MIDI
// and SSH drop for about a second), answers the computer and sends the
// pictures. Turning it off unlinks the webcam and unloads its module.
//
// The camera is never unplugged under a computer showing it (an app such as
// OBS may not survive it): while one streams, turning it off or another size
// waits until the computer stops (ScreenMirrorStatus::held), the card is not
// lent (CardShare::lend() refuses: the webcam would have to step out) and the
// USB-C audio rate does not follow the device (both rebind the gadget);
// gadget_mirror_live() says so to them. When the app stops, the webcam stays
// in the gadget, so a restart does not rebind: a computer streaming sees its
// picture stand still and the new process sends again (kernel/uvc replays
// STREAMON to it). install/restore-stock.sh takes the webcam out for the
// stock app unless a computer was showing it.
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
    bool fallback = false;  // a JPEG size was asked for and no encoder came up: 640 x 480 YUYV instead
    bool held = false;      // the app asked for a change (off, another size) that waits: a computer shows the camera
    uint64_t frames = 0;    // pictures sent since start
    std::string error;
};

class ScreenMirror {
public:
    // The sizes offered (the app's setting picks one), each the screen times a whole number: 1, 2 (YUYV), 3 (with
    // black at the sides), 4, 6, 8 (MJPEG).
    static constexpr unsigned size_count = 6;
    static constexpr unsigned widths[size_count] {320, 640, 1280, 1280, 1920, 2560}, heights[size_count] {240, 480, 720, 960, 1440, 1920};
    static bool uncompressed(unsigned width) { return width <= 640; }

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
