// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include "drivers/system_services.h"
#include "drivers/usb_gadget.h"
#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

// SD card access over USB-C: the microSD card lent to a computer as a USB
// drive, as the stock app's Project > SD Card Access does, and taken back.
// edisksd says which volume is the card and mounts and unmounts it; the
// kernel's f_mass_storage serves it to the computer. Unlike the stock app, the gadget keeps its audio, network and MIDI
// functions meanwhile (drivers/usb_gadget.h, gadget_add_storage()).
//
//   Home       the card is the MPC's (mounted by edisksd), or there is none
//   Lending    a request is being carried out: every file system on the
//              card unmounted through edisksd, then the card offered
//   Lent       the computer owns the card; nothing on the MPC may touch it
//   Returning  the medium taken back from the gadget, the card flushed and
//              its partitions read again, edisksd mounting it again
//
// The card is never offered while any of its file systems is mounted on
// the MPC, and never mounted again while the computer may still write it.
// While Lent, `/run/sample-base-card-share` names the card (and the
// gadget's function order before it), so the next
// start, or install/restore-stock.sh after a crash, takes it back.
enum class CardShareState : uint8_t { Home, Lending, Lent, Returning };
constexpr const char* card_share_state_names[] = {"home", "lending", "lent", "returning"};
constexpr const char* card_share_saved = "/run/sample-base-card-share";

struct CardShareStatus {
    CardShareState state = CardShareState::Home;
    bool connected = false; // A computer has configured the USB gadget
    bool ejected = false;   // Lent, and the computer has ejected the drive: safe to take back
    bool locked = false;    // The last take-back found the computer still holding the drive
    std::string disk;       // The card's block device, such as "mmcblk1"
    std::string message;    // Why the last request did not go through, or empty
    uint64_t returns = 0;   // Counts the card coming back: a change means rescan it
};

// The card as edisksd sees it: the first removable SD file system's disk
// (from its SysPath: .../block/mmcblk1/mmcblk1p1 -> "mmcblk1"; a card
// formatted without partitions is its own disk), every file system on that
// disk, and which of them are mounted on the MPC now.
struct CardVolumes { std::string disk; std::vector<StorageVolume> volumes, mounted; };
CardVolumes find_card(const std::vector<StorageVolume>& volumes, const std::string& disk = {});
class CardShare {
public:
    // Paths are parameters for tests; the defaults are the MPC's.
    CardShare(SystemServices& services, std::string sys = "/sys", std::string mountinfo = "/proc/self/mountinfo",
              std::string gadget = gadget_root, std::string saved = card_share_saved);
    // Service thread. Ask to lend the card (Home only; false when there is
    // no card, with the reason in status().message) or to take it back
    // (Lent; `force` even while the computer holds it, which can leave the
    // computer's view of it unfinished).
    bool lend();
    void take_back(bool force = false);
    // Service thread, every pass: carries out what was asked and follows
    // the computer (connected, ejected).
    void poll();
    // At exit: takes the card back at once, whatever the computer does.
    void shutdown();
    const CardShareStatus& status() const { return current; }
    std::string json() const;

private:
    using Clock = std::chrono::steady_clock;
    void do_lend();
    void do_return(bool force);
    void remount();
    std::string read(const std::string& path) const;
    bool mounted_here() const; // Any file system of the card in the kernel's mount table

    SystemServices& services;
    std::string sys, mountinfo, gadget, saved;
    CardShareStatus current;
    enum class Ask : uint8_t { None, Lend, Return, Force } asked = Ask::None;
    std::vector<std::string> order; // The gadget's links before the card was offered
    void save() const;
    Clock::time_point remount_until {}, next_try {}, next_follow {};
};
