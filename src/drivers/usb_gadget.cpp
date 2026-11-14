// SPDX-License-Identifier: AGPL-3.0-only
#include "drivers/usb_gadget.h"
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <mutex>
#include <sstream>
#include <sys/stat.h>
#include <vector>

namespace {
// The service thread (rate, card share) and Screen Mirror's thread both
// relink the gadget: one change at a time. Recursive: a failed drive offer
// takes the drive out again with gadget_remove_storage().
std::recursive_mutex relinking;

// A directory's entries in readdir order, which configfs gives newest first.
std::vector<std::string> unsorted(const std::string& directory) {
    std::vector<std::string> names;
    if (DIR* dir = opendir(directory.c_str())) {
        while (dirent* entry = readdir(dir))
            if (entry->d_name[0] != '.') names.emplace_back(entry->d_name);
        closedir(dir);
    }
    return names;
}

std::vector<std::string> entries(const std::string& directory) {
    auto names = unsorted(directory);
    std::sort(names.begin(), names.end());
    return names;
}

// One function linked into a configuration: the link and what it points at.
struct Link { std::string path, target; };

// Every function link in the gadget's configurations, in the order they were
// made (the order of the interfaces the computer sees). configfs reads a link
// back as a path relative to the link, but resolves a new link's target from
// the caller's working directory (kern_path() in fs/configfs/symlink.c), so
// each target is kept as the function's absolute directory.
std::vector<Link> function_links(const std::string& gadget) {
    std::vector<Link> links;
    for (const auto& config : entries(gadget + "/configs")) {
        std::string dir = gadget + "/configs/" + config;
        auto names = unsorted(dir);
        for (auto name = names.rbegin(); name != names.rend(); ++name) {
            std::string path = dir + "/" + *name;
            char target[512];
            ssize_t length = readlink(path.c_str(), target, sizeof target - 1);
            if (length <= 0) continue;
            std::string function(target, size_t(length));
            links.push_back({path, gadget + "/functions/" + function.substr(function.rfind('/') + 1)});
        }
    }
    return links;
}

bool read_line(const std::string& path, std::string& text) {
    int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    char buffer[128];
    ssize_t count = read(fd, buffer, sizeof buffer - 1);
    close(fd);
    if (count < 0) return false;
    text.assign(buffer, size_t(count));
    while (!text.empty() && (text.back() == '\n' || text.back() == ' ')) text.pop_back();
    return true;
}

// configfs reports a refused value from write() itself, so every byte is checked.
bool write_text(const std::string& path, const std::string& text, std::string& error, int flags = O_WRONLY | O_TRUNC) {
    int fd = open(path.c_str(), flags | O_CLOEXEC, 0600);
    bool ok = fd >= 0 && write(fd, text.data(), text.size()) == ssize_t(text.size());
    int saved_errno = errno;
    if (fd >= 0 && close(fd) < 0 && ok) { ok = false; saved_errno = errno; }
    if (!ok) { error = path + ": " + std::strerror(saved_errno); errno = saved_errno; }
    return ok;
}

bool parse_mask(const std::string& text, unsigned& mask) {
    if (text.empty()) return false;
    char* end = nullptr;
    errno = 0;
    unsigned long value = std::strtoul(text.c_str(), &end, 0);
    if (errno || *end || value > 0xffffffffUL) return false;
    mask = unsigned(value);
    return true;
}
// Unbinds the gadget, takes the `current` function links out, runs
// `change`, makes the `wanted` links (in that order: the order the computer
// sees the interfaces in, and the order the controller hands out endpoints)
// and binds the gadget again when it was bound. Returns false with `error`
// set when any step failed; on failure the old links and the binding are
// put back.
bool rebind(const std::string& gadget, const std::string& udc, const std::vector<Link>& current,
            const std::function<bool(std::string&)>& change, std::string& error, const std::vector<Link>* wanted = nullptr) {
    bool ok = udc.empty() || write_text(gadget + "/UDC", "\n", error);
    std::vector<Link> removed;
    for (const auto& link : current) {
        if (!ok) break;
        if (unlink(link.path.c_str()) == 0 || errno == ENOENT) removed.push_back(link); // gone: a failed try left it out
        else { ok = false; error = link.path + ": " + std::strerror(errno); }
    }
    ok = ok && change(error);
    for (const auto& link : ok && wanted ? *wanted : removed)
        if (symlink(link.target.c_str(), link.path.c_str()) != 0 && ok) {
            ok = false; error = link.path + ": " + std::strerror(errno);
        }
    std::string rebind_error;
    if (!udc.empty() && !write_text(gadget + "/UDC", udc + "\n", rebind_error)) {
        if (ok) error = rebind_error;
        ok = false;
    }
    return ok;
}

std::string link_name(const Link& link) { return link.path.substr(link.path.rfind('/') + 1); }

// The first gadget with a configuration, and that configuration.
bool first_gadget(const std::string& root, std::string& gadget, std::string& config) {
    for (const auto& name : entries(root)) {
        auto configs = entries(root + "/" + name + "/configs");
        if (configs.empty()) continue;
        gadget = root + "/" + name; config = gadget + "/configs/" + configs.front();
        return true;
    }
    return false;
}

bool exists(const std::string& path) { struct stat info; return stat(path.c_str(), &info) == 0; }

// The first gadget with a UAC2 function, and that function's directory.
bool find_uac2(const std::string& root, std::string& gadget, std::string& function) {
    for (const auto& name : entries(root))
        for (const auto& entry : entries(root + "/" + name + "/functions"))
            if (entry.rfind("uac2.", 0) == 0) { gadget = root + "/" + name; function = gadget + "/functions/" + entry; return true; }
    return false;
}

// The first change saves the original mask and rate; a later one (another
// app start, another rate) must not overwrite them with its own.
bool save_original(const std::string& function, const std::string& saved, std::string& error) {
    if (exists(saved)) return true;
    std::string mask, rate;
    if (!read_line(function + "/p_chmask", mask) || !read_line(function + "/p_srate", rate)) {
        error = function + " unreadable"; return false;
    }
    return write_text(saved, function + " " + mask + " " + rate + "\n", error, O_WRONLY | O_CREAT | O_EXCL);
}
} // namespace

