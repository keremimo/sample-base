// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include "surface/led_colours.h"
#include "surface/leds.h"
#include "surface/midi.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <optional>

// Step sequencer and drum machine. Eight tracks, one drum each, shown on the
// pad bank lights A-H; each track has sixteen steps of a 16th note, one bar.
// A step holds the velocity it was entered with, 0 when off. The clock counts
// audio frames inside the audio callback, so every step starts on the sample
// it is due, whatever the MIDI and screen threads are doing.
namespace seq {
constexpr unsigned tracks = 8, steps = 16, sample_rate = 44100;
// Tempo in tenths of a BPM, so taps and the wheel can reach 92.5.
constexpr unsigned min_tempo = 400, max_tempo = 2400, default_tempo = 1100;

enum class Drum : uint8_t { Kick, Snare, Clap, ClosedHat, OpenHat, LowTom, HighTom, Bell };
constexpr std::array<const char*, tracks> drum_names {{
    "Kick", "Snare", "Clap", "Closed hat", "Open hat", "Low tom", "High tom", "Bell"}};
constexpr std::array<const char*, tracks> drum_labels {{"KICK", "SNARE", "CLAP", "HAT", "OPEN", "LO TOM", "HI TOM", "BELL"}};
// Stereo position, -1 left to 1 right, as a kit sits in front of a drummer.
constexpr std::array<float, tracks> drum_pans {{0, 0, 0.1f, 0.25f, 0.25f, -0.35f, 0.35f, -0.2f}};

inline char track_letter(unsigned track) { return char('A' + track % tracks); }
// Each track's colour on the pads, the fader light and the screen: eight
// hues round the wheel, kick red.
inline Rgb track_colour(unsigned track, float level = 1) { return led_colour::hue(float(track % tracks) / tracks, level); }
inline unsigned clamp_tempo(unsigned tempo) { return std::clamp(tempo, min_tempo, max_tempo); }
// Frames in one 16th note; fractional, so long runs keep exact time.
inline double frames_per_step(unsigned tempo) { return sample_rate * 600.0 / (clamp_tempo(tempo) * 4.0); }

// The audio thread publishes where it is as one number: 0 while stopped,
// else (step + 1) << 8 | how far into the step, 0-255.
constexpr uint32_t position_of(unsigned step, unsigned phase) { return (step % steps + 1) << 8 | (phase & 255); }
inline int position_step(uint32_t position) { return position ? int(position >> 8) - 1 : -1; }
// The step a live hit is recorded on: the playing step in its first half,
// the next one in its second half. -1 while stopped.
inline int nearest_step(uint32_t position) {
    int step = position_step(position);
    if (step < 0) return -1;
    return (position & 255) < 128 ? step : (step + 1) % int(steps);
}

// Every track's steps and mutes. Any thread may read or change them; a step
// is one atomic byte, so the audio thread never sees half a change.
class Pattern {
public:
    Pattern() { load_default(); }
    uint8_t get(unsigned track, unsigned step) const {
        return track < tracks && step < steps ? cells[track][step].load(std::memory_order_relaxed) : 0;
    }
    void set(unsigned track, unsigned step, uint8_t velocity) {
        if (track < tracks && step < steps) cells[track][step].store(std::min<uint8_t>(velocity, 127), std::memory_order_relaxed);
    }
    // A pad tap in step mode: an empty step takes the strike's velocity, a
    // set one clears. Returns whether the step is now on.
    bool toggle(unsigned track, unsigned step, uint8_t velocity) {
        bool on = !get(track, step) && velocity;
        set(track, step, on ? velocity : 0);
        return on;
    }
    void clear(unsigned track) { for (unsigned step = 0; step < steps; ++step) set(track, step, 0); }
    void clear_all() { for (unsigned track = 0; track < tracks; ++track) { clear(track); mute(track, false); } }
    bool empty(unsigned track) const {
        for (unsigned step = 0; step < steps; ++step) if (get(track, step)) return false;
        return true;
    }
    bool muted(unsigned track) const { return track < tracks && mutes[track].load(std::memory_order_relaxed); }
    void mute(unsigned track, bool on) { if (track < tracks) mutes[track].store(on, std::memory_order_relaxed); }
    void toggle_mute(unsigned track) { mute(track, !muted(track)); }
    // A house groove to start from: four on the floor, claps on 2 and 4, soft
    // closed hats on the 16ths between and an open hat on every off-beat,
    // which the next closed hat cuts short. Snare, toms and bell are empty.
    void load_default() {
        clear_all();
        for (unsigned beat = 0; beat < 4; ++beat) {
            set(unsigned(Drum::Kick), beat * 4, 112);
            set(unsigned(Drum::OpenHat), beat * 4 + 2, 84);
            set(unsigned(Drum::ClosedHat), beat * 4 + 1, 40);
            set(unsigned(Drum::ClosedHat), beat * 4 + 3, 52);
        }
        set(unsigned(Drum::Clap), 4, 100); set(unsigned(Drum::Clap), 12, 100);
    }
private:
    std::array<std::array<std::atomic<uint8_t>, steps>, tracks> cells {};
    std::array<std::atomic<bool>, tracks> mutes {};
};

// Tap Tempo: the tempo of the last taps' average interval, over up to four
// intervals. A tap more than two seconds after the last starts again, and the
// first tap only starts the count. Times are in milliseconds.
class TapTempo {
public:
    std::optional<unsigned> tap(uint64_t ms) {
        if (count && (ms < times[count - 1] || ms - times[count - 1] > 2000)) count = 0;
        if (count == times.size()) { std::rotate(times.begin(), times.begin() + 1, times.end()); --count; }
        times[count++] = ms;
        if (count < 2 || times[count - 1] == times[0]) return std::nullopt;
        double interval = double(times[count - 1] - times[0]) / (count - 1);
        return clamp_tempo(unsigned(std::lround(600000.0 / interval)));
    }
private:
    std::array<uint64_t, 5> times {};
    size_t count = 0;
};

enum class Transport : uint8_t { None, Play, Stop };
enum class PadMode : uint8_t { Steps, Drums };

// What the sequencer's parts share across threads. Every field is atomic or
// a single-producer queue; comments name the writer where only one may write.
struct Shared {
    Pattern pattern;
    std::atomic<unsigned> tempo {default_tempo};
    std::atomic<uint8_t> transport {uint8_t(Transport::None)}; // Any thread asks; the audio thread takes it.
    std::atomic<uint32_t> position {0};                        // Audio thread; see position_of().
    std::atomic<uint8_t> track {0};                            // The selected track.
    std::atomic<uint8_t> pad_mode {uint8_t(PadMode::Steps)};
    std::atomic<bool> armed {false};                           // Seq Record: live hits are written in.
    MidiQueue<64> hits; // Live drum hits, key = track, value = velocity. The MIDI thread is the only producer.
    // A live hit recorded onto the step after the playing one has already
    // sounded; the audio thread skips that step once instead of playing it twice.
    std::array<std::atomic<int8_t>, tracks> skip {};
    std::array<std::atomic<uint16_t>, tracks> triggers {}; // Hits per track, live or sequenced. Audio thread.

