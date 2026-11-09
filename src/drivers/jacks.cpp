// SPDX-License-Identifier: AGPL-3.0-only
#include "drivers/jacks.h"
#include "drivers/platform.h"
#include <cerrno>
#include <chrono>
#include <sstream>
#include <utility>

namespace {
constexpr uint64_t second_ns = 1000000000ull;
uint64_t monotonic_ns() {
    return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}
} // namespace

JackDetect::JackDetect(std::string card) : card(std::move(card)) { open_card(); }
JackDetect::~JackDetect() { close_card(); }

void JackDetect::open_card() {
    retry_ns = monotonic_ns() + second_ns;
    if (snd_ctl_open(&ctl, card.c_str(), SND_CTL_NONBLOCK) < 0) { ctl = nullptr; return; }
    events = snd_ctl_subscribe_events(ctl, 1) >= 0;
    read_values();
}

void JackDetect::close_card() {
    if (ctl) snd_ctl_close(ctl);
    ctl = nullptr; events = false;
    for (auto& jack : jacks) jack.value = -1;
}

// By name rather than numid, so a control that went away and came back is
// found again. The kernel's jack controls are card controls, not mixer ones.
void JackDetect::read_values() {
    snd_ctl_elem_id_t* id; snd_ctl_elem_id_alloca(&id);
    snd_ctl_elem_value_t* value; snd_ctl_elem_value_alloca(&value);
    for (unsigned n = 0; n < jack_count; ++n) {
        snd_ctl_elem_id_clear(id);
        snd_ctl_elem_id_set_interface(id, SND_CTL_ELEM_IFACE_CARD);
        snd_ctl_elem_id_set_name(id, jack_info[n].control);
        snd_ctl_elem_value_set_id(value, id);
        jacks[n].value = snd_ctl_elem_read(ctl, value) < 0 ? -1 : snd_ctl_elem_value_get_boolean(value, 0) ? 1 : 0;
    }
}

unsigned JackDetect::poll(const Report& report) {
    auto now = monotonic_ns();
    if (!ctl && now >= retry_ns) open_card();
    if (ctl) {
        // Any event on the card (a jack, a control added or removed, or
        // a mixer write) re-reads both jacks: two reads, and nothing missed
        // when the kernel merges events that queued up between polls.
        bool queued = false;
        snd_ctl_event_t* event; snd_ctl_event_alloca(&event);
        for (unsigned n = 0; n < 64; ++n) {
            int result = snd_ctl_read(ctl, event);
            if (result == 0 || result == -EAGAIN || result == -EINTR) break;
            if (result < 0) { close_card(); retry_ns = now + second_ns; break; } // The card went away.
            queued = true;
        }
        if (ctl && !events && now >= retry_ns) { queued = true; retry_ns = now + second_ns; }
        if (ctl && queued) read_values();
    }
    unsigned count = 0;
    for (unsigned n = 0; n < jack_count; ++n) {
        auto& jack = jacks[n];
        if (jack.value == jack.reported) continue;
        bool initial = jack.reported < 0;
        jack.reported = jack.value;
        if (jack.value < 0) continue; // Unknown until the control returns; then it is initial again.
        if (!initial) ++jack.changes;
        jack.timestamp_ns = now;
        report({Jack(n), jack.value == 1, initial, now});
        ++count;
    }
    return count;
}

std::string JackDetect::json() const {
    std::ostringstream out;
    out << "{\"available\":" << (ctl ? "true" : "false") << ",\"events\":" << (events ? "true" : "false") << ",\"jacks\":{";
    for (unsigned n = 0; n < jack_count; ++n) {
        const auto& jack = jacks[n];
        if (n) out << ',';
        out << json_string(jack_info[n].key) << ":{\"control\":" << json_string(jack_info[n].control)
            << ",\"plugged\":" << jack.reported << ",\"changes\":" << jack.changes << ",\"timestamp_ns\":" << jack.timestamp_ns << '}';
    }
    return out.str() + "}}";
}
