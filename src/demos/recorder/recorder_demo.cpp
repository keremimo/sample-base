// SPDX-License-Identifier: AGPL-3.0-only
#include "demos/recorder/recorder_demo.h"
#include "drivers/mixer.h"
#include "drivers/platform.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <sstream>
#include <utility>

namespace {
constexpr double full_scale = 2147483648.0; // S32 sample to -1..1.

// Service thread only. Recording stops before any route changes, and the new
// source is published only once its route is in place. Without the I2S
// mixer (a host test or another --audio-input) routes are left alone.
bool select_source(DemoState& state, Hardware& hardware, InputSource source, const std::string& usb_device) {
    state.source.store(uint8_t(InputSource::Off));
    if (source != InputSource::Usb) state.usb.stop();
    if (hardware.mixer.available()) {
        const char* profile = source == InputSource::Line ? "line" : source == InputSource::Mic ? "mic" : "off";
        if (!hardware.mixer.input(profile)) { hardware.mixer.input("off"); return false; }
    }
    if (source == InputSource::Usb && state.usb.state() == UsbStreamState::Stopped) state.usb.start(usb_device);
    state.source.store(uint8_t(source));
    return true;
}

// The line under the recorder's source name.
std::string source_detail(const DemoState& state, InputSource source, bool capture) {
    switch (source) {
    case InputSource::Line: return capture ? "Line inputs L and R" : "Audio capture is unavailable";
    case InputSource::Mic: return capture ? "Built-in microphone, recorded mono" : "Audio capture is unavailable";
    case InputSource::Usb: break;
    default: return "The left button above the screen chooses an input";
    }
    switch (state.usb.state()) {
    case UsbStreamState::Missing: return "No USB audio gadget card was found";
    case UsbStreamState::Waiting: return "USB-C: play audio from the computer";
    case UsbStreamState::Failed: return "USB-C audio could not be opened";
    case UsbStreamState::Running: {
        char text[96];
        snprintf(text, sizeof(text), "USB-C: %.1f kHz, %u-bit, %u ch%s", state.usb.rate() / 1000.0, state.usb.bits(),
                 state.usb.channels(), state.usb.rate() == sample_rate ? "" : ", converted to 44.1 kHz");
        return text;
    }
    default: return "USB-C input is starting";
    }
}

std::string recorder_json(const DemoState& state) {
    auto& recorder = state.recorder;
    unsigned route = recorder.route();
    std::ostringstream out;
    out << "{\"source\":" << json_string(input_source_names[state.source.load()])
        << ",\"channels\":" << json_string(input_channel_names[state.channels.load()])
        << ",\"state\":" << json_string(Recorder::state_names[size_t(recorder.state())])
        << ",\"rate\":" << sample_rate << ",\"frames\":" << recorder.length() << ",\"position\":" << recorder.position()
        << ",\"capacity\":" << recorder.capacity() << ",\"takes\":" << recorder.takes() << ",\"refused\":" << recorder.refused()
        << ",\"take_source\":" << json_string(recorder.takes() ? input_source_names[(route >> 4) % input_source_names.size()] : "")
        << ",\"take_channels\":" << json_string(recorder.takes() ? input_channel_names[(route & 15) % input_channel_names.size()] : "")
        << ",\"input_peaks\":[" << state.input_left.load() << ',' << state.input_right.load() << ']'
        << ",\"usb\":" << state.usb.json() << '}';
    return out.str();
}
} // namespace

// The soft keys open the recorder from any page but the light show and pad
// pages, which handle their own; on the recorder page they choose the input
// and channels. Sample Record and Play also open the page.
bool recorder_button(DemoState& state, SurfaceButton button) {
    bool recorder = state.page.load() == Page::Recorder;
    switch (button) {
    case SurfaceButton::TopLeft: {
        if (recorder) {
            // The service thread applies the source; step from one still pending.
            int pending = state.source_request.load();
            auto current = pending >= 0 ? InputSource(pending) : InputSource(state.source.load());
            state.source_request.store(int(next_input_source(current)));
        }
        state.page.store(Page::Recorder); return true;
    }
    case SurfaceButton::TopMiddle:
        if (recorder && input_has_channels(InputSource(state.source.load())))
            state.channels.store(uint8_t(next_input_channels(InputChannels(state.channels.load()))));
        state.page.store(Page::Recorder); return true;
    case SurfaceButton::SampleRecord: state.recorder.record(); state.page.store(Page::Recorder); return true;
    case SurfaceButton::Play: state.recorder.play(); state.page.store(Page::Recorder); return true;
    case SurfaceButton::Stop: state.recorder.stop(); return true;
    default: return false;
    }
}

void RecorderAudio::process(DemoState& state, const AudioBlock& input, std::array<float, block_frames * 2>& playback) {
    auto source = InputSource(state.source.load(std::memory_order_relaxed));
    auto channels = InputChannels(state.channels.load(std::memory_order_relaxed));
    std::array<float, block_frames * 2> recorded {};
    if (source == InputSource::Usb) state.usb.pull(recorded.data(), block_frames);
    else if (source != InputSource::Off) for (unsigned i = 0; i < recorded.size(); ++i) recorded[i] = float(input[i] / full_scale);
    // About 20 dB per 300 ms of fall, so the 75 ms screen refresh sees peaks.
    meter_left *= 0.978f; meter_right *= 0.978f;
    for (unsigned n = 0; n < block_frames; ++n) {
        meter_left = std::max(meter_left, std::abs(recorded[2 * n]));
        meter_right = std::max(meter_right, std::abs(recorded[2 * n + 1]));
    }
    state.input_left.store(meter_left, std::memory_order_relaxed);
    state.input_right.store(meter_right, std::memory_order_relaxed);
    select_input_channels(source, channels, recorded.data(), block_frames);
    state.recorder.process(recorded.data(), playback.data(), block_frames, input_route(source, channels), source != InputSource::Off);
}

