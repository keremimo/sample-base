// SPDX-License-Identifier: AGPL-3.0-only
#include "drivers/platform.h"
#include <algorithm>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <sys/statvfs.h>

namespace {
std::string read_text(const std::filesystem::path& path, unsigned limit = 4096) {
    std::ifstream file(path, std::ios::binary);
    std::string value;
    char byte;
    while (value.size() < limit && file.get(byte)) value += byte;
    while (!value.empty() && (value.back() == '\n' || value.back() == '\0')) value.pop_back();
    return value;
}
std::vector<std::filesystem::path> entries(const std::filesystem::path& root) {
    std::vector<std::filesystem::path> result;
    std::error_code error;
    for (std::filesystem::directory_iterator it(root, error), end; !error && it != end && result.size() < 64; it.increment(error)) result.push_back(it->path());
    std::sort(result.begin(), result.end());
    return result;
}
// A whole decimal number from a sysfs file, or `missing` without one.
long read_number(const std::filesystem::path& path, long missing = -1) {
    auto text = read_text(path, 64);
    try {
        size_t end = 0;
        long value = std::stol(text, &end);
        return end == text.size() ? value : missing;
    } catch (...) { return missing; }
}
bool write_text(const std::filesystem::path& path, const std::string& value) {
    std::ofstream output(path); output << value << '\n'; output.close(); return bool(output);
}
std::string unescape_mount(const std::string& text) {
    std::string out;
    for (unsigned i = 0; i < text.size(); ++i) {
        if (text[i] == '\\' && i + 3 < text.size() &&
            text[i+1] >= '0' && text[i+1] <= '7' && text[i+2] >= '0' && text[i+2] <= '7' && text[i+3] >= '0' && text[i+3] <= '7') {
            out += char((text[i+1]-'0')*64 + (text[i+2]-'0')*8 + text[i+3]-'0'); i += 3;
        } else out += text[i];
    }
    return out;
}
}
std::string json_string(const std::string& value) {
    std::ostringstream out; out << '"';
    for (unsigned char byte : value) {
        if (byte == '"' || byte == '\\') out << '\\' << char(byte);
        else if (byte < 32) out << "\\u" << std::hex << std::setfill('0') << std::setw(4) << unsigned(byte);
        else out << char(byte);
    }
    return out.str() + '"';
}
std::vector<StorageMount> parse_mountinfo(const std::string& text) {
    std::vector<StorageMount> result;
    std::istringstream input(text); std::string line;
    while (std::getline(input, line) && result.size() < 128) {
        std::istringstream fields(line);
        std::string id, parent, device, root, path, options, word, type, source, super_options;
        if (!(fields >> id >> parent >> device >> root >> path >> options)) continue;
        while (fields >> word && word != "-") {}
        if (word != "-" || !(fields >> type >> source >> super_options) || source.rfind("/dev/", 0) != 0) continue;
        std::string combined = "," + options + "," + super_options + ",";
        result.push_back({unescape_mount(source), unescape_mount(path), type, combined.find(",ro,") != std::string::npos});
    }
    return result;
}
std::vector<CpuTimes> parse_proc_stat(const std::string& text) {
    std::vector<CpuTimes> result;
    std::istringstream input(text); std::string line;
    while (std::getline(input, line) && result.size() < 65) {
        std::istringstream fields(line); std::string name;
        fields >> name;
        if (name.rfind("cpu", 0) != 0) continue;
        // user nice system idle iowait irq softirq steal; guest and guest_nice are inside user and nice.
        uint64_t value[8] {};
        unsigned count = 0;
        while (count < 8 && fields >> value[count]) ++count;
        if (count < 4) continue;
        uint64_t idle = value[3] + value[4];
        uint64_t busy = value[0] + value[1] + value[2] + value[5] + value[6] + value[7];
        result.push_back({busy, busy + idle});
    }
    return result;
}
double cpu_load(const CpuTimes& before, const CpuTimes& after) {
    if (after.total <= before.total || after.busy < before.busy) return 0;
    return std::min(1.0, double(after.busy - before.busy) / double(after.total - before.total));
}
MemoryReading parse_meminfo(const std::string& text) {
    MemoryReading result;
    uint64_t free_kb = 0;
    bool available = false;
    std::istringstream input(text); std::string line;
    while (std::getline(input, line)) {
        std::istringstream fields(line); std::string name; uint64_t value = 0;
        if (!(fields >> name >> value)) continue;
        if (name == "MemTotal:") result.total_kb = value;
        else if (name == "MemFree:") free_kb = value;
        else if (name == "MemAvailable:") { result.available_kb = value; available = true; }
    }
    if (!available) result.available_kb = free_kb;
    return result;
}
Platform::~Platform() {
    if (!changed_backlight.empty()) write_text(changed_backlight / "brightness", saved_brightness);
}
bool Platform::brightness(unsigned value) {
    for (const auto& path : entries(sys / "class/backlight")) {
        std::string maximum = read_text(path / "max_brightness"), current = read_text(path / "brightness");
        try {
            size_t end = 0;
            unsigned long max = std::stoul(maximum, &end);
            if (end != maximum.size() || value > max || current.empty()) continue;
        } catch (...) { continue; }
        if (!changed_backlight.empty() && changed_backlight != path) return false;
        if (changed_backlight.empty()) { changed_backlight = path; saved_brightness = current; }
        return write_text(path / "brightness", std::to_string(value));
    }
    return false;
}
std::string Platform::json() const {
    std::ostringstream out;
    out << "{\"model\":" << json_string(read_text(sys / "firmware/devicetree/base/model")) << ",\"power\":[";
    bool first = true;
    for (const auto& path : entries(sys / "class/power_supply")) {
        if (!first) out << ',';
        first = false;
        out << "{\"name\":" << json_string(path.filename().string());
        for (const auto* key : {"type", "online", "status", "capacity", "voltage_now", "current_now", "temp"}) {
            auto value = read_text(path / key);
            if (!value.empty()) out << ',' << json_string(key) << ':' << json_string(value);
        }
        out << '}';
    }
    out << "],\"backlight\":["; first = true;
    for (const auto& path : entries(sys / "class/backlight")) {
        if (!first) out << ',';
        first = false;
        out << "{\"name\":" << json_string(path.filename().string()) << ",\"brightness\":" << json_string(read_text(path / "brightness"))
            << ",\"maximum\":" << json_string(read_text(path / "max_brightness")) << '}';
    }
    out << "],\"usb\":["; first = true;
    for (const auto& path : entries(sys / "class/udc")) {
        if (!first) out << ',';
        first = false;
        out << "{\"controller\":" << json_string(path.filename().string()) << ",\"state\":" << json_string(read_text(path / "state")) << '}';
    }
    out << "],\"storage\":["; first = true;
    for (const auto& mount : parse_mountinfo(read_text(proc / "self/mountinfo", 262144))) {
        if (!first) out << ',';
        first = false;
        out << "{\"device\":" << json_string(mount.device) << ",\"path\":" << json_string(mount.path)
            << ",\"type\":" << json_string(mount.type) << ",\"read_only\":" << (mount.read_only ? "true" : "false");
        struct statvfs stat {};
        if (statvfs(mount.path.c_str(), &stat) == 0)
            out << ",\"free_bytes\":" << uint64_t(stat.f_bavail) * stat.f_frsize;
        out << '}';
    }
    out << "],\"thermal\":["; first = true;
    for (const auto& path : entries(sys / "class/thermal")) {
        if (path.filename().string().rfind("thermal_zone", 0) != 0) continue;
        if (!first) out << ',';
        first = false;
        out << "{\"type\":" << json_string(read_text(path / "type")) << ",\"millidegrees\":" << json_string(read_text(path / "temp")) << '}';
    }
    out << "],\"network\":["; first = true;
    for (const auto& path : entries(sys / "class/net")) {
        if (!first) out << ',';
        first = false;
        out << "{\"name\":" << json_string(path.filename().string()) << ",\"state\":" << json_string(read_text(path / "operstate")) << '}';
    }
    return out.str() + "]}";
}

