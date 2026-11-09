// SPDX-License-Identifier: AGPL-3.0-only
#include "controls.h"
#include "hardware_midi.h"
#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstdint>
#include <vector>

static int current_card = 0, handles = 0;
static bool usb_present = true, surface_present = true, fail_read = false;
static std::vector<uint8_t> wire;
struct Handle { int card; unsigned reads = 0, writes = 0; };
extern "C" int __wrap_snd_card_next(int* card) {
    for (int next = *card + 1; next < 3; ++next) {
        if ((next == 0 && !surface_present) || (next == 1 && !usb_present)) continue;
        *card = next; return 0;
    }
    *card = -1; return 0;
}
extern "C" int __wrap_snd_ctl_open(snd_ctl_t** ctl, const char* address, int) {
    int card = 0; assert(sscanf(address, "hw:%d", &card) == 1);
    *ctl = reinterpret_cast<snd_ctl_t*>(uintptr_t(card + 1)); return 0;
}
extern "C" int __wrap_snd_ctl_close(snd_ctl_t*) { return 0; }
extern "C" int __wrap_snd_ctl_card_info(snd_ctl_t* ctl, snd_ctl_card_info_t*) { current_card = int(reinterpret_cast<uintptr_t>(ctl)) - 1; return 0; }
extern "C" const char* __wrap_snd_ctl_card_info_get_id(const snd_ctl_card_info_t*) { return current_card == 0 ? "Surface" : current_card == 1 ? "USB" : "External"; }
extern "C" const char* __wrap_snd_ctl_card_info_get_name(const snd_ctl_card_info_t*) { return current_card == 0 ? "Control Surface" : current_card == 1 ? "USB Keyboard" : "External MIDI"; }
extern "C" const char* __wrap_snd_ctl_card_info_get_driver(const snd_ctl_card_info_t*) { return current_card == 1 ? "USB-Audio" : "SerialMIDI"; }
extern "C" int __wrap_snd_ctl_rawmidi_next_device(snd_ctl_t*, int* device) { *device = *device < 0 ? 0 : -1; return 0; }
extern "C" int __wrap_snd_ctl_rawmidi_info(snd_ctl_t*, snd_rawmidi_info_t*) { return 0; }
extern "C" unsigned __wrap_snd_rawmidi_info_get_subdevices_count(const snd_rawmidi_info_t*) { return 1; }
extern "C" const char* __wrap_snd_rawmidi_info_get_subdevice_name(const snd_rawmidi_info_t*) { return ""; }
extern "C" int __wrap_snd_rawmidi_open(snd_rawmidi_t** input, snd_rawmidi_t** output, const char* address, int flags) {
    int card, device, sub; assert(sscanf(address, "hw:%d,%d,%d", &card, &device, &sub) == 3);
    assert(flags & SND_RAWMIDI_NONBLOCK);
    auto* handle = new Handle {card}; ++handles;
    if (input) *input = reinterpret_cast<snd_rawmidi_t*>(handle);
    else { assert(output); *output = reinterpret_cast<snd_rawmidi_t*>(handle); }
    return 0;
}
extern "C" int __wrap_snd_rawmidi_close(snd_rawmidi_t* port) { delete reinterpret_cast<Handle*>(port); --handles; return 0; }
extern "C" int __wrap_snd_rawmidi_drop(snd_rawmidi_t*) { return 0; }
extern "C" ssize_t __wrap_snd_rawmidi_read(snd_rawmidi_t* port, void* bytes, size_t size) {
    auto& handle = *reinterpret_cast<Handle*>(port);
    if (handle.card != 1) return -EAGAIN;
    if (fail_read) return -ENODEV;
    assert(size >= 4);
    auto* data = static_cast<uint8_t*>(bytes);
    if (handle.reads++ == 0) { data[0] = 0xf0; data[1] = 0x47; data[2] = 0xf8; return 3; }
    if (handle.reads == 2) { data[0] = 0xf7; data[1] = 0x90; data[2] = 60; data[3] = 100; return 4; }
    return -EAGAIN;
}
static std::vector<uint8_t> surface_wire;
extern "C" ssize_t __wrap_snd_rawmidi_write(snd_rawmidi_t* port, const void* bytes, size_t size) {
    auto& handle = *reinterpret_cast<Handle*>(port);
    auto* data = static_cast<const uint8_t*>(bytes);
    if (handle.card == 0) { surface_wire.insert(surface_wire.end(), data, data + size); return ssize_t(size); }
    assert(handle.card == 1);
    if (++handle.writes == 2) return -EAGAIN;
    size_t count = handle.writes == 1 ? 1 : size;
    wire.insert(wire.end(), data, data + count); return ssize_t(count);
}
int main() {
    {
        HardwareMidi midi;
        assert(midi.has_surface() && handles == 6);
        std::vector<MidiPacket> received;
        midi.read([&](const MidiPortInfo& port, const MidiPacket& packet) {
            assert(!port.surface && port.card == "USB"); received.push_back(packet);
        });
        assert(received.size() == 3);
        assert(received[0].timestamp_ns > 0 && received[2].timestamp_ns >= received[0].timestamp_ns);
        assert(received.size() == 3 && received[0].bytes[0] == 0xf8 && received[1].size == 3 && received[2].bytes[0] == 0x90);
        MidiPacket note; note.size = 3; note.bytes[0] = 0x90; note.bytes[1] = 60; note.bytes[2] = 100;
        assert(!midi.send("absent", note));
        assert(midi.send("hw:1,0,0", note));
        midi.flush(); midi.flush(); midi.flush();
        assert(wire == std::vector<uint8_t>({0x90,60,100}));
        for (unsigned n = 0; n < 7; ++n) assert(midi.send("hw:1,0,0", note));
        assert(!midi.send("hw:1,0,0", note));
        for (unsigned n = 0; n < 7; ++n) midi.flush();
        assert(wire.size() == 24 && midi.json().find("\"send_drops\":1") != std::string::npos);
        assert(midi.send_surface(surface_position_packet()));
        midi.flush();
        assert(surface_wire == std::vector<uint8_t>({0xf0,0x47,0x7f,0x5c,0x42,0x00,0x00,0xf7}) && wire.size() == 24);
        usb_present = false;
        assert(midi.rescan() && handles == 4 && midi.has_surface());
        usb_present = true;
        assert(!midi.rescan() && handles == 6);
        fail_read = true; received.clear();
        midi.read([&](const MidiPortInfo&, const MidiPacket& packet) { received.push_back(packet); });
        assert(received.size() == 1 && received[0].bytes[0] == 0xb0 && received[0].bytes[1] == 120);
        assert(handles == 4 && midi.json().find("\"input_errors\":1") != std::string::npos);
        fail_read = false; midi.rescan(); assert(handles == 6);
        surface_present = false;
        assert(midi.rescan() && !midi.has_surface() && !midi.send_surface(surface_position_packet()));
    }
    assert(handles == 0);
}
