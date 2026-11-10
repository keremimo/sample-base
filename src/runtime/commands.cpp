// SPDX-License-Identifier: AGPL-3.0-only
// The control protocol: Controller::command() answers each text command sent
// through ControlServer (`sample-base --ctl ...`). It runs on the service
// thread; anything the MIDI thread must do is queued for it. The runtime's
// commands come first; anything else goes to App::command(). docs/HARDWARE.md
// lists the commands, and tests/test_runtime.py exercises them.
#include "runtime/controller.h"
#include "drivers/audio.h"
#include "runtime/reports.h"
#include "runtime/util.h"
#include "surface/controls.h"
#include <algorithm>
#include <cctype>
#include <cstring>
#include <iterator>
#include <sstream>

std::string Controller::command(Session& session, const std::string& text) {
    std::istringstream input(text); std::string action, argument;
    input >> action;
    std::getline(input >> std::ws, argument);

    // Session
    if (action == "status" && argument.empty()) return status_line(session, app, usb_output);
    if (action == "stock" && argument.empty()) { session.running.store(false); return "OK returning to stock app"; }
    if (action == "screenshot" && argument.empty()) { session.screenshot.store(true); return "OK screenshot requested"; }
    // Calibration needs every control, including wheel Push. Remote stop,
    // the session deadline and crash recovery still return to stock.
    if (action == "push-exit" && (argument == "on" || argument == "off")) {
        session.push_exit.store(argument == "on"); return "OK push-exit " + argument;
    }
    if (action == "prompt") {
        if (argument.size() > 400 || std::any_of(argument.begin(), argument.end(), [](char c) { return static_cast<unsigned char>(c) < 0x20; }))
            return "ERR prompt must be printable text up to 400 bytes";
        std::lock_guard<std::mutex> lock(session.prompt_lock); session.prompt = argument;
        session.prompting.store(!argument.empty());
        return argument.empty() ? "OK prompt cleared" : "OK prompt shown";
    }
    if (action == "audition" && argument.empty()) {
        audition_requested.store(true); return "OK audition pad 1";
    }

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
    if (action == "midi" && argument.empty()) return "OK {\"async_send_errors\":" + std::to_string(session.midi_send_errors.load()) + ",\"state\":" + catalog.json + '}';
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
    if (action == "jacks" && argument.empty()) return "OK " + jacks.json();
    if (action == "usb-output") return usb_output_command(argument);
    if (action == "storage" && argument.empty()) return "OK {\"generation\":" + std::to_string(services.storage_generation()) + ",\"state\":" + services.storage_json() + '}';
    if (action == "card-share") return card_share_command(session, argument);
    if (action == "storage-mount" || action == "storage-unmount")
        return services.mount(argument, action == "storage-mount") ? "OK storage request completed" : "ERR storage service request failed";

    // The app's own commands
    auto reply = app.command(action, argument, hardware);
    if (!reply.empty()) return reply;
    auto names = app.command_names();
    return "ERR use status, hardware, surface, surface-log, surface-sync, push-exit, prompt, power, jacks, led, midi, midi-send, mixer, mixer-set, "
           "brightness, storage, storage-mount, storage-unmount, card-share, usb-output, audition, screenshot, clock, clock-in, clock-out or stock" +
           (names.empty() ? std::string() : "; app: " + names);
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

// usb-output [on|off]: what the computer records over USB-C. `on` restarts
// with the PCM given to --usb-output, or finds the gadget when that was off.
std::string Controller::usb_output_command(const std::string& argument) {
    if (argument.empty()) return "OK " + usb_output.json();
    if (argument == "off") { usb_output.stop(); return "OK usb-output off"; }
    if (argument != "on") return "ERR use usb-output [on|off]";
    auto device = usb_output.requested();
    usb_output.start(device.empty() ? "auto" : device);
    return "OK usb-output on";
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
// A light the app also drives changes back on its next frame.
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

// card-share [on|off|force]: the SD card lent to a computer over USB-C and
// taken back (drivers/card_share.h). The app hears of it through Session.
std::string Controller::card_share_command(Session& session, const std::string& argument) {
    if (argument == "on") {
        if (!card.lend()) return "ERR " + card.status().message;
    } else if (argument == "off" || argument == "force") {
        if (card.status().state != CardShareState::Lent) return "ERR the card is not lent";
        card.take_back(argument == "force");
    } else if (!argument.empty()) return "ERR use card-share [on|off|force]";
    if (!argument.empty()) card.poll();
    follow_card(session);
    if (card.status().locked && argument == "off") return "ERR " + card.status().message;
    return "OK " + card.json();
}