GadgetChange widen_uac2_playback(unsigned channels, const std::string& root, const std::string& saved) {
    std::lock_guard<std::recursive_mutex> lock(relinking);
    GadgetChange change;
    if (channels < 1 || channels > 27) { change.result = GadgetChange::Result::Failed; change.error = "1 to 27 channels"; return change; }
    std::string gadget;
    if (!find_uac2(root, gadget, change.function)) return change;

    std::string text, udc;
    if (!read_line(change.function + "/p_chmask", text) || !parse_mask(text, change.before)) {
        change.result = GadgetChange::Result::Failed; change.error = change.function + "/p_chmask unreadable"; return change;
    }
    change.after = (1u << channels) - 1;
    if (change.before == change.after) { change.result = GadgetChange::Result::Unchanged; return change; }
    if (!read_line(gadget + "/UDC", udc)) {
        change.result = GadgetChange::Result::Failed; change.error = gadget + "/UDC unreadable"; return change;
    }
    if (!save_original(change.function, saved, change.error)) { change.result = GadgetChange::Result::Failed; return change; }
    // f_uac2 refuses a new mask while any configuration links the function,
    // bound or not, so every link comes out and goes back in its old order.
    // USB is also how SSH reaches the MPC: when the wide gadget will not
    // bind, the original mask goes back the same way.
    auto links = function_links(gadget);
    auto mask = [&](unsigned value) {
        return [&change, value](std::string& error) { return write_text(change.function + "/p_chmask", std::to_string(value) + "\n", error); };
    };
    bool ok = rebind(gadget, udc, links, mask(change.after), change.error);
    if (!ok) {
        std::string back_error;
        change.error += rebind(gadget, udc, links, mask(change.before), back_error) ? " (original mask restored)" : "; restoring: " + back_error;
    }
    change.result = ok ? GadgetChange::Result::Changed : GadgetChange::Result::Failed;
    return change;
}