std::vector<SupplyReading> Platform::supplies() const {
    std::vector<SupplyReading> result;
    for (const auto& path : entries(sys / "class/power_supply"))
        result.push_back({path.filename().string(), read_text(path / "type"), int(read_number(path / "online")), read_number(path / "voltage_now")});
    return result;
}
std::vector<ThermalReading> Platform::temperatures() const {
    std::vector<ThermalReading> result;
    for (const auto& path : entries(sys / "class/thermal"))
        if (path.filename().string().rfind("thermal_zone", 0) == 0) result.push_back({read_text(path / "type"), read_number(path / "temp", 0)});
    return result;
}
BacklightReading Platform::backlight() const {
    for (const auto& path : entries(sys / "class/backlight")) return {read_number(path / "brightness"), read_number(path / "max_brightness")};
    return {};
}
std::string Platform::usb_state() const {
    for (const auto& path : entries(sys / "class/udc")) return read_text(path / "state");
    return {};
}
std::vector<StorageSpace> Platform::storage() const {
    std::vector<StorageSpace> result;
    for (const auto& mount : parse_mountinfo(read_text(proc / "self/mountinfo", 262144))) {
        if (std::any_of(result.begin(), result.end(), [&](const auto& seen) { return seen.device == mount.device; })) continue;
        StorageSpace space {mount.device, mount.path, mount.type, mount.read_only};
        struct statvfs stat {};
        if (statvfs(mount.path.c_str(), &stat) == 0) {
            space.free_bytes = uint64_t(stat.f_bavail) * stat.f_frsize;
            space.total_bytes = uint64_t(stat.f_blocks) * stat.f_frsize;
        }
        result.push_back(space);
    }
    return result;
}
std::vector<CpuTimes> Platform::cpu_times() const { return parse_proc_stat(read_text(proc / "stat", 8192)); }
MemoryReading Platform::memory() const { return parse_meminfo(read_text(proc / "meminfo", 16384)); }
