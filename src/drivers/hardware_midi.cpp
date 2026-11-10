// SPDX-License-Identifier: AGPL-3.0-only
#include "drivers/hardware_midi.h"
#include "drivers/platform.h"
#include <algorithm>
#include <chrono>
#include <cerrno>
#include <poll.h>
#include <time.h>
#include <sstream>

struct HardwareMidi::Port {
    MidiPortInfo info;
    snd_rawmidi_t *input = nullptr, *output = nullptr;
    MidiStreamParser parser;
    std::array<MidiPacket, HardwareMidi::output_packets + 1> pending {};
    unsigned head = 0, tail = 0, offset = 0;
    bool stamped = false; // the kernel stamps what arrives (CLOCK_MONOTONIC, at receipt in the driver)
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

// Inputs ask the kernel to stamp bytes as its driver receives them (rawmidi
// framing mode, Linux 5.14 and alsa-lib 1.2.6 on), in CLOCK_MONOTONIC, the
// clock steady_clock reads; without it the read time stands in.
static bool open_input(snd_rawmidi_t*& input, const std::string& address, bool& stamped) {
    stamped = false;
    if (snd_rawmidi_open(&input, nullptr, address.c_str(), SND_RAWMIDI_NONBLOCK) < 0) { input = nullptr; return false; }
    snd_rawmidi_params_t* params; snd_rawmidi_params_alloca(&params);
    stamped = snd_rawmidi_params_current(input, params) >= 0 &&
              snd_rawmidi_params_set_read_mode(input, params, SND_RAWMIDI_READ_TSTAMP) >= 0 &&
              snd_rawmidi_params_set_clock_type(input, params, SND_RAWMIDI_CLOCK_MONOTONIC) >= 0 &&
              snd_rawmidi_params(input, params) >= 0;
    return true;
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
        if (port->info.input && !port->input) open_input(port->input, port->info.address, port->stamped);
    }
    for (const auto& info : found) {
        if (ports.size() >= 32) break;
        if (std::any_of(ports.begin(), ports.end(), [&](const auto& port) { return port->info.address == info.address; })) continue;
        auto port = std::make_unique<Port>(); port->info = info;
        if (info.input) open_input(port->input, info.address, port->stamped);
        if (info.output && snd_rawmidi_open(nullptr, &port->output, info.address.c_str(), SND_RAWMIDI_NONBLOCK) < 0) port->output = nullptr;
        if (port->input || port->output) ports.push_back(std::move(port));
    }
    list_open();
    return lost;
}
void HardwareMidi::list_open() {
    ++changes;
    open.clear();
    for (const auto& port : ports) {
        MidiPortInfo info = port->info;
        info.input = port->input != nullptr; info.output = port->output != nullptr;
        open.push_back(std::move(info));
    }
}
void HardwareMidi::wait(long long timeout_ns) {
    if (timeout_ns <= 0) return;
    pollfd fds[64];
    nfds_t count = 0;
    for (auto& port : ports) {
        if (!port->input) continue;
        int room = int(sizeof(fds) / sizeof(fds[0])) - int(count);
        int n = room > 0 ? snd_rawmidi_poll_descriptors(port->input, fds + count, unsigned(room)) : 0;
        if (n > 0) count += nfds_t(n);
    }
    timespec limit {time_t(timeout_ns / 1000000000LL), long(timeout_ns % 1000000000LL)};
    ppoll(count ? fds : nullptr, count, &limit, nullptr);
}
void HardwareMidi::read(const Receive& receive) {
    for (auto& port : ports) {
        if (!port->input) continue;
        uint8_t bytes[256];
        for (unsigned batch = 0; batch < 8; ++batch) {
            timespec stamp {};
            auto size = port->stamped ? snd_rawmidi_tread(port->input, &stamp, bytes, sizeof(bytes))
                                      : snd_rawmidi_read(port->input, bytes, sizeof(bytes));
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
            uint64_t when = port->stamped && (stamp.tv_sec || stamp.tv_nsec)
                ? uint64_t(stamp.tv_sec) * 1000000000ull + uint64_t(stamp.tv_nsec)
                : uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
            for (int i = 0; i < size; ++i) port->parser.feed(bytes[i], [&](const MidiPacket& packet) {
                MidiPacket timed = packet;
                timed.timestamp_ns = when;
                receive(port->info, timed);
            });
        }
    }
    auto before = ports.size();
    bool closed = false;
    for (const auto& port : ports) for (const auto& info : open)
        if (info.address == port->info.address && (info.input != (port->input != nullptr) || info.output != (port->output != nullptr))) closed = true;
    ports.erase(std::remove_if(ports.begin(), ports.end(), [](const auto& p) { return !p->input && !p->output; }), ports.end());
    if (closed || ports.size() != before) list_open();
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
// Writes queued packets until the driver buffer is full. A full LED refresh
// is about 50 messages, so draining one packet per poll would drop most of it.
void HardwareMidi::flush() {
    for (auto& port : ports) {
        while (port->output && port->head != port->tail) {
            const auto& packet = port->pending[port->tail];
            unsigned remaining = packet.size - port->offset;
            auto size = snd_rawmidi_write(port->output, packet.bytes.data() + port->offset, remaining);
            if (size == -EAGAIN || size == 0) break;
            if (size < 0) {
                ++send_drops; port->tail = port->head; port->offset = 0;
                snd_rawmidi_drop(port->output); snd_rawmidi_close(port->output); port->output = nullptr;
                break;
            }
            port->offset += unsigned(size);
            if (port->offset < packet.size) break; // short write: buffer full
            port->offset = 0; port->tail = (port->tail + 1) % port->pending.size();
        }
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

MidiPorts HardwareMidi::open_ports() const {
    MidiPorts result {{}, send_drops, input_errors};
    for (const auto& port : ports) {
        auto info = port->info;
        info.input = port->input != nullptr; info.output = port->output != nullptr;
        result.ports.push_back(info);
    }
    return result;
}

std::vector<std::string> HardwareMidi::outputs() const {
    std::vector<std::string> result;
    for (const auto& port : ports) if (port->output) result.push_back(port->info.address);
    return result;
}
