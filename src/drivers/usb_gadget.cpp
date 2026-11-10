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
#include <sstream>
#include <sys/stat.h>
#include <vector>

namespace {
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
} // namespace

GadgetChange widen_uac2_playback(unsigned channels, const std::string& root, const std::string& saved) {
    GadgetChange change;
    if (channels < 1 || channels > 27) { change.result = GadgetChange::Result::Failed; change.error = "1 to 27 channels"; return change; }
    std::string gadget;
    for (const auto& name : entries(root)) {
        for (const auto& function : entries(root + "/" + name + "/functions"))
            if (function.rfind("uac2.", 0) == 0) { gadget = root + "/" + name; change.function = gadget + "/functions/" + function; break; }
        if (!gadget.empty()) break;
    }
    if (gadget.empty()) return change;

    std::string text, udc;
    if (!read_line(change.function + "/p_chmask", text) || !parse_mask(text, change.before)) {
        change.result = GadgetChange::Result::Failed; change.error = change.function + "/p_chmask unreadable"; return change;
    }
    change.after = (1u << channels) - 1;
    if (change.before == change.after) { change.result = GadgetChange::Result::Unchanged; return change; }
    if (!read_line(gadget + "/UDC", udc)) {
        change.result = GadgetChange::Result::Failed; change.error = gadget + "/UDC unreadable"; return change;
    }
    // The first change saves the original; a later one (another app start)
    // must not overwrite it with the widened mask.
    if (!exists(saved) &&
        !write_text(saved, change.function + " " + std::to_string(change.before) + "\n", change.error, O_WRONLY | O_CREAT | O_EXCL)) {
        change.result = GadgetChange::Result::Failed; return change;
    }
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

bool gadget_add_storage(const std::string& medium, std::vector<std::string>& order, std::string& error, const std::string& root) {
    std::string gadget, config, udc;
    if (!first_gadget(root, gadget, config)) { error = "no USB gadget"; return false; }
    if (!read_line(gadget + "/UDC", udc)) { error = gadget + "/UDC unreadable"; return false; }
    std::string function = gadget + "/functions/" + card_function, lun = function + "/lun.0";
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
    // keep their numbers.
    std::vector<Link> wanted, network;
    for (const auto& link : links) (link_name(link).rfind("ncm.", 0) == 0 ? network : wanted).push_back(link);
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
    std::string gadget, config, udc;
    if (!first_gadget(root, gadget, config)) return true;
    std::string function = gadget + "/functions/" + card_function;
    if (!exists(function)) return true;
    if (!read_line(gadget + "/UDC", udc)) { error = gadget + "/UDC unreadable"; return false; }
    auto links = function_links(gadget);
    bool linked = std::any_of(links.begin(), links.end(), [&](const Link& link) { return link.target == function; });
    if (linked) {
        // Back in the order before the drive came (what is not named keeps its place after).
        std::vector<Link> wanted;
        for (const auto& name : order)
            for (const auto& link : links)
                if (link_name(link) == name && link.target != function) wanted.push_back(link);
        for (const auto& link : links)
            if (link.target != function && std::none_of(wanted.begin(), wanted.end(), [&](const Link& w) { return w.path == link.path; }))
                wanted.push_back(link);
        if (!rebind(gadget, udc, links, [](std::string&) { return true; }, error, &wanted)) return false;
    }
    if (rmdir(function.c_str()) != 0 && errno != ENOENT) { error = function + ": " + std::strerror(errno); return false; }
    return true;
}

std::string describe(const GadgetChange& change) {
    std::ostringstream out;
    switch (change.result) {
    case GadgetChange::Result::Missing: out << "no UAC2 gadget function"; break;
    case GadgetChange::Result::Unchanged: out << change.function << " already p_chmask=" << change.after; break;
    case GadgetChange::Result::Changed: out << change.function << " p_chmask " << change.before << " -> " << change.after; break;
    case GadgetChange::Result::Failed: out << "gadget change failed: " << change.error; break;
    }
    return out.str();
}
