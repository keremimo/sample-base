// SPDX-License-Identifier: AGPL-3.0-only
#include "demos/sequencer/sequencer_demo.h"
#include "runtime/util.h"
#include <algorithm>
#include <cctype>
#include <sstream>

namespace {
bool on_page(const DemoState& state) { return state.page.load() == Page::Seq; }
bool drum_pads(const seq::Shared& shared) { return seq::PadMode(shared.pad_mode.load()) == seq::PadMode::Drums; }

void select_track(seq::Shared& shared, int step) {
    int count = int(seq::tracks);
    shared.track.store(uint8_t(((int(shared.track.load()) + step) % count + count) % count));
}

// A drum pad: plays the drum and selects its track. While recording it is
// also written onto the nearest step.
void play_drum(seq::Shared& shared, unsigned track, uint8_t velocity) {
    shared.track.store(uint8_t(track));
    shared.hits.push({0x99, uint8_t(track), velocity});
    if (!shared.armed.load()) return;
    uint32_t position = shared.position.load();
    int step = seq::nearest_step(position);
    if (step < 0) return;
    shared.pattern.set(track, unsigned(step), velocity);
    // Written ahead of the playhead: it has just sounded, so not again in a moment.
    if (step != seq::position_step(position)) shared.skip[track].store(int8_t(step));
}

// "A" to "H", either case.
int parse_track(const std::string& text) {
    if (text.size() != 1) return -1;
    int track = std::toupper(static_cast<unsigned char>(text[0])) - 'A';
    return track >= 0 && track < int(seq::tracks) ? track : -1;
}

// A tempo in BPM with at most one decimal, "92" or "92.5", as tenths.
bool parse_tempo(const std::string& text, unsigned& tempo) {
    auto dot = text.find('.');
    unsigned whole = 0, tenth = 0;
    if (!parse_unsigned(text.substr(0, dot), whole) || whole > 1000) return false;
    if (dot != std::string::npos && (text.size() != dot + 2 || !parse_unsigned(text.substr(dot + 1), tenth))) return false;
    tempo = whole * 10 + tenth;
    return tempo >= seq::min_tempo && tempo <= seq::max_tempo;
}

std::string sequencer_json(const DemoState& state) {
    const auto& shared = state.seq;
    uint32_t position = shared.position.load();
    std::ostringstream out;
    out << "{\"state\":\"" << (position ? "playing" : "stopped") << "\",\"step\":" << seq::position_step(position) + 1
        << ",\"tempo\":" << tempo_text(shared.tempo.load()) << ",\"track\":\"" << seq::track_letter(shared.track.load())
        << "\",\"pads\":\"" << (drum_pads(shared) ? "drums" : "steps") << "\",\"armed\":" << (shared.armed.load() ? "true" : "false")
        << ",\"tracks\":[";
    for (unsigned track = 0; track < seq::tracks; ++track) {
        out << (track ? "," : "") << "{\"track\":\"" << seq::track_letter(track) << "\",\"drum\":\"" << seq::drum_names[track]
            << "\",\"muted\":" << (shared.pattern.muted(track) ? "true" : "false") << ",\"hits\":" << shared.triggers[track].load()
            << ",\"steps\":[";
        for (unsigned step = 0; step < seq::steps; ++step) out << (step ? "," : "") << unsigned(shared.pattern.get(track, step));
        out << "]}";
    }
    out << "]}";
    return out.str();
}
} // namespace

bool SequencerInput::surface(DemoState& state, const SurfaceEvent& event) {
    const auto& input = event.input;
    if (is_button(input, SurfaceButton::Shift)) shift = input.pressed;
    if (!on_page(state) || event.syncing) return false;
    auto& shared = state.seq;
    // A wheel step is 1 BPM, or 0.1 with Shift held.
    if (is_control(input, SurfaceControl::Wheel)) { shared.nudge_tempo(input.delta * (shift ? 1 : 10)); return false; }
    if (event.pad < 0 || (event.midi.status & 0xf0) != 0x90 || !event.midi.value) return false;
    auto pad = unsigned(event.pad);
    uint8_t velocity = event.midi.value;
    unsigned track = shared.track.load();
    if (!drum_pads(shared)) {
        // A new step is heard at once while stopped; while playing, on its turn.
        if (shared.pattern.toggle(track, pad, velocity) && !shared.playing()) shared.hits.push({0x99, uint8_t(track), velocity});
    } else if (pad < seq::tracks) {
        play_drum(shared, pad, velocity);
    } else {
        shared.pattern.toggle_mute(pad - seq::tracks);
    }
    return true;
}

