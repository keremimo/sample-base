// SPDX-License-Identifier: AGPL-3.0-only
#include "demos/sequencer/sequencer.h"
#include <cassert>
#include <cmath>
#include <vector>

using namespace seq;

namespace {
// Renders `frames` frames in 128-frame blocks, as the audio thread does, and
// returns the peak of either channel.
float run(Engine& engine, Shared& shared, unsigned frames, std::vector<float>* keep = nullptr) {
    float peak = 0;
    std::vector<float> block(256);
    for (unsigned done = 0; done < frames; done += 128) {
        unsigned count = std::min(128u, frames - done);
        std::fill(block.begin(), block.end(), 0.0f);
        engine.render(shared, block.data(), count);
        for (unsigned n = 0; n < count * 2; ++n) {
            assert(std::isfinite(block[n]));
            peak = std::max(peak, std::fabs(block[n]));
            if (keep) keep->push_back(block[n]);
        }
    }
    return peak;
}
unsigned hits(const Shared& shared, Drum drum) { return shared.triggers[unsigned(drum)].load(); }
} // namespace

int main() {
    // The default groove: four on the floor, claps on 2 and 4, hats between.
    {
        Pattern pattern;
        for (unsigned step = 0; step < steps; ++step)
            assert(bool(pattern.get(unsigned(Drum::Kick), step)) == (step % 4 == 0));
        assert(pattern.get(unsigned(Drum::Clap), 4) && pattern.get(unsigned(Drum::Clap), 12) && pattern.get(unsigned(Drum::OpenHat), 2));
        assert(pattern.empty(unsigned(Drum::Snare)) && pattern.empty(unsigned(Drum::Bell)) && !pattern.muted(0));
        // A tap sets an empty step at its velocity and clears a set one.
        assert(pattern.toggle(1, 3, 90) && pattern.get(1, 3) == 90);
        assert(!pattern.toggle(1, 3, 127) && pattern.get(1, 3) == 0);
        assert(!pattern.toggle(1, 3, 0) && pattern.get(1, 3) == 0);
        pattern.set(1, 15, 200); assert(pattern.get(1, 15) == 127);
        pattern.set(8, 0, 100); pattern.set(0, 16, 100); assert(!pattern.get(8, 0) && !pattern.get(0, 16));
        pattern.toggle_mute(2); assert(pattern.muted(2));
        pattern.clear(0); assert(pattern.empty(0) && !pattern.empty(3));
        pattern.clear_all();
        for (unsigned track = 0; track < tracks; ++track) assert(pattern.empty(track) && !pattern.muted(track));
    }

    // Tap Tempo averages up to four intervals and starts again after a pause.
    {
        TapTempo taps;
        assert(!taps.tap(1000));
        assert(*taps.tap(1500) == 1200);
        assert(*taps.tap(2000) == 1200 && *taps.tap(2520) == 1184);
        for (uint64_t ms = 3000; ms < 6000; ms += 600) taps.tap(ms);
        assert(*taps.tap(6000) == 1000); // The last four intervals only.
        assert(!taps.tap(9000) && *taps.tap(9400) == 1500);
        assert(!taps.tap(100) && *taps.tap(150) == max_tempo); // Time going back starts again.
        TapTempo same, slow;
        assert(!same.tap(500) && !same.tap(500)); // Two taps in the same millisecond.
        assert(!slow.tap(0) && *slow.tap(2000) == min_tempo && clamp_tempo(300) == min_tempo);
    }

    // Timing: a 16th note at 120 BPM is 5512.5 frames; positions name the
    // step and how far into it; a live hit goes to the nearest step.
    assert(frames_per_step(1200) == 5512.5 && frames_per_step(0) == frames_per_step(min_tempo));
    assert(position_step(0) == -1 && nearest_step(0) == -1);
    assert(position_step(position_of(0, 0)) == 0 && position_step(position_of(15, 255)) == 15);
    assert(nearest_step(position_of(4, 127)) == 4 && nearest_step(position_of(4, 128)) == 5);
    assert(nearest_step(position_of(15, 200)) == 0);

    // The clock: steps land on the frame they are due, and the published
    // position follows. At 120 BPM, step 4 starts at frame 22050.
    {
        Shared shared; Engine engine;
        shared.set_tempo(1200);
        run(engine, shared, 22016);
        assert(!shared.playing() && hits(shared, Drum::Kick) == 0);
        shared.play();
        run(engine, shared, 128);
        assert(shared.playing() && position_step(shared.position.load()) == 0 && hits(shared, Drum::Kick) == 1);
        run(engine, shared, 22016 - 128); // Frames 0-22015: steps 0-3.
        assert(position_step(shared.position.load()) == 3 && hits(shared, Drum::Kick) == 1);
        assert(hits(shared, Drum::ClosedHat) == 2 && hits(shared, Drum::OpenHat) == 1 && hits(shared, Drum::Clap) == 0);
        run(engine, shared, 128);         // Frames 22016-22143 hold frame 22050.
        assert(position_step(shared.position.load()) == 4 && hits(shared, Drum::Kick) == 2 && hits(shared, Drum::Clap) == 1);
        // Play again while playing changes nothing; a full bar later the
        // pattern has played exactly once more.
        shared.play();
        run(engine, shared, 88192);
        assert(position_step(shared.position.load()) == 4 && hits(shared, Drum::Kick) == 6 && hits(shared, Drum::Clap) == 3);
        // Muted tracks stay silent; the others carry on.
        shared.pattern.mute(unsigned(Drum::Kick), true);
        run(engine, shared, 88192);
        assert(hits(shared, Drum::Kick) == 6 && hits(shared, Drum::Clap) == 5);
        shared.pattern.mute(unsigned(Drum::Kick), false);
        // A faster tempo takes effect from the next step: step 37 starts at
        // frame 203963, then one every 2756.25 frames, so 15 steps by 242627.
        shared.set_tempo(2400);
        run(engine, shared, 44100);
        assert(hits(shared, Drum::Kick) == 6 + 3 && hits(shared, Drum::Clap) == 5 + 1);
        // Stop: the position clears and nothing more plays; voices ring out.
        shared.stop();
        run(engine, shared, 128);
        auto kicks = hits(shared, Drum::Kick);
        assert(!shared.playing() && shared.position.load() == 0);
        run(engine, shared, 3 * sample_rate);
        assert(hits(shared, Drum::Kick) == kicks);
        for (unsigned track = 0; track < tracks; ++track) assert(!engine.sounding(track));
        // Play starts again from step 1.
        shared.play();
        run(engine, shared, 128);
        assert(position_step(shared.position.load()) == 0 && hits(shared, Drum::Kick) == kicks + 1);
    }

    // A hit recorded onto the next step has already sounded live, so that
    // step skips it once, then plays it on every later bar.
    {
        Shared shared; Engine engine;
        shared.pattern.clear_all(); shared.set_tempo(1200);
        shared.play();
        run(engine, shared, 128);
        shared.pattern.set(unsigned(Drum::Snare), 1, 100);
        shared.skip[unsigned(Drum::Snare)].store(1);
        run(engine, shared, 88200);
        assert(hits(shared, Drum::Snare) == 0 && shared.skip[unsigned(Drum::Snare)].load() == -1);
        run(engine, shared, 88200);
        assert(hits(shared, Drum::Snare) == 1);
        // A skip left over when the transport stopped is dropped on Play.
        shared.stop(); run(engine, shared, 128);
        shared.skip[unsigned(Drum::Snare)].store(1);
        shared.play(); run(engine, shared, 11136);
        assert(hits(shared, Drum::Snare) == 2);
    }

    // Every drum sounds, stays within range, sits in its place in the stereo
    // field and falls silent within three seconds.
    for (unsigned track = 0; track < tracks; ++track) {
        Shared shared; Engine engine;
        shared.hits.push({0x99, uint8_t(track), 127});
        std::vector<float> samples;
        float peak = run(engine, shared, 3 * sample_rate, &samples);
        assert(peak > 0.05f && peak < 0.3f);
        assert(hits(shared, Drum(track)) == 1 && !engine.sounding(track));
        double left = 0, right = 0;
        for (size_t n = 0; n + 1 < samples.size(); n += 2) { left += samples[n] * samples[n]; right += samples[n + 1] * samples[n + 1]; }
        float pan = drum_pans[track];
        assert(pan > 0 ? right > left * 1.2 : pan < 0 ? left > right * 1.2 : std::fabs(left - right) < 1e-6 * (left + 1));
        // Softer hits are quieter, and velocity 0 or a missing track plays nothing.
        Shared soft; Engine quiet;
        soft.hits.push({0x99, uint8_t(track), 32});
        assert(run(quiet, soft, sample_rate / 2) < peak * 0.4f);
        Shared none; Engine silent;
        none.hits.push({0x99, uint8_t(track), 0}); none.hits.push({0x99, uint8_t(tracks + track), 127});
        assert(run(silent, none, 1024) == 0);
    }

    // A closed hat cuts the open hat short.
    {
        Shared shared; Engine engine;
        shared.hits.push({0x99, uint8_t(Drum::OpenHat), 127});
        run(engine, shared, 4096);
        assert(engine.sounding(unsigned(Drum::OpenHat)));
        shared.hits.push({0x99, uint8_t(Drum::ClosedHat), 127});
        run(engine, shared, 4096);
        assert(!engine.sounding(unsigned(Drum::OpenHat)) && engine.sounding(unsigned(Drum::ClosedHat)));
    }

    // The default groove at full main volume never clips.
    {
        Shared shared; Engine engine;
        assert(run(engine, shared, 8 * sample_rate) < 0.01f);
        shared.play();
        float peak = run(engine, shared, 8 * sample_rate);
        assert(peak > 0.1f && peak < 0.9f);
    }

    // Track colours are distinct and within what the pads accept.
    for (unsigned a = 0; a < tracks; ++a) {
        auto colour = track_colour(a);
        assert(colour.r <= 127 && colour.g <= 127 && colour.b <= 127 && colour.r + colour.g + colour.b > 0);
        for (unsigned b = a + 1; b < tracks; ++b) {
            auto other = track_colour(b);
            assert(colour.r != other.r || colour.g != other.g || colour.b != other.b);
        }
    }
    assert(track_letter(0) == 'A' && track_letter(7) == 'H');
    return 0;
}
