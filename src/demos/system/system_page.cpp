// SPDX-License-Identifier: AGPL-3.0-only
#include "demos/demo_screen.h"
#include "demos/system/system.h"
#include <algorithm>

// The System page: one of four panels of live readings in tiles, the
// panel's name and position above them. Readings the device did not report
// show "n/a"; a dash means nothing has been measured yet.
namespace {
using namespace sysinfo;

constexpr int top = 66; // Below the panel heading.

class Painter {
public:
    Painter(juce::Graphics& g, const Ui& ui) : g(g), ui(ui) {}
    void label(const juce::String& value, int x, int y, int w, int h, float size, juce::Colour colour,
               juce::Justification justification = juce::Justification::centredLeft) const {
        ui.label(g, value, x, y, w, h, size, colour, justification);
    }
    void text(const std::string& value, int x, int y, int w, int h, float size, juce::Colour colour,
              juce::Justification justification = juce::Justification::centredLeft) const {
        label(juce::String::fromUTF8(value.c_str()), x, y, w, h, size, colour, justification);
    }
    // A tile with its title in small capitals and an optional note on the right.
    void tile(int x, int y, int w, int h, const juce::String& title, const juce::String& note = {},
              juce::Colour note_colour = colours::muted) const {
        g.setColour(colours::panel); g.fillRoundedRectangle(float(x), float(y), float(w), float(h), 5);
        label(title, x + 8, y + 2, w - 16, 14, 10, colours::muted);
        if (note.isNotEmpty()) label(note, x + 8, y + 2, w - 16, 14, 10, note_colour, juce::Justification::centredRight);
    }
    // A horizontal bar `share` full.
    void bar(int x, int y, int w, int h, float share, juce::Colour colour) const {
        g.setColour(colours::track); g.fillRoundedRectangle(float(x), float(y), float(w), float(h), h / 2.0f);
        float fill = std::clamp(share, 0.0f, 1.0f) * float(w);
        if (fill > 0.5f) { g.setColour(colour); g.fillRoundedRectangle(float(x), float(y), std::max(fill, float(h)), float(h), h / 2.0f); }
    }
    juce::Graphics& g;
    const Ui& ui;
};

juce::Colour load_colour(float share) { return share > 0.9f ? colours::red : share > 0.75f ? colours::amber : colours::mint; }
juce::Colour temperature_colour(std::optional<long> millidegrees) {
    if (!millidegrees) return colours::muted;
    return *millidegrees >= 85000 ? colours::red : *millidegrees >= 70000 ? colours::amber : colours::text;
}
juce::Colour usb_colour(UsbStreamState state) {
    return state == UsbStreamState::Running ? colours::mint : state == UsbStreamState::Failed ? colours::red
         : state == UsbStreamState::Waiting ? colours::text : colours::muted;
}

// CPU load now and per core, its history, memory, temperatures, power and backlight.
void draw_health(const Painter& p, const SystemView& view) {
    using namespace colours;
    const auto& r = view.readings;
    auto& g = p.g;
    bool measured = !r.cpu.empty();
    p.tile(8, top, 304, 44, "CPU", "Last " + juce::String(history_length * read_ms / 1000) + " s");
    p.label(measured ? juce::String(percent_text(r.cpu.front())) : "-", 16, top + 16, 60, 24, 20, measured ? text : muted);
    // One bar per core, filling upwards.
    unsigned cores = unsigned(std::min<size_t>(r.cpu.size() > 1 ? r.cpu.size() - 1 : 0, 8));
    for (unsigned core = 0; core < cores; ++core) {
        float load = r.cpu[core + 1], x = 76.0f + core * 9.0f;
        g.setColour(track); g.fillRoundedRectangle(x, float(top + 17), 6, 22, 2);
        float height = std::max(2.0f, 22 * load);
        g.setColour(load_colour(load)); g.fillRoundedRectangle(x, float(top + 39) - height, 6, height, 2);
    }
    // History, oldest on the left.
    constexpr int graph_x = 156, graph_w = 148, graph_y = top + 17, graph_h = 22;
    g.setColour(track); g.fillRect(graph_x, graph_y + graph_h, graph_w, 1);
    if (r.history.count > 1) {
        juce::Path line, area;
        float step = float(graph_w) / float(history_length - 1);
        unsigned first = history_length - r.history.count;
        for (unsigned n = first; n < history_length; ++n) {
            float x = graph_x + step * n, y = graph_y + graph_h - graph_h * r.history.percent[n] / 100.0f;
            if (n == first) { line.startNewSubPath(x, y); area.startNewSubPath(x, float(graph_y + graph_h)); }
            else line.lineTo(x, y);
            area.lineTo(x, y);
        }
        area.lineTo(float(graph_x + graph_w), float(graph_y + graph_h)); area.closeSubPath();
        g.setColour(mint.withAlpha(0.18f)); g.fillPath(area);
        g.setColour(mint); g.strokePath(line, juce::PathStrokeType(1.5f));
    }

    int y = top + 48;
    float used = memory_used(r);
    p.tile(8, y, 148, 42, "MEMORY", r.memory_total_kb ? "of " + juce::String(bytes_text(r.memory_total_kb * 1024)) : juce::String());
    p.text(r.memory_total_kb ? bytes_text((r.memory_total_kb - std::min(r.memory_available_kb, r.memory_total_kb)) * 1024) + " used" : "n/a",
           16, y + 14, 132, 18, 14, r.memory_total_kb ? text : muted);
    p.bar(16, y + 33, 132, 4, used, load_colour(used));

    p.tile(164, y, 148, 42, "TEMPERATURE");
    p.label("CPU", 172, y + 14, 40, 13, 11, muted);
    p.text(celsius_text(r.cpu_millidegrees), 204, y + 13, 100, 14, 12, temperature_colour(r.cpu_millidegrees), juce::Justification::centredRight);
    p.label("GPU", 172, y + 27, 40, 13, 11, muted);
    p.text(celsius_text(r.gpu_millidegrees), 204, y + 26, 100, 14, 12, temperature_colour(r.gpu_millidegrees), juce::Justification::centredRight);

    y += 46;
    juce::String supply = r.supply_type.empty() ? juce::String(r.supply_name) : juce::String(r.supply_type);
    if (r.supply_online >= 0) supply += r.supply_online ? " online" : " offline";
    p.tile(8, y, 148, 42, "POWER", supply);
    p.text(volts_text(r.supply_microvolts), 16, y + 13, 70, 18, 15, r.supply_microvolts >= 0 ? text : muted);
    p.text(std::string("Key ") + key_text(r), 86, y + 15, 62, 14, 10, r.key_down ? amber : muted, juce::Justification::centredRight);
    p.label(r.key_available ? "Power key pressed " + juce::String(r.key_presses) + (r.key_presses == 1 ? " time" : " times")
                            : juce::String("No power key"), 16, y + 28, 132, 12, 9, muted);

    p.tile(164, y, 148, 42, "BACKLIGHT");
    bool lit = r.backlight >= 0 && r.backlight_maximum > 0;
    p.label(lit ? juce::String(r.backlight) + " / " + juce::String(r.backlight_maximum) : "n/a", 172, y + 13, 60, 18, 15, lit ? text : muted);
    if (lit && r.backlight_maximum <= 12) {
        int count = int(r.backlight_maximum), width = (132 - (count - 1) * 3) / count;
        for (int n = 0; n < count; ++n) {
            g.setColour(n < r.backlight ? amber : track);
            g.fillRoundedRectangle(float(172 + n * (width + 3)), float(y + 32), float(width), 5, 2);
        }
    } else if (lit) p.bar(172, y + 32, 132, 5, float(r.backlight) / float(r.backlight_maximum), amber);
}

// The audio engine's counters and meters, the USB-C port and USB-C audio both ways.
void draw_audio(const Painter& p, const SystemView& view) {
    using namespace colours;
    const auto& r = view.readings;
    p.tile(8, top, 304, 44, "AUDIO ENGINE", "44.1 kHz  up " + juce::String(duration_text(view.frames, 44100)));
    p.label("OUT", 16, top + 15, 30, 12, 10, muted);
    p.bar(46, top + 19, 150, 5, view.peak, mint);
    p.label(view.capture_active ? "IN" : "IN off", 16, top + 28, 34, 12, 10, muted);
    p.bar(46, top + 32, 150, 5, view.capture_active ? view.input_peak : 0, mint);
    p.label("Recoveries " + juce::String(view.xruns), 204, top + 14, 100, 13, 11, view.xruns ? amber : text, juce::Justification::centredRight);
    p.label("Capture " + juce::String(view.capture_xruns), 204, top + 27, 100, 13, 11, view.capture_xruns ? amber : text,
            juce::Justification::centredRight);

    int y = top + 48;
    p.tile(8, y, 304, 22, "USB-C PORT");
    bool configured = r.udc_state == "configured";
    p.text(r.udc_state.empty() ? "n/a" : r.udc_state, 120, y + 2, 184, 16, 12, configured ? mint : muted, juce::Justification::centredRight);

    y += 26;
    auto stream = [&](int x, const juce::String& title, const UsbStreamStats& stats, bool output) {
        p.tile(x, y, 148, 62, title, juce::String(rate_text(stats.rate)));
        p.text(usb_state_text(stats.state), x + 8, y + 14, 132, 20, 15, usb_colour(stats.state));
        p.label("Underruns " + juce::String(stats.underruns) + "  xruns " + juce::String(stats.xruns), x + 8, y + 33, 132, 13, 10,
                stats.underruns || stats.xruns ? amber : muted);
        p.text(output ? "Drift trim " + ppm_text(stats.trim_ppm) : "Opened " + std::to_string(stats.opens) + " times", x + 8, y + 46, 132, 13, 10, muted);
    };
    stream(8, "USB-C IN", r.usb_in, false);
    stream(164, "USB-C OUT", r.usb_out, true);
}

// MIDI ports with their open directions, and the two jack detection switches.
void draw_ports(const Painter& p, const SystemView& view) {
    using namespace colours;
    const auto& r = view.readings;
    bool lost = r.send_drops || r.input_errors;
    p.tile(8, top, 304, 88, "MIDI PORTS", "Drops " + juce::String(r.send_drops) + "  errors " + juce::String(r.input_errors), lost ? amber : muted);
    if (r.ports.empty()) p.label(r.reads ? "No MIDI ports" : "-", 16, top + 18, 288, 16, 12, muted);
    for (size_t n = 0; n < r.ports.size() && n < 5; ++n) {
        const auto& port = r.ports[n];
        int y = top + 17 + int(n) * 14;
        p.text(port.name, 16, y, 180, 14, 12, text);
        if (port.surface) p.label("surface", 176, y, 50, 14, 10, muted, juce::Justification::centredRight);
        p.label("IN", 236, y, 30, 14, 10, port.input ? mint : track, juce::Justification::centred);
        p.label("OUT", 270, y, 34, 14, 10, port.output ? mint : track, juce::Justification::centred);
    }

    int y = top + 92;
    auto jack = [&](int x, const juce::String& title, int value) {
        p.tile(x, y, 148, 44, title);
        p.label(jack_text(value), x + 8, y + 15, 132, 22, 15, value > 0 ? mint : muted);
    };
    jack(8, "HEADPHONES", r.headphones);
    jack(164, "LINE OUT", r.line_out);
}

// Each mounted device: its path, file system, free space and how full it is.
void draw_storage(const Painter& p, const SystemView& view) {
    using namespace colours;
    const auto& r = view.readings;
    if (r.storage.empty()) {
        p.tile(8, top, 304, 44, "STORAGE");
        p.label(r.reads ? "No mounted storage" : "-", 16, top + 16, 288, 20, 13, muted);
        return;
    }
    for (size_t n = 0; n < r.storage.size() && n < max_mounts; ++n) {
        const auto& mount = r.storage[n];
        int y = top + int(n) * 27;
        p.g.setColour(panel); p.g.fillRoundedRectangle(8, float(y), 304, 25, 4);
        p.text(mount.path, 16, y + 1, 150, 15, 12, text);
        p.text(mount.total_bytes ? bytes_text(mount.free_bytes) + " free of " + bytes_text(mount.total_bytes) : "n/a",
               164, y + 1, 140, 15, 11, muted, juce::Justification::centredRight);
        p.text(mount.type + (mount.read_only ? "  read-only" : ""), 16, y + 14, 84, 11, 9, muted);
        float used = storage_used(mount);
        p.bar(100, y + 17, 204, 4, used, mount.read_only ? muted : load_colour(used));
    }
}
} // namespace

