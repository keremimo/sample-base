// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <string>
#include <vector>

// The stock USB gadget's audio function, in configfs. The stock
// az0x-usb-gadget@ac50 service builds the gadget at boot with a UAC2 function
// whose computer-bound (playback) side is stereo, beside USB networking and
// USB MIDI. f_uac2 takes a new channel mask only while no configuration
// links the function, so widening it unbinds the gadget from its USB
// controller, takes every function out of its configuration, writes the
// mask, links them back in their old order (configfs lists links newest
// first) and binds the gadget again: the computer sees the MPC leave and
// come back, and SSH and MIDI over USB drop for about a second. The kernel
// (drivers/usb/gadget/function/f_uac2.c) allows at most 27 channels a side.
//
// Its rate (p_srate and c_srate, equal: the stock gadget shares one clock
// between the two directions) changes the same way: set_uac2_rate() makes
// the computer record and play at the rate the audio device runs at.
//
// Before the first change the original mask and rate are saved as one line,
// "<function directory> <mask> <rate>", which install/restore-stock.sh reads
// to give the stock app its stereo 44.1 kHz gadget back.

struct GadgetChange {
    enum class Result { Missing, Unchanged, Changed, Failed } result = Result::Missing;
    std::string function; // The UAC2 function's configfs directory, or empty
    bool rate = false;              // A rate change: `before` and `after` are rates, not masks
    unsigned before = 0, after = 0; // Playback channel masks, or rates
    std::string error;
};

constexpr const char* gadget_root = "/sys/kernel/config/usb_gadget";
constexpr const char* gadget_saved_mask = "/run/sample-base-uac2-p_chmask";

// Sets the first UAC2 function's playback side to `channels` channels
// (1-27), unbinding and rebinding its gadget only when the mask differs.
// On failure the gadget is bound again when it was bound before.
GadgetChange widen_uac2_playback(unsigned channels, const std::string& root = gadget_root,
                                 const std::string& saved = gadget_saved_mask);
// Sets the first UAC2 function's rate, both ways, to `rate` (8 to 192 kHz),
// unbinding and rebinding its gadget only when it differs. On failure the
// old rate goes back.
GadgetChange set_uac2_rate(unsigned rate, const std::string& root = gadget_root, const std::string& saved = gadget_saved_mask);
std::string describe(const GadgetChange& change);

// The SD card offered to a computer as a USB drive, beside the gadget's
// audio, network and MIDI functions (the stock firmware's own SD Card Access
// replaces the whole gadget with one mass-storage function instead). The
// function is made with one removable LUN, its medium a block device, and
// linked into the gadget's first configuration; linking it rebinds the gadget like the
// stems do (SSH, MIDI and audio over USB drop for about a second).
constexpr const char* card_function = "mass_storage.sd";

// Makes and links the function with `medium` (such as "/dev/mmcblk1") as its
// medium; with the function already linked only the medium changes. The
// caller must have unmounted every file system on the medium first. The
// network function is linked again after it (the controller's last
// transmit FIFO is too small for a bulk endpoint; see usb_gadget.cpp);
// `order` gets the links' names as they were, for gadget_remove_storage().
bool gadget_add_storage(const std::string& medium, std::vector<std::string>& order, std::string& error,
                        const std::string& root = gadget_root);
// The medium offered now ("" once the computer ejected it, or none);
// `present` says whether the function exists at all.
std::string gadget_storage_medium(bool& present, const std::string& root = gadget_root);
// Takes the medium back from the computer. Locked: the computer holds it
// (mounted there) and `force` was not given; nothing changed.
enum class GadgetEject { Done, Locked, Failed };
GadgetEject gadget_eject_storage(bool force, std::string& error, const std::string& root = gadget_root);
// Unlinks and removes the function, the other links back in `order` (the
// gadget rebinds once more). A function the drive put out (Screen Mirror's
// webcam) is linked again when it still exists.
bool gadget_remove_storage(const std::vector<std::string>& order, std::string& error, const std::string& root = gadget_root);

// Screen Mirror's USB webcam (drivers/screen_mirror.h): kernel/uvc's "uvc"
// function offering one picture size at 30 fps, YUYV or MJPEG. It is linked
// after the UAC2 function (the audio interfaces keep their numbers, by which
// macOS knows the device) and before the network function, which stays last
// (the controller's sixth transmit FIFO fits only its notify endpoint). A
// drive offered by gadget_add_storage() would need a seventh IN endpoint, for
// which the controller has no FIFO: the webcam steps out while the card is
// lent. Every relink in this file takes one lock, so the service thread and
// the mirror's own thread never change the gadget at once.
constexpr const char* mirror_function = "uvc.screen";
struct MirrorFormat {
    unsigned width = 320, height = 240;
    bool mjpeg = false; // else YUYV
    bool operator==(const MirrorFormat& other) const { return width == other.width && height == other.height && mjpeg == other.mjpeg; }
    bool operator!=(const MirrorFormat& other) const { return !(*this == other); }
    // A picture's bytes at most: YUYV's two a pixel, a JPEG's one (the screen's JPEGs are far smaller).
    unsigned frame_bytes() const { return mjpeg ? width * height : width * height * 2; }
    // The streaming endpoint's bytes a microframe (125 us): one 1024-byte packet (8 MB/s), or two or three for
    // a YUYV picture that needs more at 30 fps (640 x 480: 2304 bytes, so 3072, 24.6 MB/s). A JPEG of a playing
    // project is about 340 kB at 1920 x 1440 (10 MB/s at 30 fps; one packet held it to 24 fps): JPEG gets three.
    // USB 2.0 keeps 6000 bytes a microframe for these; the 18-channel audio takes about 1040 at 96 kHz.
    unsigned packet_bytes() const {
        if (mjpeg) return 3072;
        unsigned need = unsigned(uint64_t(frame_bytes()) * 30 / 8000) + 16; // + the payload headers
        return need <= 1024 ? 1024 : need <= 2048 ? 2048 : 3072;
    }
};
enum class MirrorLink { Linked, CardShared, Failed };
// Present while a computer streams the webcam (ScreenMirror writes it, and
// leaves it when the app stops mid-stream: the computer still shows it).
// What would rebind the gadget waits while it is there.
constexpr const char* mirror_live_marker = "/run/sample-base-mirror-live";
constexpr const char* mirror_in_use = "the camera is in use on the computer";
bool gadget_mirror_live(const std::string& marker = mirror_live_marker);
// The webcam function's picture size and kind when it exists, linked or not.
bool gadget_mirror_format(MirrorFormat& format, const std::string& root = gadget_root);
// The function exists and was made for `format` as this build makes it (its packet size too).
bool gadget_mirror_made_for(const MirrorFormat& format, const std::string& root = gadget_root);
// Makes the function for `format` (one made for another size goes first) and
// links it; the gadget rebinds unless it is linked for `format` already.
// Needs kernel/uvc's module loaded. CardShared: the drive is linked, nothing
// changed.
MirrorLink gadget_add_mirror(const MirrorFormat& format, std::string& error, const std::string& root = gadget_root);
bool gadget_mirror_linked(const std::string& root = gadget_root);
// Unlinks the function (the gadget rebinds) and removes it.
bool gadget_remove_mirror(std::string& error, const std::string& root = gadget_root);
