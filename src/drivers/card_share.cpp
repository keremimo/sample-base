// SPDX-License-Identifier: AGPL-3.0-only
#include "drivers/card_share.h"
#include "drivers/platform.h"
#include "drivers/system_services.h"
#include <dirent.h>
#include <fcntl.h>
#include <linux/fs.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

namespace {
std::string base_name(const std::string& path) { return path.substr(path.rfind('/') + 1); }
std::string read_all(const std::string& path) {
    std::ifstream file(path);
    std::stringstream text; text << file.rdbuf();
    return text.str();
}
} // namespace

CardVolumes find_card(const std::vector<StorageVolume>& volumes, const std::string& disk) {
    auto disk_of = [](const StorageVolume& v) {
        auto parent = v.sys_path.substr(0, v.sys_path.rfind('/'));
        return base_name(parent) == "block" ? base_name(v.sys_path) : base_name(parent);
    };
    CardVolumes card;
    card.disk = disk;
    if (card.disk.empty())
        for (const auto& v : volumes)
            if (v.removable && !v.ignore && v.media.rfind("flash_sd", 0) == 0) { card.disk = disk_of(v); break; }
    if (card.disk.empty()) return card;
    for (const auto& v : volumes) {
        if (disk_of(v) != card.disk) continue;
        card.volumes.push_back(v);
        if (!v.mountpoints.empty()) card.mounted.push_back(v);
    }
    return card;
}

CardShare::CardShare(SystemServices& services, std::string sys, std::string mountinfo, std::string gadget, std::string saved)
    : services(services), sys(std::move(sys)), mountinfo(std::move(mountinfo)), gadget(std::move(gadget)), saved(std::move(saved)) {
    // Left lent by an earlier run (a crash, or an app swap): take it back first.
    bool present = false;
    std::string medium = gadget_storage_medium(present, this->gadget);
    std::ifstream file(this->saved);
    std::string disk, name;
    file >> disk;
    while (file >> name) order.push_back(name);
    if (disk.empty() && medium.rfind("/dev/", 0) == 0) disk = medium.substr(5);
    if (present || !disk.empty()) {
        current.state = CardShareState::Lent;
        current.disk = disk;
        asked = Ask::Force;
    }
}

std::string CardShare::read(const std::string& path) const {
    std::ifstream file(path);
    std::string line;
    std::getline(file, line);
    while (!line.empty() && (line.back() == ' ' || line.back() == '\n')) line.pop_back();
    return line;
}

bool CardShare::lend() {
    if (current.state != CardShareState::Home) { current.message = "the card is not the MPC's now"; return false; }
    auto card = find_card(services.volumes());
    if (card.disk.empty()) { current.message = "no card"; return false; }
    current.disk = card.disk;
    current.message.clear();
    current.state = CardShareState::Lending;
    asked = Ask::Lend;
    return true;
}

void CardShare::take_back(bool force) {
    if (current.state != CardShareState::Lent) return;
    asked = force ? Ask::Force : Ask::Return;
}

void CardShare::poll() {
    auto now = Clock::now();
    if (asked == Ask::Lend) { asked = Ask::None; do_lend(); }
    else if (asked == Ask::Return || asked == Ask::Force) { bool force = asked == Ask::Force; asked = Ask::None; do_return(force); }
    if (current.state == CardShareState::Returning && now >= next_try) { next_try = now + std::chrono::milliseconds(250); remount(); }
    // The computer's side, four times a second.
    if (now < next_follow) return;
    next_follow = now + std::chrono::milliseconds(250);
    std::string udc = read(gadget + "/" + [&] {
        // The first gadget's directory name.
        std::string name;
        if (DIR* dir = opendir(gadget.c_str())) {
            while (dirent* entry = readdir(dir)) if (entry->d_name[0] != '.') { name = entry->d_name; break; }
            closedir(dir);
        }
        return name;
    }() + "/UDC");
    current.connected = !udc.empty() && read(sys + "/class/udc/" + udc + "/state") == "configured";
    bool present = false;
    current.ejected = current.state == CardShareState::Lent && gadget_storage_medium(present, gadget).empty();
    if (current.ejected && current.locked) { current.locked = false; current.message.clear(); }
}

