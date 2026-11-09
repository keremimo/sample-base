// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <array>
#include <atomic>
#include <cstdint>

struct MidiEvent { uint8_t status = 0, key = 0, value = 0; };

// One control producer and one audio consumer. Overflow requests a panic;
// a lost note-off must never leave a sustained voice sounding.
template <unsigned Size = 256> class MidiQueue {
public:
    bool push(MidiEvent event) {
        auto w = write.load(std::memory_order_relaxed);
        auto next = (w + 1) % Size;
        if (next == read.load(std::memory_order_acquire)) {
            overflow.store(true, std::memory_order_release);
            return false;
        }
        events[w] = event;
        write.store(next, std::memory_order_release);
        return true;
    }
    bool pop(MidiEvent& event) {
        auto r = read.load(std::memory_order_relaxed);
        if (r == write.load(std::memory_order_acquire)) return false;
        event = events[r];
        read.store((r + 1) % Size, std::memory_order_release);
        return true;
    }
    bool take_overflow() { return overflow.exchange(false, std::memory_order_acq_rel); }
private:
    std::array<MidiEvent, Size> events {};
    std::atomic<unsigned> read {0}, write {0};
    std::atomic<bool> overflow {false};
};

// Handles running status, fragmented reads, realtime interleaving and SysEx.
// SysEx is discarded rather than interpreting its payload as notes/buttons.
class MidiParser {
public:
    template <class Callback> void feed(uint8_t byte, Callback callback) {
        if (byte >= 0xf8) return;
        if (byte & 0x80) {
            count = 0;
            if (byte >= 0xf0) { status = 0; return; }
            status = byte;
            return;
        }
        if (!status) return;
        data[count++] = byte;
        unsigned size = (status & 0xe0) == 0xc0 ? 1 : 2;
        if (count == size) {
            callback(MidiEvent {status, data[0], size == 2 ? data[1] : uint8_t(0)});
            count = 0;
        }
    }
private:
    uint8_t status = 0, data[2] {};
    unsigned count = 0;
};

inline int pad_index(MidiEvent event) {
    unsigned kind = event.status & 0xf0;
    return (kind == 0x80 || kind == 0x90 || kind == 0xa0) &&
           event.key >= 36 && event.key < 52 ? event.key - 36 : -1;
}
