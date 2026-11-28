// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "drivers/card_share.h"
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

// File Sharing: the microSD card as a network drive (SMB 2 and 3) for the
// computer on the USB-C network, at smb://192.168.155.1 (or an app's own
// name for that address). Unlike the card share's USB drive
// (drivers/card_share.h), the card stays the MPC's: both use it at once,
// through the same file system.
//
// The kernel's SMB server moves the files: ksmbd, which the MPC's kernel is
// built without, built by sample-base (kernel/ksmbd, kernel/ksmbd-6.18) and
// kept in the binary (src/drivers/smb_module.S). The server asks a process
// over generic netlink ("SMBD_GENL") for all else, and FileShare is that
// process, on a thread of its own (the server waits 2 s for each answer):
//   start   the server's settings: port 445, the USB-C network's interface
//           only, the app's name, signing when the computer asks for it
//   login   "guest" (or no name) a guest; any other name a user with an
//           empty password. Both with root's rights (exFAT has no owners:
//           every file is root's)
//   shares  "SD Card", the card's folder, read-write, while the card is the
//           MPC's; IPC$, where srvsvc lists the shares (drivers/smb_pipe.h)
//   trees   a computer connecting to a share; while one has "SD Card"
//           open, the card is not lent as a USB drive (CardUsers)
//
// The module is never unloaded. Off stops the server (/sys/class/
// ksmbd-control/kill_server, which closes every connection); on starts it
// again. A new run (a restart, a deploy) finds the server running and takes
// it over, so the computer keeps its connection; when no run answers, the
// server's heartbeat stops it 10 to 20 s later. A server stopped under a run
// (kill_server from elsewhere) is started again within a second.
// The card leaving (lent, taken out, another card) and the computer
// unplugging stop the server too, so no connection outlives what it named.
enum class FileShareState : uint8_t { Off, Starting, Serving, Failed };
constexpr const char* file_share_state_names[] = {"off", "starting", "serving", "failed"};
constexpr const char* file_share_name = "SD Card";

// What an app asks for (App::file_sharing()): on or off, and the server's
// name (NetBIOS, cut to 15 characters and upper-cased by computers).
struct FileSharing {
    bool on = false;
    std::string name = "MPC-SAMPLE";
};

struct FileShareStatus {
    FileShareState state = FileShareState::Off;
    unsigned computers = 0; // Connections to "SD Card" open now
    std::string error;      // Why the server could not start (Failed)
};

class FileShare : public CardUsers {
public:
    FileShare();
    ~FileShare() override; // Ends the thread; leaves the server to the next run
    FileShare(const FileShare&) = delete;
    FileShare& operator=(const FileShare&) = delete;
    // Service thread, every pass: what the app asks for, the card's folder
    // ("" while there is none or it is not the MPC's) and whether a computer
    // has the USB gadget configured.
    void follow(const FileSharing& wanted, const std::string& folder, bool connected);
    FileShareStatus status() const;
    // CardUsers, service thread: a computer has the card open over the
    // network; and, before the card is unmounted to be lent, the server let
    // go of it (stopped, so no connection keeps the card's folder).
    bool in_use() const override { return users.load() > 0; }
    void let_go() override;

private:
    void run();

    mutable std::mutex lock;
    std::condition_variable changed;
    FileSharing wanted;
    std::string folder;
    bool connected = false;
    bool followed = false;   // Nothing is done before the first follow(): an earlier run's server waits for it
    FileShareStatus current;
    unsigned generation = 0; // Moves when let_go() stopped the server

    std::atomic<unsigned> users {0};
    std::atomic<bool> stopping {false};
    std::thread thread;
};