    Shared() { for (auto& step : skip) step.store(-1, std::memory_order_relaxed); }
    void play() { transport.store(uint8_t(Transport::Play)); }
    void stop() { transport.store(uint8_t(Transport::Stop)); }
    bool playing() const { return position.load(std::memory_order_relaxed) != 0; }
    void set_tempo(unsigned value) { tempo.store(clamp_tempo(value)); }
    void nudge_tempo(int tenths) { set_tempo(unsigned(std::max(0, int(tempo.load()) + tenths))); }
};

// Topology-preserving state-variable filter (Zavalishin): stable at any
// cutoff, so it can sit at 7 kHz for the hats.
class Filter {
public:
    struct Out { float low, band, high; };
    void set(float hz, float q) {
        float g = std::tan(3.14159265f * hz / sample_rate);
        k = 1 / q; a1 = 1 / (1 + g * (g + k)); a2 = g * a1; a3 = g * a2; ic1 = ic2 = 0;
    }
    Out run(float x) {
        float v3 = x - ic2, v1 = a1 * ic1 + a2 * v3, v2 = ic2 + a2 * ic1 + a3 * v3;
        ic1 = 2 * v1 - ic1; ic2 = 2 * v2 - ic2;
        return {v2, v1, x - k * v1 - v2};
    }
private:
    float k = 1, a1 = 0, a2 = 0, a3 = 0, ic1 = 0, ic2 = 0;
};

// One drum. Envelopes fall by a multiply per sample and the filter
// coefficients are set once per hit, so a sounding voice costs a few
// multiplies, at most one sine and six square waves per sample.
class DrumVoice {
public:
    void start(Drum kind, float level) {
        auto decay = [](float seconds) { return std::exp(-1 / (seconds * sample_rate)); };
        *this = DrumVoice {};
        drum = kind; on = true; gain = level;
        switch (drum) {
        case Drum::Kick: tone(48, 110, 0.035f); amp_k = decay(0.30f); snap = 0.25f; snap_k = decay(0.003f); break;
        case Drum::Snare:
            tone(175, 60, 0.02f); amp = 0.3f; amp_k = decay(0.07f);
            snap = 0.45f; snap_k = decay(0.13f); filter.set(1500, 0.7f); break;
        case Drum::Clap: amp = 0; snap = 1; snap_k = decay(0.004f); filter.set(1100, 1.5f); break;
        case Drum::ClosedHat: amp = 0.6f; amp_k = decay(0.03f); filter.set(7000, 0.8f); break;
        case Drum::OpenHat: amp = 0.6f; amp_k = decay(0.28f); filter.set(7000, 0.8f); break;
        case Drum::LowTom: tone(85, 45, 0.08f); amp = 0.8f; amp_k = decay(0.25f); snap = 0.1f; snap_k = decay(0.002f); break;
        case Drum::HighTom: tone(140, 70, 0.08f); amp = 0.8f; amp_k = decay(0.2f); snap = 0.1f; snap_k = decay(0.002f); break;
        case Drum::Bell: amp = 0.4f; amp_k = decay(0.3f); snap = 0.6f; snap_k = decay(0.05f); filter.set(800, 2.5f); break;
        }
    }
    // Cut short in 5 ms, as a closed hat cuts an open one.
    void choke() { if (on) choke_k = std::exp(-1 / (0.005f * sample_rate)); }
    bool active() const { return on; }
    Drum kind() const { return drum; }