// Shift turns the track buttons backwards, the tempo buttons to 0.1 BPM and
// Erase to every track.
bool SequencerInput::button(DemoState& state, SurfaceButton button, Clock::time_point now) {
    auto& shared = state.seq;
    switch (button) {
    case SurfaceButton::TopLeft:
        shared.pad_mode.store(uint8_t(drum_pads(shared) ? seq::PadMode::Steps : seq::PadMode::Drums)); return true;
    case SurfaceButton::TopMiddle: case SurfaceButton::PadBank: select_track(shared, shift ? -1 : 1); return true;
    case SurfaceButton::Play: shared.play(); return true;
    case SurfaceButton::Minus: shared.nudge_tempo(shift ? -1 : -10); return true;
    case SurfaceButton::Plus: shared.nudge_tempo(shift ? 1 : 10); return true;
    case SurfaceButton::TapTempo: {
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
        if (auto tempo = taps.tap(uint64_t(ms))) shared.set_tempo(*tempo);
        return true;
    }
    case SurfaceButton::Mute: shared.pattern.toggle_mute(shared.track.load()); return true;
    case SurfaceButton::Erase:
        if (!shift) shared.pattern.clear(shared.track.load());
        else for (unsigned track = 0; track < seq::tracks; ++track) shared.pattern.clear(track);
        return true;
    default: return false;
    }
}

void sequencer_record_button(DemoState& state) {
    auto& shared = state.seq;
    bool arm = !on_page(state) || !shared.armed.load();
    shared.armed.store(arm);
    if (arm) shared.pad_mode.store(uint8_t(seq::PadMode::Drums));
    state.page.store(Page::Seq);
}

// Step mode shows the selected track's steps in its colour, brighter the
// harder, with the playhead white (red while recording). Drum mode lights
// the drums dim in their colours and flashes each as it sounds; the mute
// pads above show red when muted. The bank lights and fader light show the
// selected track, and Tap Tempo flashes on the beat, red on the bar.
void SequencerLights::update(SurfaceLights& lights, const DemoState& state, Clock::time_point now) {
    using namespace std::chrono;
    const auto& shared = state.seq;
    if (!showing) {
        showing = true; next_frame = now; hit.fill({});
        for (unsigned track = 0; track < seq::tracks; ++track) triggers_seen[track] = shared.triggers[track].load();
        // The recorder's lights wait until the page closes.
        lights.button(SurfaceButton::Sample, ButtonLight::Off);
        lights.button(SurfaceButton::SampleRecord, ButtonLight::Off);
    }
    if (now < next_frame) return;
    next_frame = now + milliseconds(20);
    for (unsigned track = 0; track < seq::tracks; ++track) {
        auto count = shared.triggers[track].load();
        if (count != triggers_seen[track]) { triggers_seen[track] = count; hit[track] = now; }
    }
    unsigned track = shared.track.load();
    uint32_t position = shared.position.load();
    int playing = seq::position_step(position);
    bool armed = shared.armed.load();

    if (!drum_pads(shared)) {
        float shade = shared.pattern.muted(track) ? 0.5f : 1;
        Rgb head = armed ? led_colour::rgb(1, 0.15f, 0.15f) : led_colour::white();
        for (unsigned step = 0; step < seq::steps; ++step) {
            uint8_t velocity = shared.pattern.get(track, step);
            Rgb colour = velocity ? seq::track_colour(track, (0.35f + 0.65f * velocity / 127) * shade) : Rgb {};
            if (int(step) == playing) colour = velocity ? mix_light(colour, head, 0.5f) : mix_light({}, head, 0.3f);
            lights.pad(step, colour);
        }
    } else {
        for (unsigned drum = 0; drum < seq::tracks; ++drum) {
            bool muted = shared.pattern.muted(drum);
            float flash = std::max(0.0f, 1 - duration<float, std::milli>(now - hit[drum]).count() / 180.0f);
            float rest = (drum == track ? 0.45f : 0.3f) * (muted ? 0.5f : 1);
            lights.pad(drum, seq::track_colour(drum, rest + (1 - rest) * flash));
            lights.pad(drum + seq::tracks, muted ? led_colour::rgb(1, 0, 0, 0.5f) : seq::track_colour(drum, 0.2f));
        }
    }
    lights.fader(seq::track_colour(track, 0.8f));
    for (unsigned bank = 0; bank < surface_bank_lights; ++bank) lights.bank(bank, bank == track);

    bool beat = playing >= 0 && playing % 4 == 0;
    lights.button(SurfaceButton::Seq, ButtonLight::Bright);
    lights.button(SurfaceButton::Play, playing >= 0 ? ButtonLight::Bright : ButtonLight::Dim);
    lights.button(SurfaceButton::Stop, playing >= 0 ? ButtonLight::Dim : ButtonLight::Off);
    lights.button(SurfaceButton::SeqRecord, !armed ? ButtonLight::Off : playing >= 0 ? ButtonLight::Bright : ButtonLight::Dim);
    lights.button(SurfaceButton::TapTempo, !beat ? ButtonLight::Dim : playing == 0 ? ButtonLight::RedBright : ButtonLight::Bright);
    lights.button(SurfaceButton::Mute, shared.pattern.muted(track) ? ButtonLight::RedBright : ButtonLight::Dim);
    lights.button(SurfaceButton::Erase, shared.pattern.empty(track) ? ButtonLight::Off : ButtonLight::Dim);
    lights.button(SurfaceButton::PadBank, ButtonLight::Dim);
}

