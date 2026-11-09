// SPDX-License-Identifier: AGPL-3.0-only
#include "starter/starter_app.h"
#include "runtime/screen.h"
#include <algorithm>
#include <cmath>

void StarterApp::render(const AudioBlock&, AudioBlock& output) {
    std::array<float, 16> target;
    for (unsigned pad = 0; pad < 16; ++pad) target[pad] = loudness[pad].load(std::memory_order_relaxed);
    for (unsigned frame = 0; frame < audio_frames; ++frame) {
        float sample = 0;
        for (unsigned pad = 0; pad < 16; ++pad) {
            auto& voice = voices[pad];
            voice.level += (target[pad] - voice.level) * 0.002f; // About 10 ms to fade, so no clicks.
            if (voice.level < 1e-5f) continue;
            sample += float(std::sin(6.283185307179586 * voice.phase)) * voice.level * 0.1f;
            voice.phase += 220.0 * std::exp2(pad / 12.0) / audio_rate; // Pad 1 is A3, one semitone per pad.
            voice.phase -= std::floor(voice.phase);
        }
        output[2 * frame] = output[2 * frame + 1] = int32_t(std::clamp(sample, -0.9f, 0.9f) * 2147483647.0f);
    }
}

void StarterApp::surface(const SurfaceEvent& event) {
    if (event.pad < 0) return; // Buttons and knobs: nothing yet.
    unsigned kind = event.midi.status & 0xf0;
    if (kind == 0x90 && event.midi.value) loudness[size_t(event.pad)].store(event.midi.value / 127.0f);
    else if (kind == 0x80 || kind == 0x90) loudness[size_t(event.pad)].store(0);
}

void StarterApp::lights(SurfaceLights& lights, Clock::time_point) {
    uint32_t held = session().held.load();
    for (unsigned pad = 0; pad < 16; ++pad) lights.pad(pad, held & (1u << pad) ? Rgb {0, 90, 127} : Rgb {});
}

void StarterApp::draw(juce::Graphics& g) {
    if (!font) font = load_typeface();
    int pad = session().last_pad.load();
    g.fillAll(juce::Colour(0xff10151b));
    g.setColour(juce::Colour(0xfff0f1eb));
    g.setFont(juce::Font(juce::FontOptions(font).withHeight(32)));
    g.drawText("Hello", 12, 60, 296, 40, juce::Justification::centredLeft, false);
    g.setFont(juce::Font(juce::FontOptions(font).withHeight(16)));
    g.drawText(pad >= 0 ? "Pad " + juce::String(pad + 1) + ", velocity " + juce::String(session().velocity.load()) : "Tap a pad",
               12, 104, 296, 24, juce::Justification::centredLeft, false);
    g.drawText("Push the wheel to return to the stock app", 12, 205, 296, 24, juce::Justification::centredLeft, false);
}