GadgetChange set_uac2_rate(unsigned rate, const std::string& root, const std::string& saved) {
    std::lock_guard<std::recursive_mutex> lock(relinking);
    GadgetChange change;
    change.rate = true; change.after = rate;
    if (rate < 8000 || rate > 192000) { change.result = GadgetChange::Result::Failed; change.error = "8 to 192 kHz"; return change; }
    std::string gadget;
    if (!find_uac2(root, gadget, change.function)) return change;
    std::string playback, capture, udc;
    if (!read_line(change.function + "/p_srate", playback) || !read_line(change.function + "/c_srate", capture)) {
        change.result = GadgetChange::Result::Failed; change.error = change.function + " rate unreadable"; return change;
    }
    change.before = unsigned(std::strtoul(playback.c_str(), nullptr, 10)); // A list reads as its first rate
    std::string wanted = std::to_string(rate);
    if (playback == wanted && capture == wanted) { change.result = GadgetChange::Result::Unchanged; return change; }
    if (!read_line(gadget + "/UDC", udc)) {
        change.result = GadgetChange::Result::Failed; change.error = gadget + "/UDC unreadable"; return change;
    }
    if (!save_original(change.function, saved, change.error)) { change.result = GadgetChange::Result::Failed; return change; }
    // As the mask: f_uac2 takes a rate only while no configuration links it.
    auto links = function_links(gadget);
    auto rates = [&change](std::string p, std::string c) {
        return [&change, p, c](std::string& error) {
            return write_text(change.function + "/p_srate", p + "\n", error) && write_text(change.function + "/c_srate", c + "\n", error);
        };
    };
    bool ok = rebind(gadget, udc, links, rates(wanted, wanted), change.error);
    if (!ok) {
        std::string back_error;
        change.error += rebind(gadget, udc, links, rates(playback, capture), back_error) ? " (old rate restored)" : "; restoring: " + back_error;
    }
    change.result = ok ? GadgetChange::Result::Changed : GadgetChange::Result::Failed;
    return change;
}

bool gadget_add_storage(const std::string& medium, std::vector<std::string>& order, std::string& error, const std::string& root) {
    std::lock_guard<std::recursive_mutex> lock(relinking);
    std::string gadget, config, udc;
    if (!first_gadget(root, gadget, config)) { error = "no USB gadget"; return false; }
    if (!read_line(gadget + "/UDC", udc)) { error = gadget + "/UDC unreadable"; return false; }
    std::string function = gadget + "/functions/" + card_function, lun = function + "/lun.0";
    // The webcam would have to step out under the computer showing it (CardShare::lend() asks first).
    if (!exists(function) && gadget_mirror_live()) { error = mirror_in_use; return false; }
    // mkdir makes the function and its first LUN (removable, read-write, empty).
    if (mkdir(function.c_str(), 0755) != 0 && errno != EEXIST) { error = function + ": " + std::strerror(errno); return false; }
    auto links = function_links(gadget);
    bool linked = std::any_of(links.begin(), links.end(), [&](const Link& link) { return link.target == function; });
    if (!write_text(lun + "/removable", "1\n", error) || !write_text(lun + "/ro", "0\n", error) ||
        !write_text(lun + "/file", medium + "\n", error)) {
        std::string ignored; write_text(lun + "/file", "\n", ignored);
        if (!linked) rmdir(function.c_str());
        return false;
    }
    if (linked) return true; // Already offered (a second request): the medium was all that changed.
    order.clear();
    for (const auto& link : links) order.push_back(link_name(link));
    // The controller (DWC3 on the RK3566) hands IN endpoints out in link
    // order, and its sixth transmit FIFO holds only 336 bytes, less than a
    // high-speed bulk packet: the drive's bulk IN there never sends a
    // block. NCM's notify endpoint (16-byte packets) fits it, so the
    // network function goes after the drive; the audio and MIDI interfaces
    // keep their numbers. Screen Mirror's webcam steps out (its endpoint
    // would be the seventh, which has no FIFO at all) and comes back with
    // gadget_remove_storage(), named in `order`.
    std::vector<Link> wanted, network;
    for (const auto& link : links) {
        if (link_name(link).rfind("uvc.", 0) == 0) continue;
        (link_name(link).rfind("ncm.", 0) == 0 ? network : wanted).push_back(link);
    }
    wanted.push_back({config + "/" + card_function, function});
    wanted.insert(wanted.end(), network.begin(), network.end());
    if (rebind(gadget, udc, links, [](std::string&) { return true; }, error, &wanted)) return true;
    std::string ignored;
    write_text(lun + "/file", "\n", ignored);
    gadget_remove_storage(order, ignored, root);
    return false;
}

