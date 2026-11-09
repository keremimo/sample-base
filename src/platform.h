// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include "midi.h"
#include <filesystem>
#include <string>
#include <vector>

std::string json_string(const std::string& value);
bool parse_midi_hex(const std::string& text, MidiPacket& packet);

struct StorageMount {
    std::string device, path, type;
    bool read_only = true;
};
std::vector<StorageMount> parse_mountinfo(const std::string& text);

// All file access belongs on the control/service thread. Roots are injectable
// for offline tests. No raw GPIO, I2C, regulator, gadget or block-device writes.
class Platform {
public:
    explicit Platform(std::filesystem::path sys = "/sys", std::filesystem::path proc = "/proc") : sys(std::move(sys)), proc(std::move(proc)) {}
    ~Platform();
    std::string json() const;
    bool brightness(unsigned value);
private:
    std::filesystem::path sys, proc;
    std::filesystem::path changed_backlight;
    std::string saved_brightness;
};
