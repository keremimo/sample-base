// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include "surface/led_colours.h"
#include "surface/leds.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>

// Pad velocity and pressure showcase. A pad sends a channel-10 note-on whose
// velocity is how hard it was struck, then polyphonic pressure (aftertouch)
// on the same note while it is held: 0 straight after the strike, a new value
// whenever the pressure changes, and 0 again just before the note-off.

// One pad's latest touch. `strikes` counts note-ons, so a reader can tell a
// new strike from an old one even when the pad was already let go.
struct PadTouch { uint8_t velocity = 0, pressure = 0, peak = 0; uint16_t strikes = 0; };

// Latest touch of every pad. The MIDI thread writes; any thread reads, and
// only the screen thread calls take_recent().
class PadTouches {
public:
    static constexpr unsigned pads = 16;
    // Note-on: a new touch, so the pressure and its peak start from 0.
    void strike(unsigned pad, uint8_t velocity) {
        if (pad >= pads) return;
        auto& slot = slots[pad];
        slot.velocity.store(velocity, std::memory_order_relaxed);
        slot.pressure.store(0, std::memory_order_relaxed); slot.peak.store(0, std::memory_order_relaxed);
        slot.strikes.fetch_add(1, std::memory_order_relaxed); total.fetch_add(1, std::memory_order_relaxed);
    }
    // Polyphonic pressure while the pad is held.
    void press(unsigned pad, uint8_t pressure) {
        if (pad >= pads) return;
        auto& slot = slots[pad];
        slot.pressure.store(pressure, std::memory_order_relaxed);
        if (pressure > slot.peak.load(std::memory_order_relaxed)) slot.peak.store(pressure, std::memory_order_relaxed);
        auto seen = recent.load(std::memory_order_relaxed);
        while (seen < pressure && !recent.compare_exchange_weak(seen, pressure, std::memory_order_relaxed)) {}
    }
    // Note-off: the pressure is gone; the strike and its peak stay on show.
    void release(unsigned pad) {
        if (pad < pads) slots[pad].pressure.store(0, std::memory_order_relaxed);
    }
    PadTouch touch(unsigned pad) const {
        if (pad >= pads) return {};
        const auto& slot = slots[pad];
        return {slot.velocity.load(std::memory_order_relaxed), slot.pressure.load(std::memory_order_relaxed),
                slot.peak.load(std::memory_order_relaxed), slot.strikes.load(std::memory_order_relaxed)};
    }
    unsigned strikes() const { return total.load(std::memory_order_relaxed); }
    // The highest pressure on any pad since the last call, or the highest
    // held now if that is more, so a press shorter than a screen frame still
    // shows. Screen thread only.
    uint8_t take_recent() {
        uint8_t highest = recent.exchange(0, std::memory_order_relaxed);
        for (const auto& slot : slots) highest = std::max(highest, slot.pressure.load(std::memory_order_relaxed));
        return highest;
    }
private:
    struct Slot {
        std::atomic<uint8_t> velocity {0}, pressure {0}, peak {0};
        std::atomic<uint16_t> strikes {0};
    };
    std::array<Slot, pads> slots;
    std::atomic<unsigned> total {0};
    std::atomic<uint8_t> recent {0};
};

// How hard a pad is played, as the pad lights and the screen show it.
namespace pad_touch {
constexpr unsigned flash_ms = 250, fade_ms = 300;
constexpr float rest = 0.35f; // Share of the strike a held pad keeps once the flash is over.

// Force 0..1 of a held pad `since_strike_ms` after its strike: the strike
// flashes at its velocity and settles over flash_ms to `rest` of it, so a
// held pad stays lit; pressure above that takes over.
inline float held_force(uint8_t velocity, uint8_t pressure, unsigned since_strike_ms) {
    float flash = since_strike_ms < flash_ms ? 1 - float(since_strike_ms) / flash_ms : 0;
    return std::max(velocity / 127.0f * (rest + (1 - rest) * flash), pressure / 127.0f);
}
// Blue when soft, through green and yellow to red when hard, and brighter
// the harder. `fade` 1..0 dims it after release.
inline Rgb force_colour(float force, float fade = 1) {
    force = led_colour::clamp01(force);
    return led_colour::hue(0.62f * (1 - force), (0.3f + 0.7f * force) * led_colour::clamp01(fade));
}
// Bank lights A-H as a meter: how many a force lights. Any force lights A.
inline unsigned force_banks(float force) {
    return unsigned(std::ceil(led_colour::clamp01(force) * surface_bank_lights - 0.001f));
}
} // namespace pad_touch
