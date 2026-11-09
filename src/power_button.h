// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <cstdint>
#include <filesystem>
#include <string>

// The power button is not on the surface MIDI port. On the AC50 it is the
// PMIC's "rk805 pwrkey" evdev device, which reports KEY_POWER press and
// release. The device is shared, never grabbed. On 1.3.0.12 logind is not
// running and no udev rule starts a handler for it, so pressing it under
// sample-base only produces these events. The PMIC's own long-press power-off
// remains in hardware. Control thread only; roots are injectable for tests.
class PowerButton {
public:
    static constexpr const char* device_name = "rk805 pwrkey";
    explicit PowerButton(std::filesystem::path input_class = "/sys/class/input", std::filesystem::path devices = "/dev/input");
    ~PowerButton();
    PowerButton(const PowerButton&) = delete;
    PowerButton& operator=(const PowerButton&) = delete;
    // Non-blocking. Returns the number of press/release changes read; a lost
    // device is rediscovered at most once per second.
    unsigned poll();
    bool available() const { return fd >= 0; }
    bool pressed() const { return down; }
    uint64_t presses() const { return press_count; }
    std::string json() const;
private:
    void open_device();
    std::filesystem::path input_class, devices, path;
    int fd = -1;
    bool down = false;
    uint64_t press_count = 0, timestamp_ns = 0, retry_ns = 0;
};
