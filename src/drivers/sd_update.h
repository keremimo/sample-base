// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <cstdint>
#include <string>
#include <vector>

// App updates from a release zip in the SD card's top folder (docs/APPS.md,
// "Updates from the card"). install/sd-update.sh does the work: `check`
// reports whether a zip is a signed release and its version, `install`
// installs it and restarts the app. These functions find the zip to offer,
// start the install as a separate systemd unit (it stops the app), and read
// its status. Call them from the service thread only; they run child
// processes.
namespace sd_update {

constexpr const char* status_path = "/run/sample-base-update.status";

// Compares release versions: v0.1.0-alpha.2 < v0.1.0-alpha.10 < v0.1.0 <
// v0.1.0-3-gabc1234 (git describe, with or without "-dirty") < v0.1.1. An
// unparsable version ("dev") sorts before every release. The leading "v" is
// optional. Returns negative, zero or positive, like strcmp().
int compare_versions(const std::string& a, const std::string& b);

struct Setup {
    std::string script;  // path of install/sd-update.sh, next to the running binary
    std::string package; // the app's folder name under autorun/ ("melodee")
    std::string app;     // the --app name
    std::string key;     // PEM file with the Ed25519 public key releases are signed with
};

struct Offer {
    std::string zip, version; // empty when there is no update
    uint64_t size = 0;
    bool empty() const { return zip.empty(); }
};

// Lists *.zip files (any case) in the card's top folder, sorted, smaller
// than `limit` bytes. Skips hidden files such as macOS "._" files.
std::vector<std::string> zips_at(const std::string& card, uint64_t limit = uint64_t(1) << 30);
// Returns the newest zip that the script's check accepts and whose version
// is newer than `current`. Zips listed in `skip` are ignored.
Offer find(const Setup& setup, const std::string& card, const std::string& current,
           const std::vector<std::string>& skip = {});
// Starts the install as the systemd unit sample-base-update, so that it
// keeps running after it stops the app. Returns false and sets `error` if
// systemd-run fails.
bool start(const Setup& setup, const Offer& offer, const std::string& card, std::string& error);

// Reads the status file the install writes. state: checking, writing,
// starting, done or failed (empty if no install ran since boot). version:
// the version being installed. error: signature, damaged, self-test,
// no-card, other-app, card-full, card-write, no-start or error.
struct Status {
    std::string state, version, error;
    bool running() const { return state == "checking" || state == "writing" || state == "starting"; }
};
Status read_status(const std::string& path = status_path);

} // namespace sd_update
