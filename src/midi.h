// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <array>
#include <atomic>
#include <cstdint>

struct MidiEvent { uint8_t status = 0, key = 0, value = 0; };

struct MidiPacket {
    uint64_t timestamp_ns = 0; // Monotonic receipt time, set by the hardware adapter.
    std::array<uint8_t, 256> bytes {};
    unsigned size = 0;
};

// Complete messages for bidirectional hardware ports, including clock,
// transport, system common and bounded SysEx. Oversized SysEx is discarded
// whole; realtime bytes never disturb an in-progress message.
class MidiStreamParser {
public:
    template <class Callback> void feed(uint8_t byte, Callback callback) {
        if (byte >= 0xf8) {
            MidiPacket realtime; realtime.bytes[0] = byte; realtime.size = 1;
            callback(realtime); return;
        }
        if (sysex) {
            if (byte == 0xf7) {
                if (!discard && packet.size < packet.bytes.size()) {
                    packet.bytes[packet.size++] = byte; callback(packet);
                } else ++dropped;
                sysex = discard = false; packet.size = 0; return;
            }
            if (!(byte & 0x80)) {
                if (packet.size < packet.bytes.size() - 1) packet.bytes[packet.size++] = byte;
                else discard = true;
                return;
            }
            sysex = discard = false; packet.size = 0;
        }
        if (byte & 0x80) {
            packet.size = 0; running = 0; expected = 0;
            if (byte == 0xf0) {
                sysex = true; packet.bytes[packet.size++] = byte; return;
            }
            if (byte < 0xf0) {
                running = byte;
                expected = ((byte & 0xe0) == 0xc0) ? 2 : 3;
            } else if (byte == 0xf1 || byte == 0xf3) expected = 2;
            else if (byte == 0xf2) expected = 3;
            else if (byte == 0xf6) expected = 1;
            else return;
            packet.bytes[packet.size++] = byte;
        } else {
            if (!packet.size) {
                if (!running) return;
                packet.bytes[packet.size++] = running;
            }
            packet.bytes[packet.size++] = byte;
        }
        if (packet.size == expected) { callback(packet); packet.size = 0; }
    }
    unsigned dropped_sysex() const { return dropped; }
private:
    MidiPacket packet;
    uint8_t running = 0;
    unsigned expected = 0, dropped = 0;
    bool sysex = false, discard = false;
};

// Raw state for every surface message, without assigning undocumented button
// labels or LED opcodes. Owned by the control thread, never the audio thread.
struct SurfaceState {
    std::array<uint8_t, 128> notes {}, pressure {}, controls {};
    uint16_t pitch = 8192;
    uint8_t channel_pressure = 0;
    MidiPacket last;
    void apply(const MidiPacket& packet) {
        last = packet;
        if (packet.size < 2) return;
        unsigned key = packet.bytes[1];
        if (key >= 128) return;
        switch (packet.bytes[0] & 0xf0) {
        case 0x80: notes[key] = 0; break;
        case 0x90: if (packet.size == 3) notes[key] = packet.bytes[2]; break;
        case 0xa0: if (packet.size == 3) pressure[key] = packet.bytes[2]; break;
        case 0xb0: if (packet.size == 3) controls[key] = packet.bytes[2]; break;
        case 0xd0: channel_pressure = uint8_t(key); break;
        case 0xe0: if (packet.size == 3) pitch = uint16_t(key | (packet.bytes[2] << 7)); break;
        default: break;
        }
    }
};

// One control producer and one audio consumer. Overflow requests a panic;
// a lost note-off must never leave a sustained voice sounding.
template <unsigned Size = 256, class Event = MidiEvent> class MidiQueue {
    static_assert(Size >= 2, "A queue needs a spare slot");
public:
    bool push(const Event& event) {
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
    bool pop(Event& event) {
        auto r = read.load(std::memory_order_relaxed);
        if (r == write.load(std::memory_order_acquire)) return false;
        event = events[r];
        read.store((r + 1) % Size, std::memory_order_release);
        return true;
    }
    bool take_overflow() { return overflow.exchange(false, std::memory_order_acq_rel); }
private:
    std::array<Event, Size> events {};
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
