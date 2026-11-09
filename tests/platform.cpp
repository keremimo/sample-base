// SPDX-License-Identifier: AGPL-3.0-only
#include "drivers/platform.h"
#include "surface/midi.h"
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
    // /proc readers: guest time is already in user; a short line still counts.
    auto cpus = parse_proc_stat("cpu  100 5 50 800 20 3 2 0 7 0\ncpu0 60 5 30 400 10 2 1 0 0 0\n"
                                "cpu1 40 0 20 400 10 1 1 0\ncpu2 10 0 10 80\ncpu3 1 2\nintr 1 2 3\n");
    assert(cpus.size() == 4);
    assert(cpus[0].busy == 160 && cpus[0].total == 980 && cpus[1].busy == 98 && cpus[1].total == 508);
    assert(cpus[2].busy == 62 && cpus[2].total == 472 && cpus[3].busy == 20 && cpus[3].total == 100);
    assert(cpu_load({160, 980}, {260, 1180}) == 0.5 && cpu_load(cpus[0], cpus[0]) == 0);
    assert(cpu_load({260, 1180}, {160, 980}) == 0 && cpu_load({0, 100}, {300, 200}) == 1);
    auto memory = parse_meminfo("MemTotal:        2018016 kB\nMemFree:         1586332 kB\nMemAvailable:    1789172 kB\n");
    assert(memory.total_kb == 2018016 && memory.available_kb == 1789172);
    memory = parse_meminfo("MemTotal: 1000 kB\nMemFree: 250 kB\nbroken\n");
    assert(memory.total_kb == 1000 && memory.available_kb == 250);
    memory = parse_meminfo("");
    assert(memory.total_kb == 0 && memory.available_kb == 0);
    assert(json_string("a\"b\\c\n\x01") == "\"a\\\"b\\\\c\\u000a\\u0001\"");
    write(root / "sys/class/power_supply/mains-power/type", "Mains\n");
    write(root / "sys/class/power_supply/mains-power/online", "1\n");
    write(root / "sys/class/power_supply/mains-power/voltage_now", "5000000\n");
    write(root / "sys/class/backlight/panel/max_brightness", "6\n");
    write(root / "sys/class/backlight/panel/brightness", "4\n");
    write(root / "sys/class/udc/controller/state", "configured\n");
    write(root / "sys/class/thermal/thermal_zone0/type", "soc-thermal\n");
    write(root / "sys/class/thermal/thermal_zone0/temp", "42000\n");
    write(root / "sys/class/thermal/thermal_zone1/type", "gpu-thermal\n");
    write(root / "sys/class/thermal/thermal_zone1/temp", "-1500\n");
    write(root / "sys/class/thermal/cooling_device0/type", "fan\n");
    write(root / "proc/self/mountinfo", "31 20 8:1 / /media/SD rw - exfat /dev/sda1 rw\n");
    write(root / "proc/stat", "cpu  100 5 50 800 20 3 2 0 0 0\ncpu0 100 5 50 800 20 3 2 0 0 0\n");
    write(root / "proc/meminfo", "MemTotal: 4096 kB\nMemAvailable: 1024 kB\n");
    {
        Platform platform(root / "sys", root / "proc");
        std::string json = platform.json();
        assert(json.find("5000000") != std::string::npos && json.find("configured") != std::string::npos);
        assert(json.find("capacity") == std::string::npos); // No invented battery percentage.
        assert(json.find("/media/SD") != std::string::npos && json.find("42000") != std::string::npos);
        auto supplies = platform.supplies();
        assert(supplies.size() == 1 && supplies[0].name == "mains-power" && supplies[0].type == "Mains");
        assert(supplies[0].online == 1 && supplies[0].microvolts == 5000000);
        auto zones = platform.temperatures();
        assert(zones.size() == 2 && zones[0].type == "soc-thermal" && zones[0].millidegrees == 42000);
        assert(zones[1].type == "gpu-thermal" && zones[1].millidegrees == -1500);
        auto backlight = platform.backlight();
        assert(backlight.brightness == 4 && backlight.maximum == 6 && platform.usb_state() == "configured");
        auto cpus = platform.cpu_times();
        assert(cpus.size() == 2 && cpus[1].busy == 160 && cpus[1].total == 980);
        assert(platform.memory().total_kb == 4096 && platform.memory().available_kb == 1024);
        assert(!platform.brightness(7) && platform.brightness(2));
        assert(platform.backlight().brightness == 2);
        // Each device once, its bind mounts left out, with the space statvfs reports.
        write(root / "proc/self/mountinfo", "31 20 8:1 / " + root.string() + " rw - ext4 /dev/sda1 rw\n"
              "32 20 8:1 /sub /mnt/bind rw - ext4 /dev/sda1 rw\n33 20 8:2 / /missing ro - ext4 /dev/sdb1 ro\n");
        auto storage = platform.storage();
        assert(storage.size() == 2 && storage[0].path == root.string() && !storage[0].read_only);
        assert(storage[0].total_bytes > 0 && storage[0].free_bytes <= storage[0].total_bytes);
        assert(storage[1].device == "/dev/sdb1" && storage[1].read_only && storage[1].total_bytes == 0);
    }
    std::string restored; std::ifstream(root / "sys/class/backlight/panel/brightness") >> restored;
    assert(restored == "4");
    Platform absent(root / "missing", root / "missing");
    assert(!absent.brightness(1) && absent.json().find("\"power\":[]") != std::string::npos);
    assert(absent.supplies().empty() && absent.temperatures().empty() && absent.storage().empty() && absent.cpu_times().empty());
    assert(absent.backlight().brightness == -1 && absent.backlight().maximum == -1 && absent.usb_state().empty());
    assert(absent.memory().total_kb == 0);
}
