// SPDX-License-Identifier: AGPL-3.0-only
// A fake configfs gadget in a temporary folder: the UAC2 function found among
// others, its playback mask widened with the gadget unbound and bound again,
// the original saved once, and nothing touched when it already fits.
#include "drivers/usb_gadget.h"
#include <cassert>
#include <fstream>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

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
}
