// SPDX-License-Identifier: AGPL-3.0-only
// The control protocol: Controller::command() answers each text command sent
// through ControlServer (`sample-base --ctl ...`). It runs on the service
// thread; anything the MIDI thread must do is queued for it. docs/HARDWARE.md
// lists the commands, and tests/test_runtime.py exercises them.
#include "app/controller.h"
#include "drivers/audio.h"
#include "surface/controls.h"
#include "app/light_show.h"
#include "app/reports.h"
#include "app/surface_events.h"
#include "app/util.h"
#include <algorithm>
#include <cctype>
#include <cstring>
#include <iterator>
#include <sstream>

std::string Controller::command(AppState& state, const std::string& text) {
    std::istringstream input(text); std::string action, argument;
    input >> action;
    std::getline(input >> std::ws, argument);

    // Session
    if (action == "status" && argument.empty()) return status_line(state);
    if (action == "stock" && argument.empty()) { state.running.store(false); return "OK returning to stock app"; }
    if (action == "screenshot" && argument.empty()) { state.screenshot.store(true); return "OK screenshot requested"; }
    // Calibration needs every control, including wheel Push. Remote stop,
    // the session deadline and crash recovery still return to stock.
    if (action == "push-exit" && (argument == "on" || argument == "off")) {
        state.push_exit.store(argument == "on"); return "OK push-exit " + argument;
    }
    if (action == "prompt") {
        if (argument.size() > 400 || std::any_of(argument.begin(), argument.end(), [](char c) { return static_cast<unsigned char>(c) < 0x20; }))
            return "ERR prompt must be printable text up to 400 bytes";
        std::lock_guard<std::mutex> lock(state.prompt_lock); state.prompt = argument;
        state.prompting.store(!argument.empty());
        return argument.empty() ? "OK prompt cleared" : "OK prompt shown";
    }

    // Synth, pages and the recorder
    if (action == "audition" && argument.empty()) {
        audition_requested.store(true); return "OK audition C4";
    }
    if (action == "gain" || action == "monitor") {
        unsigned value = 0;
        if (!parse_unsigned(argument, value) || value > 100) return "ERR use a percentage from 0 to 100";
        (action == "gain" ? state.gain : state.monitor).store(value / 100.0f); return "OK " + action + ' ' + argument;
    }
    if (action == "page") {
        auto name = std::find(page_names.begin(), page_names.end(), argument);
        if (name == page_names.end()) return "ERR use page synth, recorder, lights or pads";
        state.page.store(Page(name - page_names.begin())); return "OK page " + argument;
    }
    if (action == "lights") return lights_command(state, argument);
    if (action == "input") {
        auto source = argument == "off" ? InputSource::Off : argument == "mic" ? InputSource::Mic : argument == "line" ? InputSource::Line : InputSource::Count;
        return mixer.available() && source != InputSource::Count && select_source(state, source) ? "OK input " + argument : "ERR input unavailable; use off, mic or line";
    }
    if (action == "source") {
        auto name = std::find(input_source_names.begin(), input_source_names.end(), argument);
        if (name == input_source_names.end()) return "ERR use source off, line, mic or usb";
        if (!select_source(state, InputSource(name - input_source_names.begin()))) return "ERR input route unavailable; input is off";
        return "OK source " + argument + (mixer.available() ? "" : " (no I2S mixer; route unchanged)");
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

    // Control surface and MIDI
    if (action == "surface" && argument.empty()) return "OK {\"diagnostic_overflows\":" + std::to_string(surface_overflows) + ",\"state\":" + surface_json(surface) + '}';
    if (action == "surface-log") {
        uint64_t since = 0;
        if (!argument.empty() && !parse_unsigned(argument, since)) return "ERR use surface-log [SEQUENCE]";
        return "OK " + surface_log_json(surface_log, since, surface_overflows);
    }
    if (action == "surface-sync" && argument.empty()) {
        sync_requested.store(true); return "OK surface position request queued";
    }
    if (action == "led") return led(argument);
    if (action == "midi" && argument.empty()) return "OK {\"async_send_errors\":" + std::to_string(state.midi_send_errors.load()) + ",\"state\":" + catalog.json + '}';
    if (action == "midi-send") return midi_send_command(argument);

    // Platform
    if (action == "hardware" && argument.empty()) return "OK {\"platform\":" + platform.json() + ",\"midi\":" + catalog.json +
        ",\"mixer\":" + mixer.json() + ",\"audio\":" + AudioDevice::devices_json() + '}';
    if (action == "mixer" && argument.empty()) return "OK " + mixer.json();
    if (action == "mixer-set") return mixer_set_command(argument);
    if (action == "brightness") {
        unsigned value = 0;
        return parse_unsigned(argument, value) && platform.brightness(value) ? "OK brightness " + argument : "ERR brightness unavailable or outside device range";
    }
    if (action == "clock" && argument.empty()) return "OK " + clock.json();
    if (action == "clock-in" && (argument == "on" || argument == "off"))
        return clock.input(argument == "on") ? "OK sync input updated" : "ERR sync input unavailable";
    if (action == "clock-out" && (argument == "0" || argument == "1"))
        return clock.output(argument == "1") ? "OK sync output updated" : "ERR sync output unavailable";
    if (action == "power" && argument.empty()) return "OK " + power.json();
    if (action == "storage" && argument.empty()) return "OK {\"generation\":" + std::to_string(services.storage_generation()) + ",\"state\":" + services.storage_json() + '}';
    if (action == "storage-mount" || action == "storage-unmount")
        return services.mount(argument, action == "storage-mount") ? "OK storage request completed" : "ERR storage service request failed";

    return "ERR use status, hardware, surface, surface-log, surface-sync, push-exit, prompt, power, led, midi, midi-send, mixer, mixer-set, input, "
           "source, channels, transport, page, lights, recorder, gain, monitor, brightness, storage, storage-mount, storage-unmount, audition, screenshot, clock, clock-in, clock-out or stock";
}

// lights 1-N | lights next | lights previous: opens the light show at a scene.
std::string Controller::lights_command(AppState& state, const std::string& argument) {
    int count = int(light_scenes.size()), scene = -1;
    unsigned number = 0;
    if (argument == "next" || argument == "previous") scene = next_light_scene(state, argument == "next" ? 1 : -1);
    else if (parse_unsigned(argument, number) && number >= 1 && number <= light_scenes.size()) scene = int(number) - 1;
    else return "ERR use lights 1-" + std::to_string(count) + ", next or previous";
    state.page.store(Page::Lights); state.scene_request.store(scene);
    return "OK lights scene " + std::to_string(scene + 1) + ' ' + light_scenes[size_t(scene)].name;
}

// midi-send ADDRESS HEX...: queues raw MIDI for an output port the last scan found.
std::string Controller::midi_send_command(const std::string& argument) {
    std::istringstream fields(argument); std::string address, bytes;
    fields >> address; std::getline(fields >> std::ws, bytes); MidiPacket packet;
    if (!parse_midi_hex(bytes, packet) || address.size() >= MidiSendRequest{}.address.size() ||
        std::find(catalog.outputs.begin(), catalog.outputs.end(), address) == catalog.outputs.end()) return "ERR invalid message or unavailable port";
    MidiSendRequest request; strcpy(request.address.data(), address.c_str()); request.packet = packet;
    return midi_output.push(request) ? "OK MIDI queued" : "ERR full output queue";
}

// mixer-set ID VALUE...: writes one ALSA mixer control within its range.
std::string Controller::mixer_set_command(const std::string& argument) {
    std::istringstream fields(argument); std::string id;
    fields >> id; std::vector<long> values; long item;
    while (fields >> item && values.size() < 128) values.push_back(item);
    if (!fields.eof()) return "ERR invalid mixer values";
    unsigned value = 0;
    return parse_unsigned(id, value) && mixer.set(value, values) ? "OK mixer updated" : "ERR unavailable control, wrong value count or range";
}

// led pad 1-16 R G B | led fader R G B | led bank A-H on|off |
// led button NAME off|dim|bright|red-dim|red-bright | led off
std::string Controller::led(const std::string& text) {
    std::istringstream fields(text); std::string target, name, extra;
    fields >> target;
    auto colour = [&](Rgb& rgb) {
        unsigned r, g, b;
        if (!(fields >> r >> g >> b) || r > 127 || g > 127 || b > 127) return false;
        rgb = {uint8_t(r), uint8_t(g), uint8_t(b)}; return true;
    };
    auto done = [&](bool ok) { return ok && !(fields >> extra) ? "OK lights updated" : "ERR use led pad 1-16 R G B, led fader R G B, "
        "led bank A-H on|off, led button NAME off|dim|bright|red-dim|red-bright or led off; colours are 0-127"; };
    Rgb rgb;
    if (target == "off") { lights.all_off(); return done(true); }
    if (target == "fader") return done(colour(rgb) && lights.fader(rgb));
    if (target == "pad") { unsigned pad = 0; return done(fields >> pad && pad >= 1 && colour(rgb) && lights.pad(pad - 1, rgb)); }
    fields >> name;
    if (target == "bank") {
        bool on = false, valid = name.size() == 1 && name[0] >= 'A' && name[0] <= 'H' && fields >> extra && (extra == "on" || extra == "off");
        on = extra == "on"; extra.clear();
        return done(valid && lights.bank(unsigned(name[0] - 'A'), on));
    }
    if (target != "button") return done(false);
    std::string level; fields >> level;
    const char* levels[] {"off", "dim", "bright", "red-dim", "red-bright"};
    auto light = std::find(std::begin(levels), std::end(levels), level);
    // Button names match ignoring case and punctuation: "sample-select" is Sample Select.
    auto key = [](const std::string& text) {
        std::string result;
        for (char c : text) if (std::isalnum(static_cast<unsigned char>(c))) result += char(std::tolower(static_cast<unsigned char>(c)));
        return result;
    };
    for (unsigned n = 0; n < surface_buttons.size(); ++n)
        if (key(surface_buttons[n].name) == key(name) && light != std::end(levels))
            return done(lights.button(SurfaceButton(n), ButtonLight(light - std::begin(levels))));
    return done(false);
}