std::string gadget_storage_medium(bool& present, const std::string& root) {
    std::string gadget, config, file;
    present = first_gadget(root, gadget, config) && exists(gadget + "/functions/" + card_function);
    if (present) read_line(gadget + "/functions/" + card_function + "/lun.0/file", file);
    return file;
}

GadgetEject gadget_eject_storage(bool force, std::string& error, const std::string& root) {
    bool present = false;
    if (gadget_storage_medium(present, root).empty()) return GadgetEject::Done;
    std::string gadget, config;
    first_gadget(root, gadget, config);
    std::string lun = gadget + "/functions/" + card_function + "/lun.0";
    // f_mass_storage refuses (EBUSY) while the computer has locked the
    // medium in (PREVENT MEDIUM REMOVAL, held while it has it mounted);
    // forced_eject takes it anyway.
    if (write_text(lun + "/file", "\n", error)) return GadgetEject::Done;
    if (errno != EBUSY) return GadgetEject::Failed;
    if (!force) { error = "the computer has the medium locked"; return GadgetEject::Locked; }
    error.clear();
    return write_text(lun + "/forced_eject", "1\n", error) ? GadgetEject::Done : GadgetEject::Failed;
}

bool gadget_remove_storage(const std::vector<std::string>& order, std::string& error, const std::string& root) {
    std::lock_guard<std::recursive_mutex> lock(relinking);
    std::string gadget, config, udc;
    if (!first_gadget(root, gadget, config)) return true;
    std::string function = gadget + "/functions/" + card_function;
    if (!exists(function)) return true;
    if (!read_line(gadget + "/UDC", udc)) { error = gadget + "/UDC unreadable"; return false; }
    auto links = function_links(gadget);
    bool linked = std::any_of(links.begin(), links.end(), [&](const Link& link) { return link.target == function; });
    if (linked) {
        // Back in the order before the drive came (what is not named keeps its place after). A function the drive
        // put out (Screen Mirror's webcam) comes back when it still exists.
        std::vector<Link> wanted;
        for (const auto& name : order) {
            bool found = false;
            for (const auto& link : links)
                if (link_name(link) == name && link.target != function) { wanted.push_back(link); found = true; }
            std::string target = gadget + "/functions/" + name;
            if (!found && name != card_function && exists(target)) wanted.push_back({config + "/" + name, target});
        }
        for (const auto& link : links)
            if (link.target != function && std::none_of(wanted.begin(), wanted.end(), [&](const Link& w) { return w.path == link.path; }))
                wanted.push_back(link);
        if (!rebind(gadget, udc, links, [](std::string&) { return true; }, error, &wanted)) return false;
    }
    if (rmdir(function.c_str()) != 0 && errno != ENOENT) { error = function + ": " + std::strerror(errno); return false; }
    return true;
}

