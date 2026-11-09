// SPDX-License-Identifier: AGPL-3.0-only
#include "demos/pad_touch/pad_touch_demo.h"
#include <algorithm>

void record_pad_touch(PadTouches& pads, const SurfaceEvent& event) {
    if (event.pad < 0) return;
    auto pad = unsigned(event.pad);
    unsigned kind = event.midi.status & 0xf0;
    if (kind == 0xa0) pads.press(pad, event.midi.value);
    else if (kind == 0x90 && event.midi.value != 0) pads.strike(pad, event.midi.value);
    else pads.release(pad);
}

// Each pad shows how hard it is played and fades out over 300 ms after
// release; 16 Levels stays lit while the page is open.
void PadTouchLights::update(SurfaceLights& lights, const PadTouches& pads, uint32_t held, Clock::time_point now) {
    using namespace std::chrono;
    if (!showing) {
        showing = true; next_frame = now;
        for (unsigned pad = 0; pad < surface_pad_lights; ++pad) strikes_seen[pad] = pads.touch(pad).strikes;
        last_held.fill({}); force.fill(0);
        lights.button(SurfaceButton::SixteenLevels, ButtonLight::Bright);
    }
    if (now < next_frame) return;
    next_frame = now + milliseconds(20);
    float hardest = 0;
    for (unsigned pad = 0; pad < surface_pad_lights; ++pad) {
        auto touch = pads.touch(pad);
        bool down = (held & (1u << pad)) != 0;
        if (touch.strikes != strikes_seen[pad]) {
            strikes_seen[pad] = touch.strikes; struck[pad] = now;
            if (!down) { force[pad] = pad_touch::held_force(touch.velocity, 0, 0); last_held[pad] = now; }
        }
        if (down) {
            auto since = unsigned(duration_cast<milliseconds>(now - struck[pad]).count());
            force[pad] = pad_touch::held_force(touch.velocity, touch.pressure, since);
            last_held[pad] = now; hardest = std::max(hardest, force[pad]);
        }
        float fade = 1 - duration<float, std::milli>(now - last_held[pad]).count() / pad_touch::fade_ms;
        lights.pad(pad, fade > 0 ? pad_touch::force_colour(force[pad], fade) : Rgb {});
    }
    lights.fader(hardest > 0 ? pad_touch::force_colour(hardest) : Rgb {});
    unsigned banks = pad_touch::force_banks(hardest);
    for (unsigned bank = 0; bank < surface_bank_lights; ++bank) lights.bank(bank, bank < banks);
}

void PadTouchLights::close(SurfaceLights& lights) {
    if (!showing) return;
    showing = false;
    for (unsigned pad = 0; pad < surface_pad_lights; ++pad) lights.pad(pad, {});
    lights.fader({});
    for (unsigned bank = 0; bank < surface_bank_lights; ++bank) lights.bank(bank, false);
    lights.button(SurfaceButton::SixteenLevels, ButtonLight::Off);
}

void PadTrace::advance(PadTouches& pads, int velocity) {
    std::rotate(trace.begin(), trace.begin() + 1, trace.end());
    std::rotate(strike_marks.begin(), strike_marks.begin() + 1, strike_marks.end());
    unsigned strikes = pads.strikes();
    trace.back() = pads.take_recent();
    strike_marks.back() = strikes != strikes_seen ? uint8_t(velocity) : 0;
    strikes_seen = strikes;
}

PadsView PadTrace::view(const PadTouches& pads) const {
    PadsView view;
    for (unsigned pad = 0; pad < PadTouches::pads; ++pad) {
        auto touch = pads.touch(pad);
        view.velocity[pad] = touch.velocity; view.pressure[pad] = touch.pressure; view.peak[pad] = touch.peak;
    }
    view.trace = trace; view.strikes = strike_marks;
    return view;
}
