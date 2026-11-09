// SPDX-License-Identifier: AGPL-3.0-only
#include "drivers/power_button.h"
#include <linux/input.h>
#include <cassert>
#include <filesystem>
#include <fstream>
#include <initializer_list>

namespace fs = std::filesystem;

static void device(const fs::path& root, const char* node, const char* name) {
    fs::create_directories(root / "class" / node / "device");
    std::ofstream(root / "class" / node / "device/name") << name << '\n';
}
static void append(const fs::path& file, std::initializer_list<input_event> events) {
    std::ofstream out(file, std::ios::binary | std::ios::app);
    for (const auto& event : events) out.write(reinterpret_cast<const char*>(&event), sizeof(event));
}
static input_event key(unsigned code, int value, long seconds = 7) {
    input_event event {}; event.type = EV_KEY; event.code = code; event.value = value;
    event.input_event_sec = seconds; event.input_event_usec = 250;
    return event;
}

int main(int argc, char** argv) {
    assert(argc == 2);
    fs::path root = argv[1];
    fs::create_directories(root / "dev");
    {
        PowerButton missing(root / "class", root / "dev");
        assert(!missing.available() && missing.poll() == 0);
        assert(missing.json().find("\"available\":false") != std::string::npos);
    }
    device(root, "input0", "rk805 pwrkey");    // Not an event node.
    device(root, "event1", "I2S Headphones");
    device(root, "event4", "rk805 pwrkey");
    std::ofstream(root / "dev/event1").put('x');
    std::ofstream(root / "dev/event4");
    PowerButton power(root / "class", root / "dev");
    assert(power.available() && power.poll() == 0 && !power.pressed());

    input_event sync {}; sync.type = EV_SYN;
    append(root / "dev/event4", {key(KEY_POWER, 1), sync, key(KEY_POWER, 2), key(KEY_VOLUMEUP, 1), key(KEY_POWER, 0, 8)});
    assert(power.poll() == 2 && !power.pressed() && power.presses() == 1);
    assert(power.json().find("\"timestamp_ns\":8000250000") != std::string::npos);
    append(root / "dev/event4", {key(KEY_POWER, 1, 9), key(KEY_POWER, 1, 9)}); // A repeated press counts once.
    assert(power.poll() == 1 && power.pressed() && power.presses() == 2);
    auto json = power.json();
    assert(json.find("event4\"") != std::string::npos && json.find("\"key\":\"KEY_POWER\",\"pressed\":true,\"presses\":2") != std::string::npos);
}
