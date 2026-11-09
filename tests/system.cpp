// SPDX-License-Identifier: AGPL-3.0-only
// The system page's model: panels, the CPU meter and history, picking
// readings and the text the page shows.
#include "demos/system/system.h"
#include <cassert>
#include <string>

using namespace sysinfo;

int main() {
    // Panels wrap both ways.
    assert(step_panel(Panel::Health, -1) == Panel::Storage && step_panel(Panel::Storage, 1) == Panel::Health);
    assert(step_panel(Panel::Health, 5) == Panel::Audio && step_panel(Panel::Ports, -6) == Panel::Health);
    assert(std::string(panel_names[size_t(Panel::Ports)]) == "ports");

    // The first sample has nothing to compare with; a changed CPU count starts again.
    CpuMeter meter;
    assert(meter.sample({{100, 1000}, {50, 500}, {50, 500}}).empty());
    auto loads = meter.sample({{200, 1200}, {150, 600}, {50, 600}});
    assert(loads.size() == 3 && loads[0] == 0.5f && loads[1] == 1.0f && loads[2] == 0.0f);
    assert(meter.sample({{300, 1400}}).empty() && meter.sample({{400, 1800}}).size() == 1);
    meter.reset();
    assert(meter.sample({{500, 2000}}).empty());

    // History keeps the newest at the end, in percent, clamped.
    History history;
    history.push(0.25f); history.push(1.5f); history.push(-1);
    assert(history.count == 3 && history.percent[history_length - 3] == 25);
    assert(history.percent[history_length - 2] == 100 && history.percent.back() == 0);
    for (unsigned n = 0; n < 2 * history_length; ++n) history.push(0.5f);
    assert(history.count == history_length && history.percent.front() == 50);

    // Thermal zones by type prefix; the supply that reports a voltage.
    std::vector<ThermalReading> zones {{"cpu-thermal", 39444}, {"gpu-thermal", 37222}};
    assert(zone_temperature(zones, "cpu") == 39444L && zone_temperature(zones, "gpu") == 37222L);
    assert(!zone_temperature(zones, "npu") && !zone_temperature({}, "cpu"));
    std::vector<SupplyReading> supplies {{"usb", "USB", 1, -1}, {"mains-power", "Mains", 1, 3592000}};
    assert(main_supply(supplies)->name == "mains-power" && !main_supply({}));
    supplies[1].microvolts = -1;
    assert(main_supply(supplies)->name == "usb");

    // Shares used, never past full or below empty.
    Readings readings;
    assert(memory_used(readings) == 0);
    readings.memory_total_kb = 2000; readings.memory_available_kb = 500;
    assert(memory_used(readings) == 0.75f);
    readings.memory_available_kb = 4000;
    assert(memory_used(readings) == 0);
    assert(storage_used({"/", "ext4", true, 0, 100}) == 1 && storage_used({"/data", "ext4", false, 25, 100}) == 0.75f);
    assert(storage_used({"/sd", "exfat", false, 200, 100}) == 0 && storage_used({"/gone", "", true, 0, 0}) == 0);

    // The page's text.
    assert(bytes_text(0) == "0 B" && bytes_text(512) == "512 B" && bytes_text(1536) == "1.5 KB");
    assert(bytes_text(uint64_t(360) << 20) == "360 MB" && bytes_text(uint64_t(2018016) * 1024) == "1.9 GB");
    assert(bytes_text(uint64_t(9.94 * (1 << 20))) == "9.9 MB" && bytes_text(uint64_t(9.96 * (1 << 20))) == "10 MB");
    assert(bytes_text(uint64_t(3) << 40) == "3.0 TB");
    assert(volts_text(3592000) == "3.59 V" && volts_text(5000000) == "5.00 V" && volts_text(-1) == "n/a");
    assert(celsius_text(39444L) == "39.4 °C" && celsius_text(-1500L) == "-1.5 °C" && celsius_text(std::nullopt) == "n/a");
    assert(percent_text(0.123f) == "12%" && percent_text(1.5f) == "100%" && percent_text(-0.2f) == "0%");
    assert(rate_text(48000) == "48 kHz" && rate_text(44100) == "44.1 kHz" && rate_text(0) == "-");
    assert(ppm_text(-21) == "-21 ppm" && ppm_text(3) == "+3 ppm" && ppm_text(0) == "0 ppm");
    assert(duration_text(44100 * 65, 44100) == "1:05" && duration_text(uint64_t(44100) * 3725, 44100) == "1:02:05");
    assert(duration_text(1000, 0) == "0:00");
    assert(std::string(jack_text(1)) == "Plugged in" && std::string(jack_text(0)) == "Empty" && std::string(jack_text(-1)) == "n/a");
    assert(std::string(key_text(readings)) == "n/a");
    readings.key_available = true; assert(std::string(key_text(readings)) == "up");
    readings.key_down = true; assert(std::string(key_text(readings)) == "held");
    assert(std::string(usb_state_text(UsbStreamState::Running)) == "Running" && std::string(usb_state_text(UsbStreamState::Missing)) == "No gadget");

    // The screen copies what the service thread published.
    Shared shared;
    { std::lock_guard<std::mutex> hold(shared.lock); shared.readings.reads = 3; shared.readings.udc_state = "configured"; }
    auto copy = shared.copy();
    assert(copy.reads == 3 && copy.udc_state == "configured" && shared.panel.load() == Panel::Health);
}
