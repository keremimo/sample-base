// SPDX-License-Identifier: AGPL-3.0-only
#include "platform.h"
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
bool parse_midi_hex(const std::string& text, MidiPacket& packet) {
    MidiPacket candidate;
    std::istringstream input(text);
    std::string token;
    while (input >> token) {
        if (token.size() != 2 || candidate.size == candidate.bytes.size()) return false;
        unsigned byte = 0;
        for (char digit : token) {
            byte <<= 4;
            if (digit >= '0' && digit <= '9') byte += digit - '0';
            else if (digit >= 'a' && digit <= 'f') byte += digit - 'a' + 10;
            else if (digit >= 'A' && digit <= 'F') byte += digit - 'A' + 10;
            else return false;
        }
        candidate.bytes[candidate.size++] = uint8_t(byte);
    }
    MidiStreamParser parser; MidiPacket parsed; unsigned messages = 0;
    for (unsigned i = 0; i < candidate.size; ++i) parser.feed(candidate.bytes[i], [&](const auto& complete) { parsed = complete; ++messages; });
    if (messages != 1 || parsed.size != candidate.size || parsed.bytes != candidate.bytes) return false;
    packet = candidate; return true;
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
