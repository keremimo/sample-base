// SPDX-License-Identifier: AGPL-3.0-only
#include "app/offline.h"
#include "audio.h"
#include "controls.h"
#include "hardware_midi.h"
#include "mixer.h"
#include "platform.h"
#include "recorder.h"
#include "system_services.h"
#include "app/app_state.h"
#include "app/screen.h"
#include "app/synth.h"
#include "app/util.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <iostream>

namespace {
void little(std::ofstream& file, uint32_t value, unsigned bytes) {
    for (unsigned n = 0; n < bytes; ++n) file.put(char((value >> (n * 8)) & 255));
}

// A few seconds of a decaying drum-like take, for the recorder preview.
RecorderView preview_recorder() {
    RecorderView view;
    view.source = uint8_t(InputSource::Line); view.channels = uint8_t(InputChannels::Stereo);
    view.state = uint8_t(Recorder::State::Playing);
    view.frames = view.scale = 286650; view.position = 101430; view.capacity = 60 * sample_rate;
    view.left = 0.42f; view.right = 0.18f; view.detail = "Line inputs L and R";
    for (unsigned column = 0; column < RecorderView::columns; ++column) {
        double beat = std::fmod(column / 37.0, 1.0), envelope = std::exp(-beat * 5.0);
        view.waveform[column] = uint8_t(std::clamp(220.0 * envelope * (0.75 + 0.25 * std::sin(column * 1.7)) + 6, 0.0, 255.0));
    }
    return view;
}

int write_preview(const ScreenState& state, const std::string& png_path) {
    Screen screen(default_font_path());
    save_png(screen.draw(state), png_path);
    return 0;
}
} // namespace

int self_test(const std::string& wav_path) {
    Synth engine;
    constexpr unsigned rate = sample_rate, frames = rate * 2;
    std::ofstream file(wav_path, std::ios::binary);
    require(bool(file), "Create test audio");
    file.write("RIFF", 4); little(file, 36 + frames * 4, 4); file.write("WAVEfmt ", 8);
    little(file, 16, 4); little(file, 1, 2); little(file, 2, 2); little(file, rate, 4);
    little(file, rate * 4, 4); little(file, 4, 2); little(file, 16, 2);
    file.write("data", 4); little(file, frames * 4, 4);
    std::array<int32_t, block_frames * 2> samples {};
    engine.midi({0x90,60,100});
    float peak = 0; double energy = 0, tail = 0;
    bool released = false;
    for (unsigned offset = 0; offset < frames; offset += block_frames) {
        if (!released && offset >= rate / 2) { engine.midi({0x80,60,0}); released = true; }
        peak = std::max(peak, engine.render(samples.data()));
        for (unsigned i = 0; i < std::min(block_frames, frames - offset) * 2; ++i) {
            double value = samples[i] / 2147483648.0;
            energy += value * value;
            if (offset > rate * 3 / 2) tail += value * value;
            little(file, uint16_t(samples[i] >> 16), 2);
        }
    }
    double rms = std::sqrt(energy / (frames * 2)), tail_rms = std::sqrt(tail / rate);
    std::cout << "SELF_TEST peak=" << peak << " rms=" << rms << " tail_rms=" << tail_rms << '\n';
    require(file.good() && peak > 0.001f && rms > 0.0001 && tail_rms < 0.001, "Synth note/release self-test failed");
    return 0;
}

int write_synth_preview(const std::string& png_path, const std::string& prompt) {
    ScreenState state; state.last_pad = 0; state.velocity = 100; state.held = 1; state.peak = 0.17f;
    state.last_input = describe_last_input(pack_last_input(InputKind::Control, uint8_t(SurfaceControl::Wheel), uint8_t(-1)));
    state.prompt = prompt;
    return write_preview(state, png_path);
}

int write_recorder_preview(const std::string& png_path) {
    ScreenState state; state.page = Page::Recorder; state.recorder = preview_recorder();
    state.last_input = surface_buttons[size_t(SurfaceButton::Play)].name;
    return write_preview(state, png_path);
}

int write_lights_preview(const std::string& png_path, unsigned position_ms) {
    ScreenState state; state.page = Page::Lights; state.light_position = position_ms;
    state.last_input = surface_buttons[size_t(SurfaceButton::PadFx)].name;
    return write_preview(state, png_path);
}

int print_hardware() {
    Platform platform; HardwareMixer mixer; SystemServices services;
    std::cout << "{\"platform\":" << platform.json() << ",\"audio\":" << AudioDevice::devices_json()
              << ",\"mixer\":" << mixer.json() << ",\"storage\":" << services.storage_json() << ",\"midi\":[";
    bool first = true;
    for (const auto& port : HardwareMidi::enumerate()) {
        if (!first) std::cout << ',';
        first = false;
        std::cout << "{\"address\":" << json_string(port.address) << ",\"card\":" << json_string(port.card)
                  << ",\"name\":" << json_string(port.name) << ",\"input\":" << (port.input ? "true" : "false")
                  << ",\"output\":" << (port.output ? "true" : "false") << '}';
    }
    std::cout << "]}\n"; return 0;
}
