// SPDX-License-Identifier: AGPL-3.0-only
#include "surface/controls.h"
#include "surface/midi.h"
#include <cassert>
#include <vector>

int main() {
    MidiParser parser;
    std::vector<MidiEvent> got;
    auto feed = [&](std::initializer_list<uint8_t> bytes) {
        for (auto b : bytes) parser.feed(b, [&](MidiEvent event) { got.push_back(event); });
    };
    feed({0x99,36});
    assert(got.empty());
    feed({0xf8,100,37,0,0x89,36,0});
    assert(got.size() == 3 && got[0].status == 0x99 && got[0].value == 100);
    assert(got[1].key == 37 && got[1].value == 0 && got[2].status == 0x89);
    feed({0xf0,0x47,36,127,0xf8,0xf7,36,100});
    assert(got.size() == 3); // SysEx and stray data cannot become pad hits.
    feed({0xc0,4,5,0xd0,72,0xb0,111,127});
    assert(got.size() == 7 && got[3].key == 4 && got[4].key == 5 && got[5].status == 0xd0);
    assert(got[6].key == 111);
    // Measured physical pad order, independent of ascending MIDI note order.
    const uint8_t measured[] = {37,36,42,82,40,38,46,44,48,47,45,43,49,55,51,53};
    for (int pad = 0; pad < 16; ++pad) {
        assert(pad_index({0x99,measured[pad],100}) == pad);
        assert(pad_index({0x89,measured[pad],0}) == pad);
        assert(pad_index({0x99,measured[pad],0}) == pad);
        assert(pad_index({0xa9,measured[pad],64}) == pad);
    }
    assert(pad_index({0x99,39,100}) == -1 && pad_index({0x99,50,100}) == -1);
    assert(pad_index({0x90,37,100}) == -1); // Another channel is not a surface pad.
    assert(pad_index({0xb0,36,127}) == -1 && pad_index({0x90,111,127}) == -1);
    MidiQueue<4> queue;
    assert(queue.push({0x90,60,100}) && queue.push({0x90,64,100}) && queue.push({0x80,60,0}));
    assert(!queue.push({0x80,64,0}) && queue.take_overflow() && !queue.take_overflow());
    MidiEvent event;
    assert(queue.pop(event) && event.key == 60);
    assert(queue.push({0x80,64,0})); // wrap-around retains FIFO order.
    assert(queue.pop(event) && event.key == 64 && event.status == 0x90);
    assert(queue.pop(event) && event.key == 60 && event.status == 0x80);
    assert(queue.pop(event) && event.key == 64 && event.status == 0x80);
    assert(!queue.pop(event));

    // Calibration reads every surface packet in order and detects lost history.
    SurfaceLog<4> log;
    std::vector<uint64_t> seen;
    auto collect = [&](const SurfaceLog<4>::Entry& entry) { seen.push_back(entry.sequence); };
    assert(log.newest() == 0 && log.read(0, 8, collect) == 0 && seen.empty());
    for (uint8_t value = 1; value <= 3; ++value) {
        MidiPacket packet; packet.timestamp_ns = value * 10u; packet.size = 3;
        packet.bytes = {0xb0, 16, value};
        log.push(packet);
    }
    assert(log.read(0, 2, collect) == 2 && seen == std::vector<uint64_t>({1, 2}));
    assert(log.read(2, 8, [&](const SurfaceLog<4>::Entry& entry) {
        assert(entry.sequence == 3 && entry.timestamp_ns == 30 && entry.size == 3 && entry.bytes[2] == 3);
    }) == 3);
    MidiPacket sysex; sysex.size = 20;
    for (unsigned n = 0; n < sysex.size; ++n) sysex.bytes[n] = uint8_t(n);
    for (int n = 0; n < 3; ++n) log.push(sysex);
    assert(log.newest() == 6 && log.oldest() == 3);
    seen.clear();
    assert(log.read(1, 8, collect) == 6 && seen == std::vector<uint64_t>({3, 4, 5, 6}));
    log.read(5, 1, [&](const SurfaceLog<4>::Entry& entry) {
        assert(entry.size == 20 && entry.bytes[15] == 15);
    });
    assert(log.read(9, 8, collect) == 9); // A stale cursor waits rather than rewinding.

    // Measured channel-1 button notes, in SurfaceButton order.
    const uint8_t buttons[] = {6,7,1,0,49,35,9,11, 12,13,14, 75,43,74,40,36,53,55,54, 73,80,81,82,111};
    static_assert(sizeof(buttons) == size_t(SurfaceButton::Count), "every button is measured");
    for (unsigned n = 0; n < surface_buttons.size(); ++n) {
        assert(surface_buttons[n].number == buttons[n]);
        for (unsigned other = n + 1; other < surface_buttons.size(); ++other) assert(buttons[other] != buttons[n]);
        auto down = surface_input({0x90, buttons[n], 127}), up = surface_input({0x80, buttons[n], 0});
        assert(is_button(down, SurfaceButton(n)) && down.pressed && is_button(up, SurfaceButton(n)) && !up.pressed);
        assert(!surface_input({0x90, buttons[n], 0}).pressed);              // velocity-zero release
        assert(surface_input({0x99, buttons[n], 127}).kind == SurfaceInput::Kind::None); // channel 10 is pads
        assert(describe_last_input(pack_last_input(down)) == surface_buttons[n].name);
    }
    // Shared numbers stay separate: channel 1 is a button, channel 10 a pad.
    assert(is_button(surface_input({0x90,36,127}), SurfaceButton::SampleSelect) && pad_index({0x90,36,127}) == -1);
    assert(pad_index({0x99,82,127}) == 3 && surface_input({0x99,82,127}).kind == SurfaceInput::Kind::None);
    assert(surface_input({0xb0,111,127}).kind == SurfaceInput::Kind::None); // Push is a note, not CC 111.

    const uint8_t controls[] = {19,16,17,18,20,100};
    for (unsigned n = 0; n < surface_controls.size(); ++n) assert(surface_controls[n].number == controls[n]);
    auto k2 = surface_input({0xb0,17,64});
    assert(is_control(k2, SurfaceControl::K2) && k2.value == 64 && k2.delta == 0);
    assert(describe_last_input(pack_last_input(k2)) == "K2 64");
    assert(is_control(surface_input({0xb0,20,127}), SurfaceControl::Fader));
    assert(surface_input({0xb1,17,64}).kind == SurfaceInput::Kind::None);
    auto right = surface_input({0xb0,100,1}), left = surface_input({0xb0,100,127}), fast = surface_input({0xb0,100,125});
    assert(is_control(right, SurfaceControl::Wheel) && right.delta == 1 && left.delta == -1 && fast.delta == -3);
    assert(describe_last_input(pack_last_input(left)) == "Wheel -1" && describe_last_input(pack_last_input(right)) == "Wheel +1");
    assert(describe_last_input(pack_last_input(InputKind::Power, 0, 1)) == "Power" && describe_last_input(0).empty());
    auto request = surface_position_packet();
    assert(request.size == 8 && request.bytes[0] == 0xf0 && request.bytes[3] == 0x5c && request.bytes[4] == 0x42 && request.bytes[7] == 0xf7);
    // Measured reply: current positions, then a wheel message with no motion.
    assert(is_control(surface_input({0xb0,19,22}), SurfaceControl::MainVolume) && surface_input({0xb0,100,0}).delta == 0);
    assert(main_volume_gain(0) == 0.0f && main_volume_gain(127) == 1.0f && main_volume_gain(64) < 0.26f);
}