    // The next sample, -1..1 times the hit's level. `noise` is the caller's
    // xorshift state, shared by every voice.
    float next(uint32_t& noise) {
        if (!on) return 0;
        noise ^= noise << 13; noise ^= noise >> 17; noise ^= noise << 5;
        float white = float(int32_t(noise)) / 2147483648.0f, out = 0;
        switch (drum) {
        case Drum::Kick: case Drum::LowTom: case Drum::HighTom: out = sine() * amp + white * snap; break;
        case Drum::Snare: out = sine() * amp + filter.run(white).high * snap; break;
        case Drum::Clap:
            // Three quick bursts 10 ms apart, then the room's tail.
            if (age == 441 || age == 882) snap = 1;
            if (age == 1323) { amp = 0.8f; amp_k = std::exp(-1 / (0.15f * sample_rate)); }
            out = filter.run(white).band * 1.8f * (snap + amp);
            break;
        case Drum::ClosedHat: case Drum::OpenHat:
            out = filter.run(metal(6) * 0.7f + white * 0.3f).high * amp * 3.2f; break;
        case Drum::Bell: out = filter.run(metal(2)).band * 0.28f * (amp + snap); break;
        }
        amp *= amp_k * choke_k; snap *= snap_k * choke_k; sweep *= sweep_k; ++age;
        if (age > 1400 && amp < 2e-4f && snap < 2e-4f) on = false;
        return out * gain;
    }
private:
    void tone(float base, float swept, float seconds) {
        base_hz = base; sweep_hz = swept; sweep = 1; sweep_k = std::exp(-1 / (seconds * sample_rate));
    }
    float sine() {
        phase += (base_hz + sweep_hz * sweep) / sample_rate;
        phase -= std::floor(phase);
        return std::sin(6.2831853f * phase);
    }
    // The first `count` of six detuned square waves; the bell uses its own two.
    float metal(unsigned count) {
        static constexpr std::array<float, 6> hats {{205.3f, 304.4f, 369.6f, 522.7f, 540.0f, 800.0f}};
        static constexpr std::array<float, 2> bell {{540.0f, 800.0f}};
        float sum = 0;
        for (unsigned n = 0; n < count; ++n) {
            squares[n] += (count == 2 ? bell[n] : hats[n]) / sample_rate;
            squares[n] -= std::floor(squares[n]);
            sum += squares[n] < 0.5f ? 1 : -1;
        }
        return sum / float(count);
    }

