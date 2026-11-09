// SPDX-License-Identifier: AGPL-3.0-only
#include "runtime/reports.h"
#include "drivers/platform.h"
#include <cstdio>

std::string status_line(const Session& session, const App& app, const UsbAudioOutput& usb_output) {
    const auto& audio = session.audio;
    char reply[768];
    snprintf(reply, sizeof(reply), "OK frames=%llu sounding_blocks=%llu hardware_events=%u external_events=%u synthetic_events=%u "
             "held=%04x last_pad=%d velocity=%d last_midi=%06x peak=%.6f input_peak=%.6f capture_frames=%llu capture_active=%u "
             "xruns=%u capture_xruns=%u clocks=%u headphones=%d line_out=%d push_exit=%u usb_output=%s",
             static_cast<unsigned long long>(audio.frames.load()), static_cast<unsigned long long>(audio.sounding_blocks.load()),
             session.hardware_events.load(), session.external_events.load(), session.synthetic_events.load(), session.held.load(),
             session.last_pad.load() + 1, session.velocity.load(), session.last_midi.load(), double(audio.peak.load()),
             double(audio.input_peak.load()), static_cast<unsigned long long>(audio.capture_frames.load()),
             unsigned(audio.capture_active.load()), audio.xruns.load(), audio.capture_xruns.load(), session.clocks.load(),
             session.plugged(Jack::Headphones), session.plugged(Jack::LineOut), unsigned(session.push_exit.load()), usb_stream_state_names[size_t(usb_output.state())]);
    auto fields = app.status();
    return fields.empty() ? std::string(reply) : std::string(reply) + ' ' + fields;
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