void CardShare::do_lend() {
    auto fail = [&](const std::string& why) {
        current.message = why;
        current.state = CardShareState::Returning; // Mounts what was unmounted again
        remount_until = Clock::now() + std::chrono::seconds(5);
        next_try = {};
    };
    auto card = find_card(services.volumes(), current.disk);
    if (card.volumes.empty()) { current.state = CardShareState::Home; current.message = "no card"; return; }
    ::sync();
    for (const auto& volume : card.mounted)
        if (!services.mount(volume.object, false)) { fail("the card is in use"); return; }
    // Never offer a card that is still mounted here: edisksd's word, then the kernel's.
    if (!find_card(services.volumes(), current.disk).mounted.empty() || mounted_here()) { fail("the card is still mounted"); return; }
    order.clear();
    save(); // Before the gadget changes: a crash from here on still takes the card back
    std::string error;
    if (!gadget_add_storage("/dev/" + current.disk, order, error, gadget)) { std::remove(saved.c_str()); fail(error); return; }
    save();
    current.state = CardShareState::Lent;
    current.locked = false;
}

// "mmcblk1" and, once offered, the gadget's links before: "mmcblk1 uac2.audio ncm.network midi.midi".
void CardShare::save() const {
    std::ofstream file(saved, std::ios::trunc);
    file << current.disk;
    for (const auto& name : order) file << ' ' << name;
    file << '\n';
}

void CardShare::do_return(bool force) {
    std::string error;
    switch (gadget_eject_storage(force, error, gadget)) {
    case GadgetEject::Locked: current.locked = true; current.message = "eject the drive on the computer first"; return;
    case GadgetEject::Failed: current.message = error; return;
    case GadgetEject::Done: break;
    }
    if (!gadget_remove_storage(order, error, gadget)) current.message = error; // The medium is out already: carry on
    else current.message.clear();
    current.locked = false;
    if (!current.disk.empty()) {
        // What the computer wrote is in the block device's cache: write it
        // out, then read the partitions again (the computer may have
        // formatted the card).
        int fd = open(("/dev/" + current.disk).c_str(), O_RDONLY | O_CLOEXEC);
        if (fd >= 0) { fsync(fd); ioctl(fd, BLKRRPART); close(fd); }
    }
    std::remove(saved.c_str());
    order.clear();
    current.state = CardShareState::Returning;
    remount_until = Clock::now() + std::chrono::seconds(5);
    next_try = Clock::now() + std::chrono::milliseconds(250); // udev and edisksd see the partitions again first
}

// edisksd mounts each partition of the card (it picks the mount point and
// skips what holds no file system it knows); once one is mounted, or after
// five seconds, the card is home.
void CardShare::remount() {
    bool home = mounted_here();
    if (!current.disk.empty() && !home) {
        auto card = find_card(services.volumes(), current.disk);
        for (const auto& volume : card.volumes)
            if (volume.mountpoints.empty()) services.mount(volume.object, true);
        home = mounted_here();
    }
    if (current.disk.empty() || home || Clock::now() >= remount_until) {
        if (!current.disk.empty() && !home && current.message.empty()) current.message = "the card came back with nothing to mount";
        current.state = CardShareState::Home;
        current.ejected = false;
        ++current.returns;
    }
}

// The kernel's mount table: any file system on /dev/<disk> or its partitions.
bool CardShare::mounted_here() const {
    if (current.disk.empty()) return false;
    std::string disk = "/dev/" + current.disk;
    for (const auto& mount : parse_mountinfo(read_all(mountinfo)))
    {
        if (mount.device == disk) return true;
        if (mount.device.rfind(disk, 0) != 0 || mount.device.size() == disk.size()) continue;
        // Partitions: mmcblk1 -> mmcblk1p1, sda -> sda1.
        char next = mount.device[disk.size()];
        if (std::isdigit(static_cast<unsigned char>(disk.back())) ? next == 'p' : std::isdigit(static_cast<unsigned char>(next))) return true;
    }
    return false;
}

void CardShare::shutdown() {
    if (current.state == CardShareState::Lending) asked = Ask::None;
    if (current.state == CardShareState::Lent || current.state == CardShareState::Lending) do_return(true);
    auto until = Clock::now() + std::chrono::seconds(3);
    while (current.state == CardShareState::Returning && Clock::now() < until) {
        services.poll();
        remount();
        if (current.state == CardShareState::Returning) usleep(100000);
    }
}

std::string CardShare::json() const {
    std::ostringstream out;
    out << "{\"state\":" << json_string(card_share_state_names[size_t(current.state)]) << ",\"disk\":" << json_string(current.disk)
        << ",\"connected\":" << (current.connected ? "true" : "false") << ",\"ejected\":" << (current.ejected ? "true" : "false")
        << ",\"locked\":" << (current.locked ? "true" : "false") << ",\"returns\":" << current.returns
        << ",\"message\":" << json_string(current.message) << '}';
    return out.str();
}
