// SPDX-License-Identifier: AGPL-3.0-only
#include "power_button.h"
#include "platform.h"
#include <linux/input.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <cerrno>
#include <chrono>
#include <ctime>
#include <fstream>
#include <sstream>
#include <system_error>

namespace {
uint64_t monotonic_ns() {
    return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}
}

PowerButton::PowerButton(std::filesystem::path input_class, std::filesystem::path devices)
    : input_class(std::move(input_class)), devices(std::move(devices)) { open_device(); }
PowerButton::~PowerButton() { if (fd >= 0) close(fd); }

void PowerButton::open_device() {
    retry_ns = monotonic_ns() + 1000000000ull;
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(input_class, error)) {
        auto node = entry.path().filename().string();
        if (node.rfind("event", 0) != 0) continue;
        std::ifstream file(entry.path() / "device/name"); std::string name;
        if (!std::getline(file, name) || name != device_name) continue;
        auto candidate = devices / node;
        fd = open(candidate.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) continue;
        int clock = CLOCK_MONOTONIC; // Match MIDI receipt timestamps.
        ioctl(fd, EVIOCSCLOCKID, &clock);
        path = candidate; return;
    }
}

unsigned PowerButton::poll() {
    if (fd < 0) {
        if (monotonic_ns() >= retry_ns) open_device();
        if (fd < 0) return 0;
    }
    unsigned changes = 0;
    input_event events[16];
    for (unsigned batch = 0; batch < 4; ++batch) {
        ssize_t size = read(fd, events, sizeof(events));
        if (size < 0 && errno != EAGAIN && errno != EINTR) { close(fd); fd = -1; down = false; retry_ns = monotonic_ns() + 1000000000ull; break; }
        if (size <= 0) break;
        for (unsigned n = 0; n < unsigned(size) / sizeof(events[0]); ++n) {
            const auto& event = events[n];
            if (event.type != EV_KEY || event.code != KEY_POWER || event.value == 2) continue; // 2 is autorepeat.
            bool pressed = event.value != 0;
            if (pressed == down) continue;
            down = pressed; ++changes;
            if (pressed) ++press_count;
            timestamp_ns = uint64_t(event.input_event_sec) * 1000000000ull + uint64_t(event.input_event_usec) * 1000ull;
        }
    }
    return changes;
}

std::string PowerButton::json() const {
    std::ostringstream out;
    out << "{\"available\":" << (fd >= 0 ? "true" : "false") << ",\"device\":" << json_string(fd >= 0 ? path.string() : "")
        << ",\"key\":\"KEY_POWER\",\"pressed\":" << (down ? "true" : "false") << ",\"presses\":" << press_count
        << ",\"timestamp_ns\":" << timestamp_ns << '}';
    return out.str();
}
