// SPDX-License-Identifier: AGPL-3.0-only
#include "app/synth.h"
#include <cassert>
#include <cmath>

using Buffer = std::array<int32_t, Synth::block_size * 2>;

static double energy(Synth& synth, unsigned blocks) {
    Buffer output {};
    double sum = 0;
    for (unsigned b = 0; b < blocks; ++b) {
        float peak = synth.render(output.data());
        assert(std::isfinite(peak) && peak >= 0 && peak <= 0.64f);
        for (unsigned i = 0; i < Synth::block_size; ++i) {
            assert(output[2 * i] == output[2 * i + 1]);
            double sample = output[2 * i] / 2147483648.0;
            sum += sample * sample;
        }
    }
    return sum;
}

int main() {
    Synth synth;
    assert(energy(synth, 4) == 0);
    synth.midi({0x90, 69, 127});
    assert(energy(synth, 10) > 0.1);

    // A4 makes 440 cycles per second, independently of block boundaries.
    Buffer output {};
    unsigned crossings = 0;
    int32_t previous = 0;
    for (unsigned b = 0; b < 345; ++b) {
        synth.render(output.data());
        for (unsigned i = 0; i < Synth::block_size; ++i) {
            int32_t sample = output[2 * i];
            if (previous < 0 && sample >= 0) ++crossings;
            previous = sample;
        }
    }
    assert(crossings >= 439 && crossings <= 442);
    synth.midi({0x80, 69, 0});
    energy(synth, 32); // More than the 80 ms release.
    assert(energy(synth, 4) == 0);

    // Velocity scales amplitude; a velocity-zero note-on releases the note.
    Synth soft, loud;
    soft.midi({0x90, 60, 64}); loud.midi({0x90, 60, 127});
    double ratio = energy(soft, 20) / energy(loud, 20);
    assert(ratio > 0.25 && ratio < 0.26);
    soft.midi({0x90, 60, 0}); energy(soft, 32);
    assert(energy(soft, 4) == 0);

    // Repeated hits retrigger one voice rather than leaking held voices.
    for (unsigned i = 0; i < 40; ++i) {
        synth.midi({0x90, 60, 100}); energy(synth, 1);
    }
    synth.midi({0x80, 60, 0}); energy(synth, 32);
    assert(energy(synth, 4) == 0);

    // A seventeenth note steals a voice. Releasing the old key must not
    // release its replacement; releasing every current key must silence it.
    for (uint8_t note = 60; note <= 76; ++note) synth.midi({0x90, note, 127});
    assert(energy(synth, 8) > 0.1);
    for (uint8_t note = 60; note < 76; ++note) synth.midi({0x80, note, 0});
    energy(synth, 32);
    assert(energy(synth, 4) > 0.01);
    synth.midi({0x80, 76, 0}); energy(synth, 32);
    assert(energy(synth, 4) == 0);

    // Panic silences even stolen/high-pitch voices immediately. All-notes-off
    // uses the normal release envelope instead.
    for (unsigned note = 0; note < 128; ++note) synth.midi({0x90, uint8_t(note), 127});
    assert(energy(synth, 8) > 0.1);
    synth.midi({0xb0, 120, 0});
    assert(energy(synth, 4) == 0);
    synth.midi({0x90, 60, 100}); energy(synth, 4);
    synth.midi({0xb0, 123, 0}); energy(synth, 32);
    assert(energy(synth, 4) == 0);

    synth.midi({0x90, 255, 127}); synth.midi({0x90, 60, 255});
    synth.midi({0xb0, 60, 127});
    assert(energy(synth, 4) == 0);
}
