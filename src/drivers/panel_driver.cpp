// SPDX-License-Identifier: AGPL-3.0-only
#include "drivers/panel_driver.h"
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <cerrno>
#include <cstddef>
#include <cstring>
#include <string>

// Weak: a build without the module (base.mk) has neither, and keeps the stock driver.
extern "C" const unsigned char sample_base_panel_module[] __attribute__((weak));
extern "C" const unsigned char sample_base_panel_module_end[] __attribute__((weak));

namespace {
const std::string device = "/sys/bus/spi/devices/spi3.0";
const std::string drivers = "/sys/bus/spi/drivers/";
constexpr const char* ours = "sample-base-panel";
constexpr const char* stock = "st7796s";

bool put(const std::string& path, const std::string& text) {
    int fd = open(path.c_str(), O_WRONLY | O_CLOEXEC);
    if (fd < 0) return false;
    bool ok = write(fd, text.data(), text.size()) == ssize_t(text.size());
    close(fd);
    return ok;
}

// The driver the panel's SPI device is bound to, or "" for none.
std::string bound() {
    char target[256];
    ssize_t n = readlink((device + "/driver").c_str(), target, sizeof(target) - 1);
    if (n <= 0) return {};
    target[n] = 0;
    const char* slash = std::strrchr(target, '/');
    return slash ? slash + 1 : target;
}

bool loaded() { return access("/sys/module/sample_base_panel", F_OK) == 0; }

// The embedded module into the kernel, from memory (no file on any disk).
bool load() {
    if (loaded()) return true;
    if (!sample_base_panel_module || !sample_base_panel_module_end) return false;
    const size_t size = size_t(sample_base_panel_module_end - sample_base_panel_module);
    if (!size) return false;
    int fd = memfd_create("sample-base-panel", MFD_CLOEXEC);
    if (fd < 0) return false;
    bool written = true;
    for (size_t done = 0; done < size && written;) {
        ssize_t n = write(fd, sample_base_panel_module + done, size - done);
        if (n < 0 && errno == EINTR) continue;
        written = n > 0;
        if (written) done += size_t(n);
    }
    long result = written ? syscall(SYS_finit_module, fd, "", 0) : -1;
    int error = errno;
    close(fd);
    return result == 0 || (written && error == EEXIST);
}

void bind(const char* driver) { put(drivers + driver + "/bind", "spi3.0"); }
void unbind(const char* driver) { put(drivers + driver + "/unbind", "spi3.0"); }
void clear_override() { put(device + "/driver_override", "\n"); }
}

bool panel_driver::take_over() {
    if (access(device.c_str(), F_OK) != 0) return false;
    const std::string now = bound();
    if (now == ours) return true; // left there by an app that did not get to give it back
    if (now != stock || !load()) return false;
    if (!put(device + "/driver_override", ours)) return false;
    unbind(stock);
    bind(ours);
    if (bound() == ours) return true;
    // It did not take: back as it was.
    clear_override();
    if (bound().empty()) bind(stock);
    return false;
}

void panel_driver::give_back() {
    if (access(device.c_str(), F_OK) != 0) return;
    if (bound() == ours) unbind(ours);
    clear_override();
    if (bound().empty()) bind(stock);
    if (loaded()) syscall(SYS_delete_module, "sample_base_panel", O_NONBLOCK);
}
