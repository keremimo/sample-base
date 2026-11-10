// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <systemd/sd-bus.h>
#include <string>
#include <cstdint>
#include <vector>

// One of the storage service's file systems (edisksd's Filesystem objects).
struct StorageVolume {
    std::string object, device, sys_path, type, media, label; // object: the service's path for mount requests
    bool removable = false, read_only = false, ignore = false; // ignore: HintIgnore, the MPC's own partitions
    std::vector<std::string> mountpoints;
};

// Optional system bus: existing storage service owns mounts, logind owns
// shutdown/suspend. Missing services never prevent the instrument starting.
class SystemServices {
public:
    SystemServices();
    ~SystemServices();
    SystemServices(const SystemServices&) = delete;
    SystemServices& operator=(const SystemServices&) = delete;
    void poll();
    bool stopping() const { return stop_requested; }
    std::string storage_json();
    std::vector<StorageVolume> volumes(); // Empty when the service is unavailable
    bool mount(const std::string& object, bool mounted);
    bool available() const { return bus != nullptr; }
    uint64_t storage_generation() const { return generation; }
private:
    static int lifecycle(sd_bus_message* message, void* userdata, sd_bus_error* error);
    static int storage_event(sd_bus_message* message, void* userdata, sd_bus_error* error);
    sd_bus* bus = nullptr;
    sd_bus_slot *sleep_slot = nullptr, *shutdown_slot = nullptr;
    sd_bus_slot *added_slot = nullptr, *removed_slot = nullptr, *property_slot = nullptr;
    uint64_t generation = 0;
    bool stop_requested = false;
};
