// SPDX-License-Identifier: AGPL-3.0-only
#include "runtime/inventory.h"
#include "drivers/audio.h"
#include "drivers/hardware_midi.h"
#include "drivers/mixer.h"
#include "drivers/platform.h"
#include "drivers/system_services.h"
#include <iostream>

int print_hardware() {
    Platform platform; HardwareMixer mixer; SystemServices services;
    std::cout << "{\"platform\":" << platform.json() << ",\"audio\":" << AudioDevice::devices_json()
              << ",\"mixer\":" << mixer.json() << ",\"storage\":" << services.storage_json() << ",\"midi\":[";
    bool first = true;
    for (const auto& port : HardwareMidi::enumerate()) {
        if (!first) std::cout << ',';
        first = false;
        std::cout << "{\"address\":" << json_string(port.address) << ",\"card\":" << json_string(port.card)
                  << ",\"name\":" << json_string(port.name) << ",\"input\":" << (port.input ? "true" : "false")
                  << ",\"output\":" << (port.output ? "true" : "false") << '}';
    }
    std::cout << "]}\n"; return 0;
}
