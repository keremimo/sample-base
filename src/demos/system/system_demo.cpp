// SPDX-License-Identifier: AGPL-3.0-only
#include "demos/system/system_demo.h"
#include "drivers/jacks.h"
#include "drivers/platform.h"
#include "drivers/power_button.h"
#include "drivers/usb_audio.h"
#include <algorithm>
#include <iomanip>
#include <sstream>

using namespace sysinfo;

void SystemReader::service(DemoState& state, Hardware& hardware, Clock::time_point now) {
    if (state.page.load() != Page::System) { open = false; return; }
    if (!open) {
        open = true; cpu.reset(); history = {};
        next_read = next_storage = now;
    }
    if (now < next_read) return;
    next_read = now + std::chrono::milliseconds(read_ms);

    Readings readings;
    auto& platform = hardware.platform;
    readings.cpu = cpu.sample(platform.cpu_times());
    if (!readings.cpu.empty()) history.push(readings.cpu.front());
    readings.history = history;
    auto memory = platform.memory();
    readings.memory_total_kb = memory.total_kb; readings.memory_available_kb = memory.available_kb;
    auto zones = platform.temperatures();
    readings.cpu_millidegrees = zone_temperature(zones, "cpu");
    readings.gpu_millidegrees = zone_temperature(zones, "gpu");
    auto supplies = platform.supplies();
    if (auto* supply = main_supply(supplies)) {
        readings.supply_name = supply->name; readings.supply_type = supply->type;
        readings.supply_online = supply->online; readings.supply_microvolts = supply->microvolts;
    }
    readings.key_available = hardware.power.available();
    readings.key_down = hardware.power.pressed();
    readings.key_presses = hardware.power.presses();
    auto backlight = platform.backlight();
    readings.backlight = backlight.brightness; readings.backlight_maximum = backlight.maximum;

    readings.udc_state = platform.usb_state();
    readings.usb_in = state.usb.stats();
    readings.usb_out = hardware.usb_output.stats();

    for (const auto& port : hardware.midi.ports) {
        if (readings.ports.size() == max_ports) break;
        readings.ports.push_back({port.name, port.input, port.output, port.surface});
    }
    readings.send_drops = hardware.midi.send_drops; readings.input_errors = hardware.midi.input_errors;
    readings.headphones = hardware.jacks.state(Jack::Headphones);
    readings.line_out = hardware.jacks.state(Jack::LineOut);

    if (now >= next_storage) {
        next_storage = now + std::chrono::milliseconds(storage_ms);
        storage.clear();
        for (const auto& space : platform.storage()) {
            if (storage.size() == max_mounts) break;
            storage.push_back({space.path, space.type, space.read_only, space.free_bytes, space.total_bytes});
        }
    }
    readings.storage = storage;
    readings.reads = ++reads;

    std::lock_guard<std::mutex> hold(state.system.lock);
    state.system.readings = std::move(readings);
}

bool system_button(DemoState& state, SurfaceButton button) {
    if (button != SurfaceButton::TopLeft && button != SurfaceButton::TopMiddle) return false;
    state.system.panel.store(step_panel(state.system.panel.load(), button == SurfaceButton::TopLeft ? -1 : 1));
    return true;
}

bool system_wheel(DemoState& state, const SurfaceEvent& event) {
    if (state.page.load() != Page::System || !is_control(event.input, SurfaceControl::Wheel) || !event.input.delta) return false;
    state.system.panel.store(step_panel(state.system.panel.load(), event.input.delta > 0 ? 1 : -1));
    return true;
}

void SystemLights::update(SurfaceLights& lights) {
    if (showing) return;
    showing = true;
    lights.button(SurfaceButton::KnobFx, ButtonLight::Bright);
}

void SystemLights::close(SurfaceLights& lights) {
    if (!showing) return;
    showing = false;
    lights.button(SurfaceButton::KnobFx, ButtonLight::Off);
}

