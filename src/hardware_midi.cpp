// SPDX-License-Identifier: AGPL-3.0-only
#include "hardware_midi.h"
#include "platform.h"
#include <algorithm>
#include <chrono>
#include <cerrno>
#include <sstream>

struct HardwareMidi::Port {
    MidiPortInfo info;
    snd_rawmidi_t *input = nullptr, *output = nullptr;
    MidiStreamParser parser;
    std::array<MidiPacket, 8> pending {};
    unsigned head = 0, tail = 0, offset = 0;
    ~Port() {
        // Nonblocking close: never drain an absent USB device indefinitely.
        if (input) snd_rawmidi_close(input);
        if (output) { snd_rawmidi_drop(output); snd_rawmidi_close(output); }
    }
};

std::vector<MidiPortInfo> HardwareMidi::enumerate() {
    std::vector<MidiPortInfo> result;
    snd_ctl_card_info_t* card_info; snd_ctl_card_info_alloca(&card_info);
    snd_rawmidi_info_t* info; snd_rawmidi_info_alloca(&info);
    int card = -1;
    for (unsigned cards = 0; cards < 64 && snd_card_next(&card) >= 0 && card >= 0; ++cards) {
        snd_ctl_t* ctl = nullptr;
        std::string address = "hw:" + std::to_string(card);
        if (snd_ctl_open(&ctl, address.c_str(), SND_CTL_NONBLOCK) < 0) continue;
        if (snd_ctl_card_info(ctl, card_info) < 0) { snd_ctl_close(ctl); continue; }
        std::string id = snd_ctl_card_info_get_id(card_info), name = snd_ctl_card_info_get_name(card_info);
        int device = -1;
        for (unsigned devices = 0; devices < 64 && snd_ctl_rawmidi_next_device(ctl, &device) >= 0 && device >= 0; ++devices) {
            unsigned count = 0;
            for (auto direction : {SND_RAWMIDI_STREAM_INPUT, SND_RAWMIDI_STREAM_OUTPUT}) {
                snd_rawmidi_info_set_device(info, unsigned(device));
                snd_rawmidi_info_set_stream(info, direction);
                snd_rawmidi_info_set_subdevice(info, 0);
                if (snd_ctl_rawmidi_info(ctl, info) >= 0) count = std::max(count, snd_rawmidi_info_get_subdevices_count(info));
            }
            for (unsigned sub = 0; sub < std::min(count, 32u) && result.size() < 64; ++sub) {
                MidiPortInfo port;
                port.address = address + "," + std::to_string(device) + "," + std::to_string(sub);
                port.card = id; port.name = name; port.driver = snd_ctl_card_info_get_driver(card_info);
                port.surface = id == "Surface" || name == "Control Surface";
                for (auto direction : {SND_RAWMIDI_STREAM_INPUT, SND_RAWMIDI_STREAM_OUTPUT}) {
                    snd_rawmidi_info_set_device(info, unsigned(device));
                    snd_rawmidi_info_set_subdevice(info, sub);
                    snd_rawmidi_info_set_stream(info, direction);
                    if (snd_ctl_rawmidi_info(ctl, info) < 0) continue;
                    if (direction == SND_RAWMIDI_STREAM_INPUT) port.input = true; else port.output = true;
                    const char* label = snd_rawmidi_info_get_subdevice_name(info);
                    if (label && *label) port.name = name + " / " + label;
                }
                result.push_back(port);
            }
        }
        snd_ctl_close(ctl);
    }
    return result;
}

