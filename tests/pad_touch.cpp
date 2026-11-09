// SPDX-License-Identifier: AGPL-3.0-only
#include "app/pad_touch.h"
#include <cassert>
#include <cmath>

static int brightness(Rgb colour) { return colour.r + colour.g + colour.b; }
static bool near(float a, float b) { return std::fabs(a - b) < 1e-5f; }

int main() {
    using namespace pad_touch;

    // A strike starts a touch with no pressure; pressure raises the peak.
    PadTouches pads;
    pads.strike(3, 100);
    auto touch = pads.touch(3);
    assert(touch.velocity == 100 && touch.pressure == 0 && touch.peak == 0 && touch.strikes == 1 && pads.strikes() == 1);
    pads.press(3, 40); pads.press(3, 90); pads.press(3, 20);
    touch = pads.touch(3);
    assert(touch.pressure == 20 && touch.peak == 90);
    // The screen trace sees the highest pressure since its last read, then what is held now.
    assert(pads.take_recent() == 90 && pads.take_recent() == 20);
    // Release drops the pressure but keeps the strike and its peak on show.
    pads.release(3);
    touch = pads.touch(3);
    assert(touch.pressure == 0 && touch.velocity == 100 && touch.peak == 90 && pads.take_recent() == 0);
    // A new strike starts the peak again; other pads keep their own touch.
    pads.strike(12, 64); pads.press(12, 30);
    pads.strike(3, 30);
    assert(pads.touch(3).peak == 0 && pads.touch(3).strikes == 2 && pads.strikes() == 3);
    assert(pads.touch(12).pressure == 30 && pads.take_recent() == 30);
    // Pads outside 1-16 are ignored.
    pads.strike(16, 127); pads.press(16, 127); pads.release(99);
    assert(pads.strikes() == 3 && pads.take_recent() == 30 && pads.touch(16).strikes == 0);

    // Force: a strike flashes at its velocity and settles to `rest` of it;
    // pressure above that takes over.
    assert(near(held_force(127, 0, 0), 1) && near(held_force(127, 0, flash_ms), rest));
    assert(near(held_force(127, 0, 10 * flash_ms), rest) && near(held_force(0, 0, 0), 0));
    assert(near(held_force(64, 127, flash_ms), 1) && near(held_force(127, 20, flash_ms), rest));
    for (unsigned ms = 0; ms < flash_ms; ms += 5) assert(held_force(100, 0, ms) >= held_force(100, 0, ms + 5));

    // Colour: values the pads accept, blue when soft and red when hard, never
    // less red as force grows, and dark once faded.
    auto soft = force_colour(0), hard = force_colour(1);
    assert(soft.b > soft.r && soft.b > soft.g && hard.r == 127 && !hard.g && !hard.b);
    int red = -1;
    for (unsigned n = 0; n <= 100; ++n) {
        auto colour = force_colour(n / 100.0f);
        assert(colour.r <= 127 && colour.g <= 127 && colour.b <= 127 && brightness(colour) > 0);
        assert(colour.r >= red); red = colour.r;
    }
    assert(brightness(force_colour(0.5f)) < brightness(hard));
    for (unsigned n = 10; n > 0; --n) assert(brightness(force_colour(0.6f, n / 10.0f)) >= brightness(force_colour(0.6f, (n - 1) / 10.0f)));
    assert(!brightness(force_colour(1, 0)) && !brightness(force_colour(1, -1)));
    assert(brightness(force_colour(2)) == brightness(hard));

    // Bank lights A-H meter the force; any force lights A.
    assert(force_banks(0) == 0 && force_banks(0.01f) == 1 && force_banks(0.5f) == 4);
    assert(force_banks(1) == 8 && force_banks(2) == 8 && force_banks(-1) == 0);
    return 0;
}
