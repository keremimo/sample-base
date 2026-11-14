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
    put(function + "/p_srate", "44100\n"); put(function + "/c_srate", "44100\n");
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
    assert(slurp(saved) == function + " 3 44100\n");
    assert(describe(change) == function + " p_chmask 3 -> 262143");

    // The next app start finds it widened and keeps the original saved.
    put(gadget + "/UDC", "fcc00000.usb\n");
    change = widen_uac2_playback(18, root, saved);
    assert(change.result == GadgetChange::Result::Unchanged && slurp(saved) == function + " 3 44100\n");

    // An unbound gadget stays unbound.
    put(gadget + "/UDC", "\n");
    change = widen_uac2_playback(2, root, saved);
    assert(change.result == GadgetChange::Result::Changed && slurp(function + "/p_chmask") == "3\n");
    assert(slurp(gadget + "/UDC") == "\n" && slurp(saved) == function + " 3 44100\n" && linked());

    // The rate follows the audio device's, both ways at once (the stock gadget shares one clock); the
    // original stays saved. The same rate again touches nothing; a rate f_uac2 has no room for is refused.
    put(gadget + "/UDC", "fcc00000.usb\n");
    change = set_uac2_rate(96000, root, saved);
    assert(change.result == GadgetChange::Result::Changed && change.rate && change.before == 44100 && change.after == 96000);
    assert(slurp(function + "/p_srate") == "96000\n" && slurp(function + "/c_srate") == "96000\n");
    assert(slurp(gadget + "/UDC") == "fcc00000.usb\n" && linked() && slurp(saved) == function + " 3 44100\n");
    assert(describe(change) == function + " p_srate 44100 -> 96000");
    assert(set_uac2_rate(96000, root, saved).result == GadgetChange::Result::Unchanged);
    assert(set_uac2_rate(400000, root, saved).result == GadgetChange::Result::Failed && slurp(function + "/p_srate") == "96000\n");
    unlink(saved.c_str());
    assert(set_uac2_rate(44100, root, saved).result == GadgetChange::Result::Changed); // the first change saves the original
    assert(slurp(saved) == function + " 3 96000\n" && slurp(function + "/c_srate") == "44100\n");
    unlink(saved.c_str()); put(saved, function + " 3 44100\n");

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

    // Screen Mirror: the webcam made for one size and linked (absolute links throughout; configfs makes the
    // default groups itself, here make_dirs() does), the same size again left alone, another size made anew.
    std::string webcam = gadget + "/functions/uvc.screen";
    auto points_at = [](const std::string& link, const std::string& to) {
        char buffer[512] {};
        return readlink(link.c_str(), buffer, sizeof buffer - 1) > 0 && std::string(buffer) == to;
    };
    assert(!gadget_mirror_linked(root));
    assert(gadget_add_mirror({1280, 720, true}, error, root) == MirrorLink::Linked);
    assert(gadget_mirror_linked(root) && linked() && slurp(gadget + "/UDC") == "fcc00000.usb\n");
    assert(points_at(config + "/uvc.screen", webcam));
    std::string frame = webcam + "/streaming/mjpeg/m/f";
    assert(slurp(frame + "/wWidth") == "1280\n" && slurp(frame + "/wHeight") == "720\n");
    assert(slurp(frame + "/dwMaxVideoFrameBufferSize") == "921600\n" && slurp(frame + "/dwFrameInterval") == "333333\n");
    assert(slurp(frame + "/dwMaxBitRate") == "221184000\n" && slurp(webcam + "/streaming_maxpacket") == "3072\n");
    MirrorFormat found;
    assert(gadget_mirror_format(found, root) && found == (MirrorFormat {1280, 720, true})); // what a restarted app takes over
    assert(points_at(webcam + "/streaming/header/h/m", webcam + "/streaming/mjpeg/m"));
    for (auto speed : {"fs", "hs", "ss"}) assert(points_at(webcam + "/streaming/class/" + speed + "/h", webcam + "/streaming/header/h"));
    for (auto speed : {"fs", "ss"}) assert(points_at(webcam + "/control/class/" + speed + "/h", webcam + "/control/header/h"));
    put(webcam + "/kept", "1\n");
    assert(gadget_add_mirror({1280, 720, true}, error, root) == MirrorLink::Linked && slurp(webcam + "/kept") == "1\n");
    assert(gadget_add_mirror({320, 240, false}, error, root) == MirrorLink::Linked);
    assert(stat((webcam + "/kept").c_str(), &info) != 0 && stat((webcam + "/streaming/mjpeg/m").c_str(), &info) != 0);
    assert(slurp(webcam + "/streaming/uncompressed/u/f/wWidth") == "320\n");
    assert(slurp(webcam + "/streaming/uncompressed/u/f/dwMaxVideoFrameBufferSize") == "153600\n");
    assert(slurp(webcam + "/streaming_maxpacket") == "1024\n" && gadget_mirror_linked(root) && linked());
    // 640 x 480 YUYV needs 18.4 MB/s: three packets a microframe; a JPEG (a playing project: 340 kB) three too.
    assert((MirrorFormat {640, 480, false}.packet_bytes() == 3072 && MirrorFormat {320, 240, false}.packet_bytes() == 1024));
    assert((MirrorFormat {2560, 1920, true}.packet_bytes() == 3072 && MirrorFormat {2560, 1920, true}.frame_bytes() == 4915200));
    assert(gadget_add_mirror({640, 480, false}, error, root) == MirrorLink::Linked && slurp(webcam + "/streaming_maxpacket") == "3072\n");
    assert(gadget_add_mirror({320, 240, false}, error, root) == MirrorLink::Linked && slurp(webcam + "/streaming_maxpacket") == "1024\n");

    // The drive puts the webcam out (the controller has no FIFO for a seventh IN endpoint) and brings it back;
    // asked for meanwhile, the webcam waits.
    for (auto dir : {storage, lun}) mkdir(dir.c_str(), 0700);
    for (auto name : {"file", "removable", "ro", "forced_eject"}) put(lun + "/" + name, "\n");
    order.clear();
    assert(gadget_add_storage("/dev/mmcblk1", order, error, root));
    assert(!gadget_mirror_linked(root) && stat(webcam.c_str(), &info) == 0 && linked());
    assert(std::find(order.begin(), order.end(), "uvc.screen") != order.end());
    assert(gadget_add_mirror({320, 240, false}, error, root) == MirrorLink::CardShared && !gadget_mirror_linked(root));
    assert(gadget_eject_storage(false, error, root) == GadgetEject::Done);
    for (auto name : {"file", "removable", "ro", "forced_eject"}) unlink((lun + "/" + name).c_str());
    rmdir(lun.c_str());
    assert(gadget_remove_storage(order, error, root));
    assert(gadget_mirror_linked(root) && points_at(config + "/uvc.screen", webcam) && linked());

    // Off: unlinked and gone, the rest as it was.
    assert(gadget_remove_mirror(error, root));
    assert(!gadget_mirror_linked(root) && stat(webcam.c_str(), &info) != 0 && lstat((config + "/uvc.screen").c_str(), &info) != 0);
    assert(linked() && slurp(gadget + "/UDC") == "fcc00000.usb\n");
    assert(gadget_remove_mirror(error, root)); // None: nothing to do
    assert(!gadget_mirror_format(found, root));
    // A computer showing the camera: no drive (the webcam would step out under it).
    std::string marker = std::string(argv[1]) + "/live";
    assert(!gadget_mirror_live(marker));
    put(marker, "");
    assert(gadget_mirror_live(marker));
}
