// SPDX-License-Identifier: AGPL-3.0-only
// A fake configfs gadget in a temporary folder: the UAC2 function found among
// others, its playback mask widened with the gadget unbound and bound again,
// the original saved once, and nothing touched when it already fits.
#include "drivers/usb_gadget.h"
#include <algorithm>
#include <cassert>
#include <fstream>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

static std::string slurp(const std::string& path) {
    std::ifstream in(path);
    std::stringstream text; text << in.rdbuf();
    return text.str();
}
static void put(const std::string& path, const std::string& text) { std::ofstream(path) << text; }

int main(int argc, char** argv) {
    assert(argc == 2);
    std::string root = std::string(argv[1]) + "/usb_gadget", saved = std::string(argv[1]) + "/saved";
    assert(widen_uac2_playback(18, root, saved).result == GadgetChange::Result::Missing);

    std::string gadget = root + "/ac50", function = gadget + "/functions/uac2.audio";
    for (auto dir : {root, gadget, gadget + "/functions", gadget + "/functions/midi.midi", gadget + "/functions/ncm.network", function})
        mkdir(dir.c_str(), 0700);
    put(function + "/p_chmask", "3\n");
    put(gadget + "/UDC", "fcc00000.usb\n");
    std::string config = gadget + "/configs/c.1";
    for (auto dir : {gadget + "/configs", config}) mkdir(dir.c_str(), 0700);
    put(config + "/MaxPower", "500\n");
    for (auto name : {"uac2.audio", "ncm.network", "midi.midi"})
        assert(symlink(("../../functions/" + std::string(name)).c_str(), (config + "/" + name).c_str()) == 0);
    auto linked = [&] {
        for (auto name : {"uac2.audio", "ncm.network", "midi.midi"}) {
            char target[256] {};
            if (readlink((config + "/" + name).c_str(), target, sizeof target - 1) <= 0 ||
                std::string(target) != gadget + "/functions/" + name) return false; // Absolute: configfs resolves from the cwd
        }
        return slurp(config + "/MaxPower") == "500\n";
    };

    assert(widen_uac2_playback(28, root, saved).result == GadgetChange::Result::Failed); // Past the kernel's 27.
    assert(slurp(function + "/p_chmask") == "3\n");

    auto change = widen_uac2_playback(18, root, saved);
    assert(change.result == GadgetChange::Result::Changed && change.function == function);
    assert(change.before == 3 && change.after == 0x3ffff);
    assert(slurp(function + "/p_chmask") == "262143\n");
    assert(slurp(gadget + "/UDC") == "fcc00000.usb\n" && linked()); // Linked and bound again.
    assert(slurp(saved) == function + " 3\n");
    assert(describe(change) == function + " p_chmask 3 -> 262143");

    // The next app start finds it widened and keeps the original saved.
    put(gadget + "/UDC", "fcc00000.usb\n");
    change = widen_uac2_playback(18, root, saved);
    assert(change.result == GadgetChange::Result::Unchanged && slurp(saved) == function + " 3\n");

    // An unbound gadget stays unbound.
    put(gadget + "/UDC", "\n");
    change = widen_uac2_playback(2, root, saved);
    assert(change.result == GadgetChange::Result::Changed && slurp(function + "/p_chmask") == "3\n");
    assert(slurp(gadget + "/UDC") == "\n" && slurp(saved) == function + " 3\n" && linked());

    // SD card access: the mass-storage function made (configfs makes its
    // LUN; here it is there already), linked last, bound, given back.
    put(gadget + "/UDC", "fcc00000.usb\n");
    std::string storage = gadget + "/functions/mass_storage.sd", lun = storage + "/lun.0";
    for (auto dir : {storage, lun}) mkdir(dir.c_str(), 0700);
    for (auto name : {"file", "removable", "ro", "forced_eject"}) put(lun + "/" + name, "\n");
    bool present = false;
    assert(gadget_storage_medium(present, root).empty() && present);
    std::string error;
    std::vector<std::string> order;
    assert(gadget_add_storage("/dev/mmcblk1", order, error, root));
    assert(slurp(lun + "/file") == "/dev/mmcblk1\n" && slurp(lun + "/removable") == "1\n" && slurp(lun + "/ro") == "0\n");
    char target[256] {};
    assert(readlink((config + "/mass_storage.sd").c_str(), target, sizeof target - 1) > 0 && std::string(target) == storage);
    assert(linked() && slurp(gadget + "/UDC") == "fcc00000.usb\n");
    // The links as they were, named for gadget_remove_storage(). The order is configfs's readdir order (newest
    // first), which a plain folder does not keep: the order itself is checked on the MPC.
    std::sort(order.begin(), order.end());
    assert((order == std::vector<std::string> {"midi.midi", "ncm.network", "uac2.audio"}));
    assert(gadget_storage_medium(present, root) == "/dev/mmcblk1" && present);
    // A second request changes only the medium.
    std::vector<std::string> again;
    assert(gadget_add_storage("/dev/mmcblk2", again, error, root) && slurp(lun + "/file") == "/dev/mmcblk2\n" && again.empty());
    assert(gadget_eject_storage(false, error, root) == GadgetEject::Done && gadget_storage_medium(present, root).empty());
    assert(gadget_eject_storage(false, error, root) == GadgetEject::Done); // Ejected already: nothing to do
    for (auto name : {"file", "removable", "ro", "forced_eject"}) unlink((lun + "/" + name).c_str());
    rmdir(lun.c_str());
    assert(gadget_remove_storage(order, error, root));
    struct stat info;
    assert(lstat((config + "/mass_storage.sd").c_str(), &info) != 0 && stat(storage.c_str(), &info) != 0);
    assert(linked() && slurp(gadget + "/UDC") == "fcc00000.usb\n");
    assert(gadget_remove_storage(order, error, root)); // None: nothing to do
    gadget_storage_medium(present, root);
    assert(!present);
}
