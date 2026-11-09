// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include "drivers/usb_audio.h"
#include "runtime/app.h"
#include "runtime/session.h"
#include "surface/midi.h"
#include <cstdint>
#include <sstream>
#include <string>

// Replies to the runtime's read-only control commands. Field names are part
// of the control protocol that device.py, tools/ and tests/ parse; keep them
// stable.

// `status`: one "OK key=value ..." line, also printed when the app exits:
// the runtime's fields, then App::status().
std::string status_line(const Session& session, const App& app, const UsbAudioOutput& usb_output);
// `surface`: the latest value of every surface note, pressure and control.
std::string surface_json(const SurfaceState& state);

// `surface-log`: raw surface packets after sequence `since`, at most 256.
// Entries are [sequence, timestamp_ns, size, [bytes]]. `next` is the cursor
// for the following request; a first sequence above since+1 means lost history.
template <unsigned Size> std::string surface_log_json(const SurfaceLog<Size>& log, uint64_t since, unsigned overflows) {
    std::ostringstream out; out << "{\"oldest\":" << log.oldest() << ",\"newest\":" << log.newest()
        << ",\"diagnostic_overflows\":" << overflows << ",\"packets\":[";
    bool first = true;
    auto next = log.read(since, 256, [&](const typename SurfaceLog<Size>::Entry& entry) {
        if (!first) out << ',';
        first = false;
        out << '[' << entry.sequence << ',' << entry.timestamp_ns << ',' << entry.size << ",[";
        for (unsigned n = 0; n < entry.size && n < entry.bytes.size(); ++n) { if (n) out << ','; out << unsigned(entry.bytes[n]); }
        out << "]]";
    });
    out << "],\"next\":" << next << '}';
    return out.str();
}