bool SequencerLights::close(SurfaceLights& lights) {
    if (!showing) return false;
    showing = false;
    for (unsigned pad = 0; pad < surface_pad_lights; ++pad) lights.pad(pad, {});
    lights.fader({});
    for (unsigned bank = 0; bank < surface_bank_lights; ++bank) lights.bank(bank, false);
    for (auto button : {SurfaceButton::Seq, SurfaceButton::Play, SurfaceButton::Stop, SurfaceButton::SeqRecord,
                        SurfaceButton::TapTempo, SurfaceButton::Mute, SurfaceButton::Erase, SurfaceButton::PadBank})
        lights.button(button, ButtonLight::Off);
    return true;
}

std::string sequencer_command(DemoState& state, const std::string& action, const std::string& argument) {
    if (action != "seq") return {};
    auto& shared = state.seq;
    std::istringstream words(argument);
    std::string verb, first, second, third, extra;
    words >> verb >> first >> second >> third;
    const std::string usage = "ERR use seq, seq play|stop|clear|reset, seq tempo 40-240, seq step A-H 1-16 0-127 or seq mute A-H on|off";
    if (words >> extra) return usage;
    if (verb.empty()) return "OK " + sequencer_json(state);
    if (first.empty()) {
        if (verb == "play") { shared.play(); return "OK seq play requested"; }
        if (verb == "stop") { shared.stop(); return "OK seq stop requested"; }
        if (verb == "clear") { shared.pattern.clear_all(); return "OK seq clear"; }
        if (verb == "reset") { shared.pattern.load_default(); shared.set_tempo(seq::default_tempo); return "OK seq reset"; }
        return usage;
    }
    unsigned tempo = 0, step = 0, velocity = 0;
    if (verb == "tempo" && second.empty() && parse_tempo(first, tempo)) {
        shared.set_tempo(tempo); return "OK seq tempo " + tempo_text(tempo);
    }
    int track = parse_track(first);
    if (track < 0) return usage;
    std::string name(1, seq::track_letter(unsigned(track)));
    if (verb == "step" && parse_unsigned(second, step) && step >= 1 && step <= seq::steps && parse_unsigned(third, velocity) && velocity <= 127) {
        shared.pattern.set(unsigned(track), step - 1, uint8_t(velocity));
        return "OK seq step " + name + ' ' + second + ' ' + third;
    }
    if (verb == "mute" && third.empty() && (second == "on" || second == "off")) {
        shared.pattern.mute(unsigned(track), second == "on"); return "OK seq mute " + name + ' ' + second;
    }
    return usage;
}

std::string sequencer_status(const DemoState& state) {
    uint32_t position = state.seq.position.load();
    return std::string("seq=") + (position ? "playing" : "stopped") + " seq_step=" + std::to_string(seq::position_step(position) + 1) +
           " tempo=" + tempo_text(state.seq.tempo.load());
}

std::string tempo_text(unsigned tempo) { return std::to_string(tempo / 10) + '.' + std::to_string(tempo % 10); }

SeqView SeqFeed::frame(const DemoState& state, bool shown) {
    SeqView view;
    const auto& shared = state.seq;
    for (unsigned track = 0; track < seq::tracks; ++track) {
        auto count = shared.triggers[track].load();
        if (count != triggers_seen[track]) { triggers_seen[track] = count; view.hits |= uint8_t(1u << track); }
    }
    if (!shown) return {};
    for (unsigned track = 0; track < seq::tracks; ++track) {
        for (unsigned step = 0; step < seq::steps; ++step) view.steps[track][step] = shared.pattern.get(track, step);
        if (shared.pattern.muted(track)) view.muted |= uint8_t(1u << track);
    }
    view.tempo = shared.tempo.load(); view.step = seq::position_step(shared.position.load());
    view.track = shared.track.load(); view.drums = drum_pads(shared); view.armed = shared.armed.load();
    return view;
}
