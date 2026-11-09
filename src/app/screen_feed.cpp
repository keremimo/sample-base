// SPDX-License-Identifier: AGPL-3.0-only
#include "app/screen_feed.h"
#include "controls.h"
#include <algorithm>
#include <cstdio>
#include <mutex>

namespace {
// The line under the recorder's source name.
std::string source_detail(const AppState& state, InputSource source) {
    bool capture = state.audio.capture_active.load();
    switch (source) {
    case InputSource::Line: return capture ? "Line inputs L and R" : "Audio capture is unavailable";
    case InputSource::Mic: return capture ? "Built-in microphone, recorded mono" : "Audio capture is unavailable";
    case InputSource::Usb: break;
    default: return "The left button above the screen chooses an input";
    }
    switch (state.usb.state()) {
    case UsbInputState::Missing: return "No USB audio gadget card was found";
    case UsbInputState::Waiting: return "USB-C: play audio from the computer";
    case UsbInputState::Failed: return "USB-C audio could not be opened";
    case UsbInputState::Running: {
        char text[96];
        snprintf(text, sizeof(text), "USB-C: %.1f kHz, %u-bit, %u ch%s", state.usb.rate() / 1000.0, state.usb.bits(),
                 state.usb.channels(), state.usb.rate() == sample_rate ? "" : ", converted to 44.1 kHz");
        return text;
    }
    default: return "USB-C input is starting";
    }
}
} // namespace

RecorderView recorder_view(const AppState& state) {
    RecorderView view;
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
    view.detail = source_detail(state, InputSource(view.source));
    return view;
}

ScreenState ScreenFeed::frame(AppState& state) {
    ScreenState view;
    view.held = state.held.load(); view.last_pad = state.last_pad.load(); view.velocity = state.velocity.load();
    view.peak = state.audio.peak.load(); view.xruns = state.audio.xruns.load();
    view.last_input = describe_last_input(state.last_input.load()); view.page = state.page.load();
    view.light_position = state.light_position.load();
    { std::lock_guard<std::mutex> lock(state.prompt_lock); view.prompt = state.prompt; }
    if (view.page == Page::Recorder) view.recorder = recorder_view(state);

    // The pressure trace moves on every frame, so it is current when the pad page opens.
    std::rotate(trace.begin(), trace.begin() + 1, trace.end());
    std::rotate(strike_marks.begin(), strike_marks.begin() + 1, strike_marks.end());
    unsigned strikes = state.pads.strikes();
    trace.back() = state.pads.take_recent();
    strike_marks.back() = strikes != strikes_seen ? uint8_t(view.velocity) : 0;
    strikes_seen = strikes;
    if (view.page == Page::Pads) {
        for (unsigned pad = 0; pad < PadTouches::pads; ++pad) {
            auto touch = state.pads.touch(pad);
            view.pads.velocity[pad] = touch.velocity; view.pads.pressure[pad] = touch.pressure; view.pads.peak[pad] = touch.peak;
        }
        view.pads.trace = trace; view.pads.strikes = strike_marks;
    }
    auto now = std::chrono::steady_clock::now();
    if (state.recorder.refused() != refusals_seen) { refusals_seen = state.recorder.refused(); hint_until = now + std::chrono::milliseconds(2500); }
    if (now < hint_until) { view.recorder.detail = "Choose an input first: left button above the screen"; view.recorder.hint = true; }
    return view;
}