    Drum drum = Drum::Kick;
    bool on = false;
    uint32_t age = 0;
    float gain = 0, amp = 1, amp_k = 0, snap = 0, snap_k = 0, choke_k = 1;
    float base_hz = 0, sweep_hz = 0, sweep = 0, sweep_k = 0, phase = 0;
    std::array<float, 6> squares {};
    Filter filter;
};

// The audio thread's half: the clock and one voice per track. A new hit on a
// track restarts its voice; a closed hat chokes the open hat.
class Engine {
public:
    static constexpr float level = 0.2f; // Full-velocity kick peak, before the main volume.

    // Adds the drums to interleaved stereo `out` (-1..1), `frames` frames.
    void render(Shared& shared, float* out, unsigned frames) {
        auto request = Transport(shared.transport.exchange(uint8_t(Transport::None)));
        if (request == Transport::Stop) { playing = false; step = -1; }
        if (request == Transport::Play && !playing) {
            playing = true; step = -1; countdown = 0;
            for (auto& skip : shared.skip) skip.store(-1, std::memory_order_relaxed);
        }
        MidiEvent hit;
        for (unsigned n = 0; n < 64 && shared.hits.pop(hit); ++n) trigger(shared, hit.key, hit.value);
        shared.hits.take_overflow(); // Hits are one-shots: a lost one leaves nothing sounding.

        for (unsigned frame = 0; frame < frames; ++frame) {
            if (playing) {
                if (countdown <= 0) {
                    step = (step + 1) % int(steps);
                    length = frames_per_step(shared.tempo.load(std::memory_order_relaxed));
                    countdown += length;
                    play_step(shared, unsigned(step));
                }
                countdown -= 1;
            }
            float left = 0, right = 0;
            for (unsigned track = 0; track < tracks; ++track) {
                if (!voices[track].active()) continue;
                float sample = voices[track].next(noise), pan = drum_pans[track];
                left += sample * (pan > 0 ? 1 - pan : 1); right += sample * (pan < 0 ? 1 + pan : 1);
            }
            out[2 * frame] += left * level; out[2 * frame + 1] += right * level;
        }
        unsigned phase = playing ? unsigned(std::clamp((length - countdown) / length * 256, 0.0, 255.0)) : 0;
        shared.position.store(playing ? position_of(unsigned(step), phase) : 0, std::memory_order_relaxed);
    }
    bool sounding(unsigned track) const { return track < tracks && voices[track].active(); }

private:
    void trigger(Shared& shared, unsigned track, uint8_t velocity) {
        if (track >= tracks || !velocity) return;
        if (Drum(track) == Drum::ClosedHat) voices[unsigned(Drum::OpenHat)].choke();
        voices[track].start(Drum(track), std::min<uint8_t>(velocity, 127) / 127.0f);
        shared.triggers[track].fetch_add(1, std::memory_order_relaxed);
    }
    void play_step(Shared& shared, unsigned at) {
        for (unsigned track = 0; track < tracks; ++track) {
            int8_t recorded = int8_t(at);
            if (shared.skip[track].compare_exchange_strong(recorded, -1, std::memory_order_relaxed)) continue;
            uint8_t velocity = shared.pattern.get(track, at);
            if (velocity && !shared.pattern.muted(track)) trigger(shared, track, velocity);
        }
    }

    std::array<DrumVoice, tracks> voices {};
    uint32_t noise = 0x9e3779b9;
    bool playing = false;
    int step = -1;
    double countdown = 0, length = 1;
};
} // namespace seq
