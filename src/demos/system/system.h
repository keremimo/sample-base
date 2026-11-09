// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include "drivers/platform.h"
#include "drivers/usb_audio.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

// The system page's model: what the service thread reads about the MPC,
// the CPU meter and the text the page shows. Portable and file-free; the
// demo (system_demo) does the reading and the page (system_page) the drawing.
namespace sysinfo {

// The page's panels, stepped through by the buttons above the screen.
// `panel_names` are what the `system` command accepts.
enum class Panel : uint8_t { Health, Audio, Ports, Storage, Count };
constexpr std::array<const char*, size_t(Panel::Count)> panel_names {{"health", "audio", "ports", "storage"}};
constexpr std::array<const char*, size_t(Panel::Count)> panel_titles {{"Health", "Audio", "Ports", "Storage"}};
inline Panel step_panel(Panel panel, int delta) {
    int count = int(Panel::Count);
    return Panel(((int(panel) + delta) % count + count) % count);
}

// Reads happen every half second while the page is open, storage every
// two seconds; nothing is read while it is closed.
constexpr unsigned read_ms = 500, storage_ms = 2000;
constexpr unsigned history_length = 48; // 24 seconds of CPU load
constexpr unsigned max_ports = 6, max_mounts = 5;

struct Port { std::string name; bool input = false, output = false, surface = false; };
struct Mount { std::string path, type; bool read_only = true; uint64_t free_bytes = 0, total_bytes = 0; };

// CPU load, busiest share in percent, oldest first.
struct History {
    std::array<uint8_t, history_length> percent {};
    unsigned count = 0;
    void push(float load) {
        std::rotate(percent.begin(), percent.begin() + 1, percent.end());
        percent.back() = uint8_t(std::lround(std::clamp(load, 0.0f, 1.0f) * 100));
        count = std::min(count + 1, history_length);
    }
};

// Everything the service thread reads for the page. -1, empty and nullopt
// mean the device did not report it.
struct Readings {
    unsigned reads = 0; // Completed reads since the start; 0 before the first.

    // Health
    std::vector<float> cpu; // Total, then each core, 0..1; empty until two samples.
    History history;
    uint64_t memory_total_kb = 0, memory_available_kb = 0;
    std::optional<long> cpu_millidegrees, gpu_millidegrees;
    std::string supply_name, supply_type;
    int supply_online = -1;
    long supply_microvolts = -1;
    bool key_available = false, key_down = false;
    uint64_t key_presses = 0;
    long backlight = -1, backlight_maximum = -1;

    // USB-C: the device controller and both audio directions.
    std::string udc_state;
    UsbStreamStats usb_in, usb_out;

    // Ports: MIDI and the jack detection switches (1 in, 0 out, -1 unknown).
    std::vector<Port> ports;
    unsigned send_drops = 0, input_errors = 0;
    int headphones = -1, line_out = -1;

