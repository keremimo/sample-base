// SPDX-License-Identifier: AGPL-3.0-only
#include "demos/tools.h"
#include "runtime/screen.h"
#include "runtime/util.h"
#include "surface/controls.h"
#include "demos/demo_screen.h"
#include "demos/demo_state.h"
#include "demos/light_show/light_show.h"
#include "demos/sequencer/sequencer.h"
#include "demos/synth/synth.h"
#include "demos/system/system.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
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

// Three pads held, one pressed hard, after a few seconds of presses.
PadsView preview_pads() {
    PadsView view;
    view.velocity = {{82, 64, 0, 127, 0, 101, 0, 0, 0, 0, 38, 0, 0, 55, 0, 0}};
    view.pressure[5] = 104; view.pressure[10] = 22; view.pressure[1] = 47;
    view.peak = view.pressure; view.peak[5] = 118; view.peak[3] = 76;
    // Three presses: a quick tap, a slow squeeze and the one held now.
    auto press = [&](unsigned from, unsigned length, float height, uint8_t velocity) {
        view.strikes[from] = velocity;
        for (unsigned n = 0; n < length && from + n < PadsView::trace_length; ++n)
            view.trace[from + n] = uint8_t(std::lround(height * 127 * std::sin(3.14159265 * (n + 1) / (length + 1))));
    };
    press(6, 5, 0.35f, 127); press(20, 22, 0.93f, 76);
    view.strikes[48] = 101;
    for (unsigned n = 49; n < PadsView::trace_length; ++n)
        view.trace[n] = uint8_t(std::lround(104 * std::min(1.0, (n - 49) / 9.0)));
    return view;
}

// The default groove playing step 7 while recording, a snare written in
// and the bell muted, with the clap selected and just struck.
SeqView preview_seq() {
    seq::Pattern pattern;
    for (unsigned step : {4u, 12u, 15u}) pattern.set(unsigned(seq::Drum::Snare), step, step == 15 ? 48 : 96);
    for (unsigned step : {3u, 10u}) pattern.set(unsigned(seq::Drum::Bell), step, 90);
    pattern.set(unsigned(seq::Drum::LowTom), 14, 70); pattern.set(unsigned(seq::Drum::HighTom), 13, 80);
    SeqView view;
    for (unsigned track = 0; track < seq::tracks; ++track)
        for (unsigned step = 0; step < seq::steps; ++step) view.steps[track][step] = pattern.get(track, step);
    view.muted = 1u << unsigned(seq::Drum::Bell);
    view.hits = 1u << unsigned(seq::Drum::Clap) | 1u << unsigned(seq::Drum::ClosedHat);
    view.tempo = 1235; view.step = 6; view.track = uint8_t(seq::Drum::Clap); view.drums = true; view.armed = true;
    return view;
}

// An MPC an hour into a session, recording over USB-C, with the readings
// the device reports (a 3.59 V supply, a six-step backlight and the stock
// mounts plus an SD card).
SystemView preview_system(sysinfo::Panel panel) {
    using namespace sysinfo;
    SystemView view;
    view.panel = panel;
    auto& r = view.readings;
    r.reads = 7200;
    r.cpu = {0.18f, 0.12f, 0.09f, 0.11f, 0.41f};
    for (unsigned n = 0; n < history_length; ++n) r.history.push(0.16f + 0.05f * std::sin(n * 0.45f) + (n % 11 == 3 ? 0.22f : 0));
    r.memory_total_kb = 2018016; r.memory_available_kb = 1789172;
    r.cpu_millidegrees = 39444; r.gpu_millidegrees = 37222;
    r.supply_name = "mains-power"; r.supply_type = "Mains"; r.supply_online = 1; r.supply_microvolts = 3592000;
    r.key_available = true; r.key_presses = 2;
    r.backlight = 6; r.backlight_maximum = 6;
    r.udc_state = "configured";
    r.usb_in.state = UsbStreamState::Stopped;
    r.usb_out = {UsbStreamState::Running, 48000, 2, 32, 1031, 0, 0, 0, 1, -21};
    r.ports = {{"Control Surface", true, true, true}, {"External MIDI", true, true, false}, {"f_midi", true, true, false}};
    r.headphones = 1; r.line_out = 0;
    constexpr uint64_t mb = 1024 * 1024;
    r.storage = {{"/", "ext4", true, 0, 360 * mb}, {"/content", "ext4", true, 412 * mb, 3900 * mb},
                 {"/factory", "ext4", false, 27 * mb, 30 * mb}, {"/data", "ext4", false, 2600 * mb, 2980 * mb},
                 {"/media/3265-3932", "exfat", false, 18400 * mb, 30500 * mb}};
    view.frames = uint64_t(3725) * sample_rate;
    view.capture_active = true; view.peak = 0.31f; view.input_peak = 0.05f;
    return view;
}

