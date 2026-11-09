// SPDX-License-Identifier: AGPL-3.0-only
#include "drivers/jacks.h"
#include <cassert>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

// A fake I2S card: the two jack controls, a queue of control events and a
// way to lose the card. Reads and opens are counted.
static int values[jack_count] = {1, 0};
static bool present[jack_count] = {true, true};
static bool card = true, subscribe = true, lost = false;
static unsigned pending = 0, opens = 0, closes = 0, reads = 0;

extern "C" int __wrap_snd_ctl_open(snd_ctl_t** ctl, const char* name, int mode) {
    assert(mode == SND_CTL_NONBLOCK);
    if (strcmp(name, "hw:I2S") || !card) return -ENODEV;
    ++opens; lost = false; *ctl = reinterpret_cast<snd_ctl_t*>(1); return 0;
}
extern "C" int __wrap_snd_ctl_close(snd_ctl_t*) { ++closes; return 0; }
extern "C" int __wrap_snd_ctl_subscribe_events(snd_ctl_t*, int on) { assert(on == 1); return subscribe ? 0 : -ENOSYS; }
extern "C" int __wrap_snd_ctl_read(snd_ctl_t*, snd_ctl_event_t*) {
    if (lost) return -ENODEV;
    if (!pending) return -EAGAIN;
    --pending; return 1;
}
extern "C" int __wrap_snd_ctl_elem_read(snd_ctl_t*, snd_ctl_elem_value_t* value) {
    assert(!lost);
    ++reads;
    if (snd_ctl_elem_value_get_interface(value) != SND_CTL_ELEM_IFACE_CARD) return -ENOENT;
    for (unsigned n = 0; n < jack_count; ++n) {
        if (strcmp(snd_ctl_elem_value_get_name(value), jack_info[n].control) || !present[n]) continue;
        snd_ctl_elem_value_set_boolean(value, 0, values[n]);
        return 0;
    }
    return -ENOENT;
}

static std::vector<JackEvent> heard;
static unsigned poll(JackDetect& jacks) {
    heard.clear();
    return jacks.poll([](const JackEvent& event) { heard.push_back(event); });
}
static void wait_a_second() { std::this_thread::sleep_for(std::chrono::milliseconds(1050)); }

int main() {
    {
        card = false;
        JackDetect missing; // No I2S card: nothing to report, unknown states.
        assert(!missing.available() && poll(missing) == 0 && missing.state(Jack::Headphones) == -1);
        assert(missing.json() == "{\"available\":false,\"events\":false,\"jacks\":{\"headphones\":{\"control\":\"Headphones Jack\","
               "\"plugged\":-1,\"changes\":0,\"timestamp_ns\":0},\"line_out\":{\"control\":\"Line Out Jack\",\"plugged\":-1,"
               "\"changes\":0,\"timestamp_ns\":0}}}");
        JackDetect other("hw:Other"); assert(!other.available());
        card = true;
    }
    JackDetect jacks;
    assert(jacks.available() && opens == 1 && reads == 2);
    // Both states arrive once, as initial, in jack order.
    assert(jacks.state(Jack::Headphones) == -1); // Not reported yet.
    assert(poll(jacks) == 2 && heard.size() == 2);
    assert(heard[0].jack == Jack::Headphones && heard[0].plugged && heard[0].initial && heard[0].timestamp_ns > 0);
    assert(heard[1].jack == Jack::LineOut && !heard[1].plugged && heard[1].initial);
    assert(jacks.state(Jack::Headphones) == 1 && jacks.state(Jack::LineOut) == 0 && jacks.changes(Jack::Headphones) == 0);

    // Quiet jacks are not read again.
    for (unsigned n = 0; n < 10; ++n) assert(poll(jacks) == 0);
    assert(reads == 2);

    // A plug is one event and one report.
    values[1] = 1; pending = 1;
    assert(poll(jacks) == 1 && heard[0].jack == Jack::LineOut && heard[0].plugged && !heard[0].initial);
    assert(jacks.changes(Jack::LineOut) == 1 && jacks.state(Jack::LineOut) == 1 && reads == 4);
    // An unplug, with the kernel's events for it and a mixer write queued together.
    values[0] = 0; pending = 3;
    assert(poll(jacks) == 1 && heard[0].jack == Jack::Headphones && !heard[0].plugged && !heard[0].initial);
    assert(pending == 0 && reads == 6 && jacks.changes(Jack::Headphones) == 1);
    // An event that changes no jack (say a route switched) reports nothing.
    pending = 1;
    assert(poll(jacks) == 0 && reads == 8);
    auto json = jacks.json();
    assert(json.find("{\"available\":true,\"events\":true,\"jacks\":{\"headphones\":{\"control\":\"Headphones Jack\",\"plugged\":0,\"changes\":1,") == 0);
    assert(json.find("\"line_out\":{\"control\":\"Line Out Jack\",\"plugged\":1,\"changes\":1,") != std::string::npos);

    // A control that disappears is unknown without a report; when it comes
    // back its state is initial again, not a plug.
    present[1] = false; pending = 1;
    assert(poll(jacks) == 0 && jacks.state(Jack::LineOut) == -1);
    present[1] = true; pending = 1;
    assert(poll(jacks) == 1 && heard[0].jack == Jack::LineOut && heard[0].plugged && heard[0].initial);
    assert(jacks.changes(Jack::LineOut) == 1);

    // A lost card closes, reads unknown and is reopened a second later. This
    // time it refuses events, so the jacks are re-read once a second instead.
    lost = true; subscribe = false;
    assert(poll(jacks) == 0 && closes == 1 && !jacks.available());
    assert(jacks.state(Jack::Headphones) == -1 && jacks.state(Jack::LineOut) == -1);
    assert(poll(jacks) == 0 && opens == 1);
    wait_a_second();
    values[0] = 1;
    assert(poll(jacks) == 2 && opens == 2 && heard[0].initial && heard[0].plugged && heard[1].initial);
    assert(jacks.json().find("\"available\":true,\"events\":false") != std::string::npos);
    values[0] = 0; reads = 0;
    assert(poll(jacks) == 0 && reads == 0); // Not yet: the next read is a second away.
    wait_a_second();
    assert(poll(jacks) == 1 && reads == 2 && heard[0].jack == Jack::Headphones && !heard[0].plugged && !heard[0].initial);
    assert(jacks.changes(Jack::Headphones) == 2);
}
