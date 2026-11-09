// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "surface/midi.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

// A small, fixed-size synth for exercising pads, audio and the display.
// Only the audio thread owns it; MIDI and rendering never allocate or do I/O.
class Synth {
public:
    static constexpr unsigned sample_rate = 44100, block_size = 128, polyphony = 16;

    void midi(MidiEvent event) {
        unsigned kind = event.status & 0xf0;
        if (kind == 0xb0 && event.key == 120) {
            for (auto& voice : voices) voice = Voice {};
        } else if (kind == 0xb0 && event.key == 123) {
            for (auto& voice : voices) voice.held = false;
        } else if (kind == 0x80 || (kind == 0x90 && !event.value)) {
            for (auto& voice : voices) if (voice.key == event.key) voice.held = false;
        } else if (kind == 0x90 && event.key < 128 && event.value < 128) {
            Voice* selected = nullptr;
            for (auto& voice : voices) if (voice.key == event.key) { selected = &voice; break; }
            if (!selected) {
                for (auto& voice : voices) if (voice.key < 0) { selected = &voice; break; }
            }
            if (!selected) { selected = &voices[next]; next = (next + 1) % polyphony; }
            *selected = Voice {};
            selected->key = event.key;
            selected->held = true;
            selected->gain = event.value / 127.0f;
            selected->step = 440.0 * std::exp2((int(event.key) - 69) / 12.0) / sample_rate;
        }
    }

    float render(int32_t* output) {
        constexpr double tau = 6.2831853071795864769;
        float peak = 0;
        for (unsigned frame = 0; frame < block_size; ++frame) {
            double sample = 0;
            for (auto& voice : voices) {
                if (voice.key < 0) continue;
                if (voice.held) voice.level = std::min(1.0f, voice.level + 1.0f / (0.005f * sample_rate));
                else voice.level = std::max(0.0f, voice.level - 1.0f / (0.080f * sample_rate));
                if (!voice.held && voice.level == 0) { voice = Voice {}; continue; }
                double triangle = 1.0 - 4.0 * std::abs(voice.phase - 0.5);
                sample += (0.7 * std::sin(tau * voice.phase) + 0.3 * triangle) * voice.level * voice.gain * 0.04;
                voice.phase += voice.step;
                if (voice.phase >= 1.0) voice.phase -= 1.0;
            }
            sample = std::clamp(sample, -0.9, 0.9);
            peak = std::max(peak, float(std::abs(sample)));
            output[2 * frame] = output[2 * frame + 1] = static_cast<int32_t>(sample * 2147483647.0);
        }
        return peak;
    }

private:
    struct Voice {
        int key = -1;
        bool held = false;
        double phase = 0, step = 0;
        float level = 0, gain = 0;
    };
    std::array<Voice, polyphony> voices {};
    unsigned next = 0;
};