// Draws one made-up frame, or a remote prompt when `prompt` is not empty.
int write_preview(const DemoView& view, const std::string& png_path, const std::string& prompt = {}) {
    juce::Image image(juce::Image::ARGB, screen_width, screen_height, true);
    {
        juce::Graphics g(image);
        auto typeface = load_typeface();
        if (prompt.empty()) draw_demo_screen(g, Ui(typeface), view);
        else draw_prompt(g, typeface, prompt);
    }
    save_png(image, png_path);
    return 0;
}

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
    DemoView view; view.last_pad = 0; view.velocity = 100; view.held = 1; view.peak = 0.17f;
    view.last_input = describe_last_input(pack_last_input(InputKind::Control, uint8_t(SurfaceControl::Wheel), uint8_t(-1)));
    return write_preview(view, png_path, prompt);
}

int write_recorder_preview(const std::string& png_path) {
    DemoView view; view.page = Page::Recorder; view.recorder = preview_recorder();
    view.last_input = surface_buttons[size_t(SurfaceButton::Play)].name;
    return write_preview(view, png_path);
}

int write_lights_preview(const std::string& png_path, unsigned position_ms) {
    DemoView view; view.page = Page::Lights; view.light_position = position_ms;
    view.last_input = surface_buttons[size_t(SurfaceButton::PadFx)].name;
    return write_preview(view, png_path);
}

int write_seq_preview(const std::string& png_path) {
    DemoView view; view.page = Page::Seq; view.seq = preview_seq();
    view.last_input = surface_buttons[size_t(SurfaceButton::SeqRecord)].name;
    return write_preview(view, png_path);
}

int write_system_preview(const std::string& png_path, sysinfo::Panel panel) {
    DemoView view; view.page = Page::System; view.system = preview_system(panel);
    view.last_input = surface_buttons[size_t(SurfaceButton::KnobFx)].name;
    return write_preview(view, png_path);
}

int write_pads_preview(const std::string& png_path) {
    DemoView view; view.page = Page::Pads; view.pads = preview_pads();
    view.last_pad = 5; view.velocity = 101; view.held = 1u << 5 | 1u << 10 | 1u << 1;
    view.last_input = surface_buttons[size_t(SurfaceButton::SixteenLevels)].name;
    return write_preview(view, png_path);
}
} // namespace

std::optional<int> run_demo_tool(int argc, char** argv) {
    // Each tool takes exactly its own arguments.
    auto tool = [&](const char* flag, int min_args, int max_args) {
        return argc - 1 >= min_args && argc - 1 <= max_args && !strcmp(argv[1], flag);
    };
    if (tool("--self-test", 2, 2)) return self_test(argv[2]);
    if (tool("--preview", 2, 3)) return write_synth_preview(argv[2], argc == 4 ? argv[3] : "");
    if (tool("--preview-recorder", 2, 2)) return write_recorder_preview(argv[2]);
    if (tool("--preview-pads", 2, 2)) return write_pads_preview(argv[2]);
    if (tool("--preview-seq", 2, 2)) return write_seq_preview(argv[2]);
    if (tool("--preview-system", 2, 3)) {
        auto name = argc == 4 ? std::find(sysinfo::panel_names.begin(), sysinfo::panel_names.end(), std::string(argv[3]))
                              : sysinfo::panel_names.begin();
        require(name != sysinfo::panel_names.end(), "Unknown system panel; use health, audio, ports or storage");
        return write_system_preview(argv[2], sysinfo::Panel(name - sysinfo::panel_names.begin()));
    }
    if (tool("--preview-lights", 2, 3)) {
        // By default, mid-way through the rainbow.
        unsigned position = light_scene_start(3) + 1800;
        require(argc == 3 || parse_unsigned(std::string(argv[3]), position), "Invalid light show position");
        return write_lights_preview(argv[2], position);
    }
    return std::nullopt;
}