namespace {
// configfs makes a function's default groups itself (control/header,
// streaming/uncompressed...): mkdir finds them there (EEXIST) and makes the
// rest, also in a test's plain folder.
bool make_dirs(const std::string& path, std::string& error) {
    for (size_t at = 1; at != std::string::npos;) {
        at = path.find('/', at + 1);
        std::string part = path.substr(0, at);
        if (mkdir(part.c_str(), 0755) != 0 && errno != EEXIST) { error = part + ": " + std::strerror(errno); return false; }
    }
    return true;
}

// An attribute (O_CREAT: configfs has the file already; a test's folder gets it).
bool put(const std::string& path, const std::string& text, std::string& error) {
    return write_text(path, text, error, O_WRONLY | O_TRUNC | O_CREAT);
}

// A function's folder, its links first: alphabetical order takes each
// folder's class/ links out before the header/ they point at, and header/'s
// own link before the format. configfs refuses to remove its default groups
// and attribute files (they go with the function's own rmdir), a test's
// plain folder lets them go.
void remove_tree(const std::string& dir) {
    for (const auto& name : entries(dir)) {
        std::string path = dir + "/" + name;
        struct stat info;
        if (lstat(path.c_str(), &info) != 0) continue;
        if (S_ISLNK(info.st_mode) || S_ISREG(info.st_mode)) unlink(path.c_str());
        else if (S_ISDIR(info.st_mode)) { remove_tree(path); rmdir(path.c_str()); }
    }
}

bool storage_linked(const std::vector<Link>& links) {
    return std::any_of(links.begin(), links.end(), [](const Link& link) { return link_name(link) == card_function; });
}

// The webcam function for one picture size, unlinked (kernel/uvc's
// configfs: Documentation/usb/gadget-uvc.rst). No interrupt endpoint; the
// streaming endpoint's packets are 1024 bytes, up to three a microframe
// (MirrorFormat::packet_bytes()): 320 x 240 YUYV at 30 fps is 4.6 MB/s,
// 640 x 480 YUYV 18.4 MB/s, a 2560 x 1920 JPEG of the screen about 100 kB.
// Three packets still fit the controller's 3104-byte FIFO (3 x (1024 + 8) + 8).
bool make_mirror(const std::string& function, const MirrorFormat& format, std::string& error) {
    unsigned frame_bytes = format.frame_bytes();
    std::string kind = format.mjpeg ? "mjpeg" : "uncompressed", short_name = format.mjpeg ? "m" : "u";
    std::string control = function + "/control", streaming = function + "/streaming";
    std::string picture = streaming + "/" + kind + "/" + short_name, frame = picture + "/f";
    std::string bits = std::to_string(uint64_t(frame_bytes) * 8 * 30);
    bool ok = make_dirs(function, error) && put(function + "/streaming_maxpacket", std::to_string(format.packet_bytes()) + "\n", error) &&
              put(function + "/streaming_interval", "1\n", error) && put(function + "/function_name", "MPC Sample Screen\n", error) &&
              make_dirs(control + "/header/h", error) && make_dirs(control + "/class/fs", error) && make_dirs(control + "/class/ss", error) &&
              make_dirs(frame, error) && put(frame + "/wWidth", std::to_string(format.width) + "\n", error) &&
              put(frame + "/wHeight", std::to_string(format.height) + "\n", error) &&
              put(frame + "/dwMinBitRate", bits + "\n", error) && put(frame + "/dwMaxBitRate", bits + "\n", error) &&
              put(frame + "/dwMaxVideoFrameBufferSize", std::to_string(frame_bytes) + "\n", error) &&
              put(frame + "/dwDefaultFrameInterval", "333333\n", error) && put(frame + "/dwFrameInterval", "333333\n", error) &&
              make_dirs(streaming + "/header/h", error);
    for (const char* speed : {"fs", "hs", "ss"}) ok = ok && make_dirs(streaming + "/class/" + speed, error);
    auto link = [&](const std::string& target, const std::string& path) {
        if (ok && symlink(target.c_str(), path.c_str()) != 0) { ok = false; error = path + ": " + std::strerror(errno); }
    };
    link(picture, streaming + "/header/h/" + short_name);
    for (const char* speed : {"fs", "hs", "ss"}) link(streaming + "/header/h", streaming + "/class/" + speed + "/h");
    for (const char* speed : {"fs", "ss"}) link(control + "/header/h", control + "/class/" + speed + "/h");
    return ok;
}

// What the function was made for, read back from its frame.
bool mirror_made_for(const std::string& function, const MirrorFormat& format) {
    std::string frame = function + (format.mjpeg ? "/streaming/mjpeg/m/f" : "/streaming/uncompressed/u/f"), width, height;
    return read_line(frame + "/wWidth", width) && read_line(frame + "/wHeight", height) &&
           width == std::to_string(format.width) && height == std::to_string(format.height);
}
} // namespace

