// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

std::string json_string(const std::string& value);

struct StorageMount {
    std::string device, path, type;
    bool read_only = true;
};
std::vector<StorageMount> parse_mountinfo(const std::string& text);

// CPU time from /proc/stat in clock ticks: busy (user, nice, system, irq,
// softirq, steal) and the total including idle and iowait. The first entry
// sums every CPU; cpu0, cpu1 ... follow. Guest time is already in user.
struct CpuTimes { uint64_t busy = 0, total = 0; };
std::vector<CpuTimes> parse_proc_stat(const std::string& text);
// The busy share of the time between two readings, 0..1; 0 when no time
// passed or the counters went backwards.
double cpu_load(const CpuTimes& before, const CpuTimes& after);

// /proc/meminfo in kB, 0 where a line is missing. Kernels without
// MemAvailable report MemFree instead.
struct MemoryReading { uint64_t total_kb = 0, available_kb = 0; };
MemoryReading parse_meminfo(const std::string& text);

// Typed readings for an app's own screen; json() reports everything as text.
// Values the device lacks stay -1 or empty: never a made-up battery level.
struct SupplyReading { std::string name, type; int online = -1; long microvolts = -1; };
struct ThermalReading { std::string type; long millidegrees = 0; };
struct BacklightReading { long brightness = -1, maximum = -1; };
struct StorageSpace { std::string device, path, type; bool read_only = true; uint64_t free_bytes = 0, total_bytes = 0; };

// All file access belongs on the control/service thread. Roots are injectable
// for offline tests. No raw GPIO, I2C, regulator, gadget or block-device writes.
class Platform {
public:
    explicit Platform(std::filesystem::path sys = "/sys", std::filesystem::path proc = "/proc") : sys(std::move(sys)), proc(std::move(proc)) {}
    ~Platform();
    std::string json() const;
    bool brightness(unsigned value);

    std::vector<SupplyReading> supplies() const;      // Every power supply
    std::vector<ThermalReading> temperatures() const; // Every thermal zone, in zone order
    BacklightReading backlight() const;               // The first backlight
    std::string usb_state() const;                    // The first USB device controller's state
    // Mounted block devices, each device once (bind mounts of it are left
    // out), with free and total space. statvfs() can wait on a slow card.
    std::vector<StorageSpace> storage() const;
    std::vector<CpuTimes> cpu_times() const;          // parse_proc_stat() of /proc/stat
    MemoryReading memory() const;                     // parse_meminfo() of /proc/meminfo
private:
    std::filesystem::path sys, proc;
    std::filesystem::path changed_backlight;
    std::string saved_brightness;
};
