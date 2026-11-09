// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include "surface/midi.h"
#include <alsa/asoundlib.h>
#include <array>
#include <functional>
#include <memory>
#include <string>
#include <vector>

struct MidiPortInfo {
    std::string address, card, name, driver;
    bool input = false, output = false, surface = false;
};

// The open ports and output losses, as the `midi` command reports them.
struct MidiPorts {
    std::vector<MidiPortInfo> ports; // `input` and `output` are the directions open now.
    unsigned send_drops = 0, input_errors = 0;
};

// ALSA owns the UARTs and USB endpoints. Device numbers are discovered, never
// assumed. All methods run on one control thread; no audio-thread scanning.
class HardwareMidi {
public:
    using Receive = std::function<void(const MidiPortInfo&, const MidiPacket&)>;
    static constexpr unsigned output_packets = 64; // queued complete messages per output port
    static std::vector<MidiPortInfo> enumerate();
    HardwareMidi();
    ~HardwareMidi();
    HardwareMidi(const HardwareMidi&) = delete;
    HardwareMidi& operator=(const HardwareMidi&) = delete;
    bool rescan(); // true if an open input disappeared; caller must panic
    void read(const Receive& receive);
    bool send(const std::string& address, const MidiPacket& packet);
    bool send_surface(const MidiPacket& packet); // false without a surface output
    void flush();
    bool has_surface() const;
    std::string json() const;
    MidiPorts open_ports() const;
    std::vector<std::string> outputs() const;
private:
    struct Port;
    std::vector<std::unique_ptr<Port>> ports;
    unsigned send_drops = 0, input_errors = 0;
};