namespace {
std::string usb_json(const UsbStreamStats& stats) {
    std::ostringstream out;
    out << "{\"state\":" << json_string(usb_stream_state_names[size_t(stats.state)]) << ",\"rate\":" << stats.rate
        << ",\"trim_ppm\":" << stats.trim_ppm << ",\"underruns\":" << stats.underruns << ",\"xruns\":" << stats.xruns << '}';
    return out.str();
}

std::string optional_json(std::optional<long> value) { return value ? std::to_string(*value) : "null"; }

std::string readings_json(const Readings& readings, Panel panel) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(3);
    out << "{\"panel\":" << json_string(panel_names[size_t(panel)]) << ",\"reads\":" << readings.reads << ",\"cpu\":[";
    for (size_t n = 0; n < readings.cpu.size(); ++n) out << (n ? "," : "") << readings.cpu[n];
    out << "],\"memory_total_kb\":" << readings.memory_total_kb << ",\"memory_available_kb\":" << readings.memory_available_kb
        << ",\"cpu_millidegrees\":" << optional_json(readings.cpu_millidegrees)
        << ",\"gpu_millidegrees\":" << optional_json(readings.gpu_millidegrees)
        << ",\"supply\":{\"name\":" << json_string(readings.supply_name) << ",\"type\":" << json_string(readings.supply_type)
        << ",\"online\":" << readings.supply_online << ",\"microvolts\":" << readings.supply_microvolts << '}'
        << ",\"power_key\":{\"available\":" << (readings.key_available ? "true" : "false")
        << ",\"down\":" << (readings.key_down ? "true" : "false") << ",\"presses\":" << readings.key_presses << '}'
        << ",\"backlight\":{\"brightness\":" << readings.backlight << ",\"maximum\":" << readings.backlight_maximum << '}'
        << ",\"udc\":" << json_string(readings.udc_state)
        << ",\"usb_in\":" << usb_json(readings.usb_in) << ",\"usb_out\":" << usb_json(readings.usb_out)
        << ",\"midi\":{\"send_drops\":" << readings.send_drops << ",\"input_errors\":" << readings.input_errors << ",\"ports\":[";
    for (size_t n = 0; n < readings.ports.size(); ++n) {
        const auto& port = readings.ports[n];
        out << (n ? "," : "") << "{\"name\":" << json_string(port.name) << ",\"input\":" << (port.input ? "true" : "false")
            << ",\"output\":" << (port.output ? "true" : "false") << ",\"surface\":" << (port.surface ? "true" : "false") << '}';
    }
    out << "]},\"jacks\":{\"headphones\":" << readings.headphones << ",\"line_out\":" << readings.line_out << "},\"storage\":[";
    for (size_t n = 0; n < readings.storage.size(); ++n) {
        const auto& mount = readings.storage[n];
        out << (n ? "," : "") << "{\"path\":" << json_string(mount.path) << ",\"type\":" << json_string(mount.type)
            << ",\"read_only\":" << (mount.read_only ? "true" : "false") << ",\"free_bytes\":" << mount.free_bytes
            << ",\"total_bytes\":" << mount.total_bytes << '}';
    }
    out << "]}";
    return out.str();
}
} // namespace

std::string system_command(DemoState& state, const std::string& argument) {
    auto& shared = state.system;
    if (argument.empty()) return "OK " + readings_json(shared.copy(), shared.panel.load());
    auto panel = shared.panel.load();
    auto name = std::find(panel_names.begin(), panel_names.end(), argument);
    if (argument == "next" || argument == "previous") panel = step_panel(panel, argument == "next" ? 1 : -1);
    else if (name != panel_names.end()) panel = Panel(name - panel_names.begin());
    else return "ERR use system, system next|previous or system health|audio|ports|storage";
    shared.panel.store(panel); state.page.store(Page::System);
    return std::string("OK system ") + panel_names[size_t(panel)];
}

SystemView system_view(const DemoState& state, const Session& session, bool shown) {
    SystemView view;
    if (!shown) return view;
    view.panel = state.system.panel.load();
    view.readings = state.system.copy();
    const auto& audio = session.audio;
    view.frames = audio.frames.load(); view.xruns = audio.xruns.load(); view.capture_xruns = audio.capture_xruns.load();
    view.capture_active = audio.capture_active.load(); view.peak = audio.peak.load(); view.input_peak = audio.input_peak.load();
    return view;
}