    std::vector<Mount> storage;
};

// What the page's parts share across threads. The MIDI thread and the
// `system` command pick the panel; the service thread writes `readings` and
// the screen thread copies them, both under `lock`, which the audio thread
// never takes.
struct Shared {
    std::atomic<Panel> panel {Panel::Health};
    mutable std::mutex lock;
    Readings readings;
    Readings copy() const { std::lock_guard<std::mutex> hold(lock); return readings; }
};

// Turns /proc/stat readings into loads: the share of time each CPU was busy
// since the last sample.
class CpuMeter {
public:
    // Total, then each core. Empty for the first sample, or when the number
    // of CPUs changed since the last one.
    std::vector<float> sample(const std::vector<CpuTimes>& now) {
        std::vector<float> loads;
        if (now.size() == previous.size())
            for (size_t n = 0; n < now.size(); ++n) loads.push_back(float(cpu_load(previous[n], now[n])));
        previous = now;
        return loads;
    }
    void reset() { previous.clear(); }
private:
    std::vector<CpuTimes> previous;
};

// The first thermal zone whose type starts with `prefix` ("cpu" finds
// "cpu-thermal").
inline std::optional<long> zone_temperature(const std::vector<ThermalReading>& zones, const std::string& prefix) {
    for (const auto& zone : zones)
        if (zone.type.rfind(prefix, 0) == 0) return zone.millidegrees;
    return std::nullopt;
}

// The supply to show: the first that reports a voltage, else the first.
inline const SupplyReading* main_supply(const std::vector<SupplyReading>& supplies) {
    for (const auto& supply : supplies)
        if (supply.microvolts >= 0) return &supply;
    return supplies.empty() ? nullptr : &supplies.front();
}

inline float memory_used(const Readings& readings) {
    if (!readings.memory_total_kb) return 0;
    auto available = std::min(readings.memory_available_kb, readings.memory_total_kb);
    return float(readings.memory_total_kb - available) / float(readings.memory_total_kb);
}

inline float storage_used(const Mount& mount) {
    if (!mount.total_bytes) return 0;
    return float(mount.total_bytes - std::min(mount.free_bytes, mount.total_bytes)) / float(mount.total_bytes);
}

// --- The page's text ---

inline std::string format(const char* pattern, double value) {
    char text[32];
    snprintf(text, sizeof(text), pattern, value);
    return text;
}

// 1024-based, one decimal below ten: "512 B", "9.5 MB", "359 MB", "1.9 GB".
inline std::string bytes_text(uint64_t bytes) {
    const char* units[] {"B", "KB", "MB", "GB", "TB"};
    double value = double(bytes);
    unsigned unit = 0;
    while (value >= 1024 && unit < 4) { value /= 1024; ++unit; }
    if (unit == 0) return std::to_string(bytes) + " B";
    std::string number = value < 9.95 ? format("%.1f", value) : format("%.0f", value);
    return number + ' ' + units[unit];
}

inline std::string volts_text(long microvolts) { return microvolts < 0 ? "n/a" : format("%.2f V", microvolts / 1e6); }
inline std::string celsius_text(std::optional<long> millidegrees) {
    return millidegrees ? format("%.1f", *millidegrees / 1000.0) + " °C" : "n/a";
}
inline std::string percent_text(float share) { return format("%.0f%%", double(std::clamp(share, 0.0f, 1.0f)) * 100); }
// "48 kHz", "44.1 kHz", or a dash while nothing streams.
inline std::string rate_text(unsigned hertz) {
    if (!hertz) return "-";
    return hertz % 1000 ? format("%.1f kHz", hertz / 1000.0) : std::to_string(hertz / 1000) + " kHz";
}
inline std::string ppm_text(int ppm) { return (ppm > 0 ? "+" : "") + std::to_string(ppm) + " ppm"; }
// Frames played at `rate` as h:mm:ss, or m:ss under an hour.
inline std::string duration_text(uint64_t frames, unsigned rate) {
    uint64_t seconds = rate ? frames / rate : 0;
    char text[32];
    if (seconds >= 3600) snprintf(text, sizeof(text), "%llu:%02u:%02u", static_cast<unsigned long long>(seconds / 3600),
                                  unsigned(seconds / 60 % 60), unsigned(seconds % 60));
    else snprintf(text, sizeof(text), "%u:%02u", unsigned(seconds / 60), unsigned(seconds % 60));
    return text;
}
inline const char* jack_text(int value) { return value < 0 ? "n/a" : value ? "Plugged in" : "Empty"; }
inline const char* key_text(const Readings& readings) {
    return !readings.key_available ? "n/a" : readings.key_down ? "held" : "up";
}
// The stream state with a capital, as the page shows it.
inline const char* usb_state_text(UsbStreamState state) {
    constexpr std::array<const char*, size_t(UsbStreamState::Count)> names {{"Off", "No gadget", "Waiting", "Running", "Failed"}};
    return state < UsbStreamState::Count ? names[size_t(state)] : "?";
}

} // namespace sysinfo
