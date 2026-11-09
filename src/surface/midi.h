// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include "surface/pads.h"
#include <array>
#include <atomic>
#include <cstdint>
#include <sstream>
#include <string>

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

// Lossless recent history of surface packets for diagnostics and calibration.
// Owned by the control thread. Sequence numbers start at 1; a reader that
// falls more than Size entries behind sees the gap between its cursor and
// the first sequence returned. Packets longer than 16 bytes keep their real
// size but only their first 16 bytes.
template <unsigned Size = 1024> class SurfaceLog {
    static_assert(Size >= 1, "A log needs an entry");
public:
    struct Entry {
        uint64_t sequence = 0, timestamp_ns = 0;
        unsigned size = 0;
        std::array<uint8_t, 16> bytes {};
    };
    void push(const MidiPacket& packet) {
        auto& entry = entries[latest % Size];
        entry.sequence = ++latest;
        entry.timestamp_ns = packet.timestamp_ns;
        entry.size = packet.size;
        for (unsigned n = 0; n < entry.bytes.size(); ++n) entry.bytes[n] = n < packet.size ? packet.bytes[n] : 0;
    }
    uint64_t newest() const { return latest; }
    uint64_t oldest() const { return latest > Size ? latest - Size + 1 : 1; }
    // Visits at most `limit` entries newer than `after`, oldest first, and
    // returns the cursor for the next read.
    template <class Visit> uint64_t read(uint64_t after, unsigned limit, Visit visit) const {
        uint64_t sequence = after + 1 > oldest() ? after + 1 : oldest();
        for (unsigned n = 0; n < limit && sequence <= latest; ++n, ++sequence) visit(entries[(sequence - 1) % Size]);
        return sequence - 1 > after ? sequence - 1 : after;
    }
private:
    std::array<Entry, Size> entries {};
    uint64_t latest = 0;
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

// One complete MIDI message written as hex bytes ("90 3c 64"); false for
// anything else, including two messages or a partial one.
inline bool parse_midi_hex(const std::string& text, MidiPacket& packet) {
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

inline int pad_index(MidiEvent event) {
    unsigned kind = event.status & 0xf0;
    if ((event.status & 0x0f) != 9 || (kind != 0x80 && kind != 0x90 && kind != 0xa0)) return -1;
    return surface_pad_index(event.key);
}