MirrorLink gadget_add_mirror(const MirrorFormat& format, std::string& error, const std::string& root) {
    std::lock_guard<std::recursive_mutex> lock(relinking);
    std::string gadget, config, udc;
    if (!first_gadget(root, gadget, config)) { error = "no USB gadget"; return MirrorLink::Failed; }
    if (!read_line(gadget + "/UDC", udc)) { error = gadget + "/UDC unreadable"; return MirrorLink::Failed; }
    std::string function = gadget + "/functions/" + mirror_function;
    auto links = function_links(gadget);
    if (storage_linked(links)) return MirrorLink::CardShared;
    bool linked = std::any_of(links.begin(), links.end(), [&](const Link& link) { return link.target == function; });
    if (linked && mirror_made_for(function, format)) return MirrorLink::Linked;
    // Another size: configfs takes no change while the function is linked, so out with it and made anew.
    if (linked && !gadget_remove_mirror(error, root)) return MirrorLink::Failed;
    if (exists(function)) { remove_tree(function); rmdir(function.c_str()); }
    links = function_links(gadget);
    if (!make_mirror(function, format, error)) {
        remove_tree(function); rmdir(function.c_str());
        return MirrorLink::Failed;
    }
    // After the UAC2 function (its interface numbers, by which macOS knows
    // the audio device, stay), the network function last: the controller
    // hands IN endpoints out in link order and its sixth FIFO (336 bytes)
    // fits only NCM's notify endpoint (gadget_add_storage()).
    Link webcam {config + "/" + mirror_function, function};
    std::vector<Link> wanted, network;
    bool placed = false;
    for (const auto& link : links) {
        if (link_name(link).rfind("ncm.", 0) == 0) { network.push_back(link); continue; }
        wanted.push_back(link);
        if (!placed && link_name(link).rfind("uac2.", 0) == 0) { wanted.push_back(webcam); placed = true; }
    }
    if (!placed) wanted.insert(wanted.begin(), webcam);
    wanted.insert(wanted.end(), network.begin(), network.end());
    if (rebind(gadget, udc, links, [](std::string&) { return true; }, error, &wanted)) return MirrorLink::Linked;
    remove_tree(function); rmdir(function.c_str());
    return MirrorLink::Failed;
}

bool gadget_mirror_linked(const std::string& root) {
    std::lock_guard<std::recursive_mutex> lock(relinking); // never halfway through another thread's relink
    std::string gadget, config;
    if (!first_gadget(root, gadget, config)) return false;
    std::string function = gadget + "/functions/" + mirror_function;
    auto links = function_links(gadget);
    return std::any_of(links.begin(), links.end(), [&](const Link& link) { return link.target == function; });
}

bool gadget_mirror_live(const std::string& marker) { return access(marker.c_str(), F_OK) == 0; }

bool gadget_mirror_format(MirrorFormat& format, const std::string& root) {
    std::lock_guard<std::recursive_mutex> lock(relinking);
    std::string gadget, config;
    if (!first_gadget(root, gadget, config)) return false;
    std::string function = gadget + "/functions/" + mirror_function;
    for (bool mjpeg : {true, false}) {
        std::string frame = function + (mjpeg ? "/streaming/mjpeg/m/f" : "/streaming/uncompressed/u/f"), width, height;
        if (read_line(frame + "/wWidth", width) && read_line(frame + "/wHeight", height)) {
            format = {unsigned(std::strtoul(width.c_str(), nullptr, 10)), unsigned(std::strtoul(height.c_str(), nullptr, 10)), mjpeg};
            return format.width && format.height;
        }
    }
    return false;
}

bool gadget_remove_mirror(std::string& error, const std::string& root) {
    std::lock_guard<std::recursive_mutex> lock(relinking);
    std::string gadget, config, udc;
    if (!first_gadget(root, gadget, config)) return true;
    std::string function = gadget + "/functions/" + mirror_function;
    if (!exists(function)) return true;
    if (!read_line(gadget + "/UDC", udc)) { error = gadget + "/UDC unreadable"; return false; }
    auto links = function_links(gadget);
    if (std::any_of(links.begin(), links.end(), [&](const Link& link) { return link.target == function; })) {
        std::vector<Link> wanted;
        for (const auto& link : links) if (link.target != function) wanted.push_back(link);
        if (!rebind(gadget, udc, links, [](std::string&) { return true; }, error, &wanted)) return false;
    }
    remove_tree(function);
    if (rmdir(function.c_str()) != 0 && errno != ENOENT) { error = function + ": " + std::strerror(errno); return false; }
    return true;
}

std::string describe(const GadgetChange& change) {
    std::ostringstream out;
    switch (change.result) {
    case GadgetChange::Result::Missing: out << "no UAC2 gadget function"; break;
    case GadgetChange::Result::Unchanged: out << change.function << (change.rate ? " already p_srate=" : " already p_chmask=") << change.after; break;
    case GadgetChange::Result::Changed:
        out << change.function << (change.rate ? " p_srate " : " p_chmask ") << change.before << " -> " << change.after; break;
    case GadgetChange::Result::Failed: out << "gadget change failed: " << change.error; break;
    }
    return out.str();
}
