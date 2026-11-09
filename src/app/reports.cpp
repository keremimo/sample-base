// SPDX-License-Identifier: AGPL-3.0-only
#include "app/reports.h"
#include "platform.h"
#include "app/light_show.h"
#include <cstdio>

std::string status_line(const AppState& state) {
    int last = state.last_pad.load();
    auto touch = last >= 0 ? state.pads.touch(unsigned(last)) : PadTouch {};
    char reply[1536];
    snprintf(reply, sizeof(reply), "OK frames=%llu sounding_blocks=%llu hardware_events=%u external_events=%u synthetic_events=%u "
             "held=%04x last_pad=%d velocity=%d pressure=%u peak_pressure=%u last_midi=%06x peak=%.6f input_peak=%.6f capture_frames=%llu capture_active=%u "
             "xruns=%u capture_xruns=%u queue_overruns=%u clocks=%u gain=%.2f monitor=%.2f push_exit=%u "
             "source=%s channels=%s recorder=%s recorded_frames=%u play_frame=%u takes=%u usb=%s page=%s light_scene=%u",
             static_cast<unsigned long long>(state.audio.frames.load()), static_cast<unsigned long long>(state.audio.sounding_blocks.load()),
             state.hardware_events.load(), state.external_events.load(), state.synthetic_events.load(), state.held.load(), last + 1,
             state.velocity.load(), unsigned(touch.pressure), unsigned(touch.peak), state.last_midi.load(), double(state.audio.peak.load()), double(state.audio.input_peak.load()),
             static_cast<unsigned long long>(state.audio.capture_frames.load()), unsigned(state.audio.capture_active.load()),
             state.audio.xruns.load(), state.audio.capture_xruns.load(), state.overruns.load(), state.clocks.load(), double(state.gain.load()), double(state.monitor.load()), unsigned(state.push_exit.load()),
             input_source_names[state.source.load()], input_channel_names[state.channels.load()],
             Recorder::state_names[size_t(state.recorder.state())], state.recorder.length(), state.recorder.position(),
             state.recorder.takes(), usb_input_state_names[size_t(state.usb.state())], page_names[size_t(state.page.load())],
             state.page.load() == Page::Lights ? light_scene_at(state.light_position.load()) + 1 : 0u);
    return reply;
}

std::string recorder_json(const AppState& state) {
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

std::string surface_json(const SurfaceState& state) {
    std::ostringstream out; out << "{\"timestamp_ns\":" << state.last.timestamp_ns << ",\"pitch\":" << state.pitch << ",\"channel_pressure\":" << unsigned(state.channel_pressure);
    for (const auto& pair : {std::make_pair("notes", &state.notes), std::make_pair("pressure", &state.pressure), std::make_pair("controls", &state.controls)}) {
        out << ',' << json_string(pair.first) << ":[";
        for (unsigned i = 0; i < pair.second->size(); ++i) { if (i) out << ','; out << unsigned((*pair.second)[i]); }
        out << ']';
    }
    out << ",\"last\":[";
    for (unsigned i = 0; i < state.last.size; ++i) { if (i) out << ','; out << unsigned(state.last.bytes[i]); }
    return out.str() + "]}";
}