void RecorderLights::update(SurfaceLights& lights, const DemoState& state) {
    auto mode = state.recorder.state();
    bool recording = mode == Recorder::State::Recording, playing = mode == Recorder::State::Playing;
    bool source = state.source.load() != uint8_t(InputSource::Off);
    const std::array<std::pair<SurfaceButton, ButtonLight>, 4> wanted {{
        {SurfaceButton::Sample, state.page.load() == Page::Recorder ? ButtonLight::Bright : ButtonLight::Off},
        {SurfaceButton::SampleRecord, recording ? ButtonLight::Bright : source ? ButtonLight::Dim : ButtonLight::Off},
        {SurfaceButton::Play, playing ? ButtonLight::Bright : state.recorder.length() ? ButtonLight::Dim : ButtonLight::Off},
        {SurfaceButton::Stop, recording || playing ? ButtonLight::Dim : ButtonLight::Off},
    }};
    for (unsigned n = 0; n < wanted.size(); ++n)
        if (shown[n] != uint8_t(wanted[n].second) && lights.button(wanted[n].first, wanted[n].second))
            shown[n] = uint8_t(wanted[n].second);
}

void apply_source_request(DemoState& state, Hardware& hardware, const std::string& usb_device) {
    int requested = state.source_request.exchange(-1);
    if (requested >= 0) select_source(state, hardware, InputSource(requested), usb_device);
}

std::string recorder_command(DemoState& state, Hardware& hardware, const std::string& usb_device,
                             const std::string& action, const std::string& argument) {
    if (action == "input") {
        auto source = argument == "off" ? InputSource::Off : argument == "mic" ? InputSource::Mic : argument == "line" ? InputSource::Line : InputSource::Count;
        return hardware.mixer.available() && source != InputSource::Count && select_source(state, hardware, source, usb_device)
            ? "OK input " + argument : "ERR input unavailable; use off, mic or line";
    }
    if (action == "source") {
        auto name = std::find(input_source_names.begin(), input_source_names.end(), argument);
        if (name == input_source_names.end()) return "ERR use source off, line, mic or usb";
        if (!select_source(state, hardware, InputSource(name - input_source_names.begin()), usb_device)) return "ERR input route unavailable; input is off";
        return "OK source " + argument + (hardware.mixer.available() ? "" : " (no I2S mixer; route unchanged)");
    }
    if (action == "channels") {
        auto name = std::find(input_channel_names.begin(), input_channel_names.end(), argument);
        if (name == input_channel_names.end()) return "ERR use channels stereo, left or right";
        state.channels.store(uint8_t(name - input_channel_names.begin())); return "OK channels " + argument;
    }
    if (action == "transport") {
        if (argument == "record") state.recorder.record();
        else if (argument == "play") state.recorder.play();
        else if (argument == "stop") state.recorder.stop();
        else return "ERR use transport record, play or stop";
        return "OK transport " + argument + " requested";
    }
    if (action == "recorder" && argument.empty()) return "OK " + recorder_json(state);
    return {};
}

std::string recorder_status(const DemoState& state) {
    char fields[256];
    snprintf(fields, sizeof(fields), "source=%s channels=%s recorder=%s recorded_frames=%u play_frame=%u takes=%u usb=%s",
             input_source_names[state.source.load()], input_channel_names[state.channels.load()],
             Recorder::state_names[size_t(state.recorder.state())], state.recorder.length(), state.recorder.position(),
             state.recorder.takes(), usb_stream_state_names[size_t(state.usb.state())]);
    return fields;
}

RecorderView RecorderFeed::frame(const DemoState& state, bool capture_active, bool shown) {
    RecorderView view;
    if (shown) {
        const auto& recorder = state.recorder;
        view.source = state.source.load(); view.channels = state.channels.load(); view.state = uint8_t(recorder.state());
        view.frames = recorder.length(); view.position = recorder.position(); view.capacity = recorder.capacity(); view.rate = sample_rate;
        view.left = state.input_left.load(); view.right = state.input_right.load();
        // While recording the waveform spans at least ten seconds, so it grows.
        view.scale = view.state == uint8_t(Recorder::State::Recording) ? std::max(view.frames, 10 * sample_rate) : std::max(view.frames, 1u);
        for (unsigned column = 0; column < RecorderView::columns; ++column) {
            unsigned first = unsigned(uint64_t(column) * view.scale / RecorderView::columns);
            unsigned last = unsigned(uint64_t(column + 1) * view.scale / RecorderView::columns);
            view.waveform[column] = recorder.overview_peak(first, std::max(last, first + 1));
        }
        view.detail = source_detail(state, InputSource(view.source), capture_active);
    }
    auto now = std::chrono::steady_clock::now();
    if (state.recorder.refused() != refusals_seen) { refusals_seen = state.recorder.refused(); hint_until = now + std::chrono::milliseconds(2500); }
    if (now < hint_until) { view.detail = "Choose an input first: left button above the screen"; view.hint = true; }
    return view;
}
