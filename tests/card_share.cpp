// SPDX-License-Identifier: AGPL-3.0-only
// SD card access without a card reader or a storage service: the card found
// among edisksd's volumes, nothing lent without one, and a card an earlier
// run left lent taken back at start.
#include "drivers/card_share.h"
#include "drivers/system_services.h"
#include <cassert>
#include <chrono>
#include <fstream>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <vector>
#include <unistd.h>

static void put(const std::string& path, const std::string& text) { std::ofstream(path) << text; }
static bool exists(const std::string& path) { struct stat info; return lstat(path.c_str(), &info) == 0; }

int main(int argc, char** argv) {
    assert(argc == 2);
    std::string temp = argv[1];
    // edisksd's volumes as read on the MPC: the internal eMMC's (HintIgnore)
    // and the card's.
    auto volume = [](std::string name, std::string disk, std::string media, bool removable, bool ignore, std::vector<std::string> mounts) {
        StorageVolume v;
        v.object = "/io/github/inmusicdev/edisksd/dev/" + name; v.device = "/dev/" + name;
        v.sys_path = "/sys/devices/platform/fe2b0000.mmc/mmc_host/mmc1/mmc1:d555/block/" + disk + (disk == name ? "" : "/" + name);
        v.media = media; v.removable = removable; v.ignore = ignore; v.mountpoints = mounts;
        return v;
    };
    std::vector<StorageVolume> volumes {
        volume("mmcblk0p11", "mmcblk0", "flash_mmc", false, true, {"/content"}),
        volume("mmcblk1p1", "mmcblk1", "flash_sd", true, false, {"/media/3265-3932"}),
        volume("mmcblk1p2", "mmcblk1", "flash_sd", true, false, {}),
    };
    auto card = find_card(volumes);
    assert(card.disk == "mmcblk1" && card.volumes.size() == 2 && card.mounted.size() == 1);
    assert(card.mounted[0].object == "/io/github/inmusicdev/edisksd/dev/mmcblk1p1");
    assert(find_card({volumes[0]}).disk.empty()); // The MPC's own storage is never the card
    assert(find_card({volume("mmcblk1", "mmcblk1", "flash_sd", true, false, {})}).disk == "mmcblk1"); // No partition table
    assert(find_card(volumes, "mmcblk1").volumes.size() == 2 && find_card(volumes, "sda").volumes.empty());

    std::string mountinfo = temp + "/mountinfo", gadget = temp + "/usb_gadget", saved = temp + "/card-share.saved";
    for (auto dir : {gadget, gadget + "/ac50", gadget + "/ac50/functions", gadget + "/ac50/configs", gadget + "/ac50/configs/c.1"})
        mkdir(dir.c_str(), 0700);
    put(mountinfo, "327 49 179:97 / /media/3265-3932 rw,nosuid - exfat /dev/mmcblk1p1 rw\n");
    put(gadget + "/ac50/UDC", "fcc00000.usb\n");

    SystemServices services; // No system bus here: no volumes, every mount request fails.
    {
        CardShare share(services, temp, mountinfo, gadget, saved);
        assert(share.status().state == CardShareState::Home);
        assert(!share.lend() && share.status().message == "no card" && share.status().state == CardShareState::Home);
        share.poll();
        assert(!exists(gadget + "/ac50/functions/mass_storage.sd") && !exists(saved));
    }
    put(mountinfo, "");

    // An earlier run left it lent: taken back (function unlinked and gone,
    // the note removed), then mounted again or given up after five seconds.
    mkdir((gadget + "/ac50/functions/mass_storage.sd").c_str(), 0700);
    assert(symlink((gadget + "/ac50/functions/mass_storage.sd").c_str(), (gadget + "/ac50/configs/c.1/mass_storage.sd").c_str()) == 0);
    put(saved, "mmcblk1\n");
    CardShare share(services, temp, mountinfo, gadget, saved);
    assert(share.status().state == CardShareState::Lent && share.status().disk == "mmcblk1");
    share.poll();
    assert(share.status().state == CardShareState::Returning);
    assert(!exists(gadget + "/ac50/configs/c.1/mass_storage.sd") && !exists(gadget + "/ac50/functions/mass_storage.sd") && !exists(saved));
    auto until = std::chrono::steady_clock::now() + std::chrono::seconds(7);
    while (share.status().state == CardShareState::Returning && std::chrono::steady_clock::now() < until) {
        share.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    assert(share.status().state == CardShareState::Home && share.status().returns == 1);
    assert(share.status().message == "the card came back with nothing to mount");
    assert(share.json().find("\"state\":\"home\"") != std::string::npos);
}