HardwareMidi::HardwareMidi() { rescan(); }
HardwareMidi::~HardwareMidi() = default;
bool HardwareMidi::rescan() {
    auto found = enumerate();
    bool lost = false;
    ports.erase(std::remove_if(ports.begin(), ports.end(), [&](const auto& port) {
        auto match = std::find_if(found.begin(), found.end(), [&](const auto& info) {
            return info.address == port->info.address && info.card == port->info.card &&
                   info.name == port->info.name && info.driver == port->info.driver;
        });
        if (match == found.end()) { lost = lost || port->input; return true; }
        return false;
    }), ports.end());
    for (auto& port : ports) {
        if (port->info.output && !port->output && snd_rawmidi_open(nullptr, &port->output, port->info.address.c_str(), SND_RAWMIDI_NONBLOCK) < 0) port->output = nullptr;
        if (port->info.input && !port->input && snd_rawmidi_open(&port->input, nullptr, port->info.address.c_str(), SND_RAWMIDI_NONBLOCK) < 0) port->input = nullptr;
    }
    for (const auto& info : found) {
        if (ports.size() >= 32) break;
        if (std::any_of(ports.begin(), ports.end(), [&](const auto& port) { return port->info.address == info.address; })) continue;
        auto port = std::make_unique<Port>(); port->info = info;
        if (info.input && snd_rawmidi_open(&port->input, nullptr, info.address.c_str(), SND_RAWMIDI_NONBLOCK) < 0) port->input = nullptr;
        if (info.output && snd_rawmidi_open(nullptr, &port->output, info.address.c_str(), SND_RAWMIDI_NONBLOCK) < 0) port->output = nullptr;
        if (port->input || port->output) ports.push_back(std::move(port));
    }
    return lost;
}
void HardwareMidi::read(const Receive& receive) {
    for (auto& port : ports) {
        if (!port->input) continue;
        uint8_t bytes[256];
        for (unsigned batch = 0; batch < 8; ++batch) {
            auto size = snd_rawmidi_read(port->input, bytes, sizeof(bytes));
            if (size == -EAGAIN || size == 0) break;
            if (size < 0) {
                ++input_errors;
                // Reopen on the next scan, even when ALSA recycled the number.
                MidiPacket panic; panic.size = 3; panic.bytes[0] = 0xb0; panic.bytes[1] = 120;
                receive(port->info, panic);
                snd_rawmidi_close(port->input); port->input = nullptr;
                if (port->output) { snd_rawmidi_drop(port->output); snd_rawmidi_close(port->output); port->output = nullptr; }
                break;
            }
            for (int i = 0; i < size; ++i) port->parser.feed(bytes[i], [&](const MidiPacket& packet) {
                MidiPacket timed = packet;
                timed.timestamp_ns = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
                receive(port->info, timed);
            });
        }
    }
    ports.erase(std::remove_if(ports.begin(), ports.end(), [](const auto& p) { return !p->input && !p->output; }), ports.end());
}
bool HardwareMidi::send(const std::string& address, const MidiPacket& packet) {
    if (!packet.size || packet.size > packet.bytes.size()) return false;
    for (auto& port : ports) if (port->info.address == address && port->output) {
        unsigned next = (port->head + 1) % port->pending.size();
        if (next == port->tail) { ++send_drops; return false; }
        port->pending[port->head] = packet; port->head = next; return true;
    }
    return false;
}
bool HardwareMidi::send_surface(const MidiPacket& packet) {
    for (const auto& port : ports) if (port->info.surface && port->output) return send(port->info.address, packet);
    return false;
}
void HardwareMidi::flush() {
    for (auto& port : ports) {
        if (!port->output || port->head == port->tail) continue;
        const auto& packet = port->pending[port->tail];
        auto size = snd_rawmidi_write(port->output, packet.bytes.data() + port->offset, packet.size - port->offset);
        if (size == -EAGAIN || size == 0) continue;
        if (size < 0) {
            ++send_drops; port->tail = port->head; port->offset = 0;
            snd_rawmidi_drop(port->output); snd_rawmidi_close(port->output); port->output = nullptr;
            continue;
        }
        port->offset += unsigned(size);
        if (port->offset == packet.size) { port->offset = 0; port->tail = (port->tail + 1) % port->pending.size(); }
    }
}
bool HardwareMidi::has_surface() const {
    return std::any_of(ports.begin(), ports.end(), [](const auto& p) { return p->info.surface && p->input; });
}
std::string HardwareMidi::json() const {
    std::ostringstream out; out << "{\"send_drops\":" << send_drops << ",\"input_errors\":" << input_errors << ",\"ports\":[";
    bool first = true;
    for (const auto& port : ports) {
        if (!first) out << ',';
        first = false;
        out << "{\"address\":" << json_string(port->info.address) << ",\"card\":" << json_string(port->info.card)
            << ",\"name\":" << json_string(port->info.name) << ",\"driver\":" << json_string(port->info.driver)
            << ",\"surface\":" << (port->info.surface ? "true" : "false")
            << ",\"input\":" << (port->input ? "true" : "false") << ",\"output\":" << (port->output ? "true" : "false")
            << ",\"sysex_drops\":" << port->parser.dropped_sysex() << '}';
    }
    return out.str() + "]}";
}

std::vector<std::string> HardwareMidi::outputs() const {
    std::vector<std::string> result;
    for (const auto& port : ports) if (port->output) result.push_back(port->info.address);
    return result;
}
