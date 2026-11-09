// SPDX-License-Identifier: AGPL-3.0-only
#include "midi.h"
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
    const uint8_t pitches[] = {60,62,64,65,67,69,71,72,74,76,77,79,81,83,84,86};
    for (int pad = 0; pad < 16; ++pad) {
        assert(pad_index({0x99,measured[pad],100}) == pad);
        assert(pad_index({0x89,measured[pad],0}) == pad);
        assert(pad_index({0x99,measured[pad],0}) == pad);
        assert(pad_index({0xa9,measured[pad],64}) == pad);
        assert(pad_scale_notes[pad] == pitches[pad]);
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
}