void draw_system_page(juce::Graphics& g, const Ui& ui, const DemoView& state) {
    using namespace colours;
    const auto& view = state.system;
    Painter p(g, ui);
    auto panel = view.panel;
    ui.soft_key(g, 0, "PREVIOUS", panel_titles[size_t(step_panel(panel, -1))], text);
    ui.soft_key(g, 1, "NEXT", panel_titles[size_t(step_panel(panel, 1))], text);
    ui.soft_key(g, 2, "PAGE", "Synth", text);

    // The panel's name, and where it sits among the four.
    p.label(juce::String("SYSTEM  /  ") + juce::String(panel_titles[size_t(panel)]).toUpperCase(), 12, 48, 200, 16, 10, mint);
    if (!view.readings.reads) p.label("Reading", 200, 48, 64, 16, 10, muted, juce::Justification::centredRight);
    for (unsigned n = 0; n < unsigned(Panel::Count); ++n) {
        g.setColour(n == unsigned(panel) ? mint : track);
        g.fillRoundedRectangle(float(272 + n * 10), 54, 6, 6, 3);
    }

    switch (panel) {
    case Panel::Audio: draw_audio(p, view); break;
    case Panel::Ports: draw_ports(p, view); break;
    case Panel::Storage: draw_storage(p, view); break;
    default: draw_health(p, view); break;
    }
    ui.footer(g, "Knob FX: synth   Wheel: panels", state.last_input);
}
