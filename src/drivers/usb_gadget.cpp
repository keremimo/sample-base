// SPDX-License-Identifier: AGPL-3.0-only
#include "drivers/usb_gadget.h"
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
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
    if (!ok) error = path + ": " + std::strerror(saved_errno);
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
    struct stat info;
    if (stat(saved.c_str(), &info) != 0 &&
        !write_text(saved, change.function + " " + std::to_string(change.before) + "\n", change.error, O_WRONLY | O_CREAT | O_EXCL)) {
        change.result = GadgetChange::Result::Failed; return change;
    }
    // f_uac2 refuses a new mask while any configuration links the function,
    // bound or not, so every link comes out and goes back in its old order.
    // USB is also how SSH reaches the MPC: when the wide gadget will not
    // bind, the original mask goes back the same way.
    auto links = function_links(gadget);
    auto swap = [&](unsigned mask, std::string& error) {
        bool ok = udc.empty() || write_text(gadget + "/UDC", "\n", error);
        std::vector<Link> removed;
        for (const auto& link : links) {
            if (!ok) break;
            if (unlink(link.path.c_str()) == 0 || errno == ENOENT) removed.push_back(link); // gone: a failed try left it out
            else { ok = false; error = link.path + ": " + std::strerror(errno); }
        }
        ok = ok && write_text(change.function + "/p_chmask", std::to_string(mask) + "\n", error);
        for (const auto& link : removed)
            if (symlink(link.target.c_str(), link.path.c_str()) != 0 && ok) {
                ok = false; error = link.path + ": " + std::strerror(errno);
            }
        std::string rebind_error;
        if (!udc.empty() && !write_text(gadget + "/UDC", udc + "\n", rebind_error)) {
            if (ok) error = rebind_error;
            ok = false;
        }
        return ok;
    };
    bool ok = swap(change.after, change.error);
    if (!ok) {
        std::string back_error;
        change.error += swap(change.before, back_error) ? " (original mask restored)" : "; restoring: " + back_error;
    }
    change.result = ok ? GadgetChange::Result::Changed : GadgetChange::Result::Failed;
    return change;
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
