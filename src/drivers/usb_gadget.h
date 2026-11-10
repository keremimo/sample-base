// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <string>

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
// Before the first change the original mask is saved as one line,
// "<function directory> <mask>", which install/restore-stock.sh reads to
// give the stock app its stereo gadget back.

struct GadgetChange {
    enum class Result { Missing, Unchanged, Changed, Failed } result = Result::Missing;
    std::string function; // The UAC2 function's configfs directory, or empty
    unsigned before = 0, after = 0; // Playback channel masks
    std::string error;
};

constexpr const char* gadget_root = "/sys/kernel/config/usb_gadget";
constexpr const char* gadget_saved_mask = "/run/sample-base-uac2-p_chmask";

// Sets the first UAC2 function's playback side to `channels` channels
// (1-27), unbinding and rebinding its gadget only when the mask differs.
// On failure the gadget is bound again when it was bound before.
GadgetChange widen_uac2_playback(unsigned channels, const std::string& root = gadget_root,
                                 const std::string& saved = gadget_saved_mask);
std::string describe(const GadgetChange& change);
