// SPDX-License-Identifier: AGPL-3.0-only
#include "platform.h"
#include <cassert>
#include <filesystem>
#include <fstream>
#include <vector>

static void write(const std::filesystem::path& path, const std::string& value) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream(path) << value;
}
int main(int argc, char** argv) {
    assert(argc == 2);
    std::filesystem::path root(argv[1]);
    MidiStreamParser parser;
    std::vector<MidiPacket> messages;
    auto feed = [&](std::initializer_list<uint8_t> bytes) {
        for (auto byte : bytes) parser.feed(byte, [&](const auto& message) { messages.push_back(message); });
    };
    feed({0x90,60,0xf8,100,61,0,0xf2,1,2,0xf1,3,0xf6,0xd0,70,71});
    assert(messages.size() == 8 && messages[0].size == 1 && messages[0].bytes[0] == 0xf8);
    assert(messages[1].size == 3 && messages[1].bytes[1] == 60 && messages[1].bytes[2] == 100);
    assert(messages[2].bytes[1] == 61 && messages[2].bytes[2] == 0);
    assert(messages[3].bytes[0] == 0xf2 && messages[3].size == 3);
    assert(messages[4].bytes[0] == 0xf1 && messages[4].size == 2);
    assert(messages[5].bytes[0] == 0xf6 && messages[5].size == 1);
    assert(messages[7].bytes[0] == 0xd0 && messages[7].bytes[1] == 71);
    messages.clear(); feed({0xf0,0x47,1,0xfa,2,0xf7,60,100});
    assert(messages.size() == 2 && messages[0].bytes[0] == 0xfa && messages[1].size == 5);
    messages.clear(); feed({0xf0});
    for (unsigned i = 0; i < 500; ++i) feed({1});
    feed({0xf8,0xf7,0x90,60,100});
    assert(parser.dropped_sysex() == 1 && messages.size() == 2 && messages[1].bytes[0] == 0x90);
    messages.clear(); feed({0xf0,1,0x80,60,0});
    assert(messages.size() == 1 && messages[0].bytes[0] == 0x80);
    MidiPacket packet;
    for (const auto& valid : {"90 3c 64", "f8", "f0 47 00 7f f7", "C0 01", "f2 00 01"}) assert(parse_midi_hex(valid, packet));
    for (const auto& invalid : {"", "90 3c", "90 3c ff", "f0 47", "f7", "f8 fa", "f0 f8 f7", "90 3c 64 01", "zz", "0x90 3c 64"}) assert(!parse_midi_hex(invalid, packet));
    assert(parse_midi_hex("b0 6f 7f", packet));
    SurfaceState surface; surface.apply(packet); assert(surface.controls[111] == 127);
    assert(parse_midi_hex("90 24 64", packet)); surface.apply(packet); assert(surface.notes[36] == 100);
    assert(parse_midi_hex("a0 24 40", packet)); surface.apply(packet); assert(surface.pressure[36] == 64);
    assert(parse_midi_hex("e0 00 7f", packet)); surface.apply(packet); assert(surface.pitch == 16256);
    assert(parse_midi_hex("d0 40", packet)); surface.apply(packet); assert(surface.channel_pressure == 64);
    assert(parse_midi_hex("80 24 00", packet)); surface.apply(packet); assert(surface.notes[36] == 0);

    auto mounts = parse_mountinfo("31 20 8:1 / /media/My\\040SD rw,nosuid shared:1 - exfat /dev/sda1 rw\n"
                                  "32 20 179:1 / /mnt/internal ro - ext4 /dev/mmcblk0p1 rw\n"
                                  "33 20 0:1 / /proc rw - proc proc rw\nmalformed\n");
    assert(mounts.size() == 2 && mounts[0].path == "/media/My SD" && !mounts[0].read_only && mounts[1].read_only);
    assert(json_string("a\"b\\c\n\x01") == "\"a\\\"b\\\\c\\u000a\\u0001\"");
    write(root / "sys/class/power_supply/mains-power/type", "Mains\n");
    write(root / "sys/class/power_supply/mains-power/online", "1\n");
    write(root / "sys/class/power_supply/mains-power/voltage_now", "5000000\n");
    write(root / "sys/class/backlight/panel/max_brightness", "6\n");
    write(root / "sys/class/backlight/panel/brightness", "4\n");
    write(root / "sys/class/udc/controller/state", "configured\n");
    write(root / "sys/class/thermal/thermal_zone0/type", "soc-thermal\n");
    write(root / "sys/class/thermal/thermal_zone0/temp", "42000\n");
    write(root / "proc/self/mountinfo", "31 20 8:1 / /media/SD rw - exfat /dev/sda1 rw\n");
    {
        Platform platform(root / "sys", root / "proc");
        std::string json = platform.json();
        assert(json.find("5000000") != std::string::npos && json.find("configured") != std::string::npos);
        assert(json.find("capacity") == std::string::npos); // No invented battery percentage.
        assert(json.find("/media/SD") != std::string::npos && json.find("42000") != std::string::npos);
        assert(!platform.brightness(7) && platform.brightness(2));
    }
    std::string restored; std::ifstream(root / "sys/class/backlight/panel/brightness") >> restored;
    assert(restored == "4");
    Platform absent(root / "missing", root / "missing");
    assert(!absent.brightness(1) && absent.json().find("\"power\":[]") != std::string::npos);
}
