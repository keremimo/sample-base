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
// gadget rebinds once more).
bool gadget_remove_storage(const std::vector<std::string>& order, std::string& error, const std::string& root = gadget_root);
