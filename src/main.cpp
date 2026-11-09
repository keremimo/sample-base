// SPDX-License-Identifier: AGPL-3.0-only
#include "display.h"
#include "controls.h"
#include "midi.h"
#include "screen.h"
#include "synth.h"
#include "audio.h"
#include "clock_io.h"
#include "hardware_midi.h"
#include "leds.h"
#include "mixer.h"
#include "platform.h"
#include "power_button.h"
#include "recorder.h"
#include "system_services.h"
#include "usb_audio.h"
#include <alsa/asoundlib.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <poll.h>
#include <unistd.h>
#include <pthread.h>
#include <algorithm>
#include <cctype>
#include <iterator>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include <charconv>
#include <filesystem>
#include <memory>
#include <mutex>
#include <sstream>

namespace {
constexpr unsigned rate = Synth::sample_rate, block = Synth::block_size;
volatile std::sig_atomic_t interrupted = 0;
void signal_handler(int) { interrupted = 1; }
void require(bool ok, const std::string& message) { if (!ok) throw std::runtime_error(message); }
struct Shared {
    MidiQueue<> queue;
    AudioStats audio;
    std::atomic<bool> running {true}, failed {false}, screenshot {false};
    std::atomic<uint32_t> held {0}, last_midi {0}, last_input {0};
    std::atomic<int> last_pad {-1}, velocity {0};
    std::atomic<unsigned> hardware_events {0}, external_events {0}, synthetic_events {0}, overruns {0}, clocks {0};
    std::atomic<float> gain {1}, monitor {0};
    std::atomic<unsigned> midi_send_errors {0};
    std::atomic<bool> push_exit {true};
    std::atomic<bool> prompting {false}; // A remote tool owns the screen; recorder buttons rest.
    std::mutex prompt_lock; // Control thread writes, screen thread copies.
    std::string prompt;
    // Input recording showcase. `source` changes only on the service thread,
    // after the matching hardware route is in place; `channels` and the page
    // are plain choices any thread may set.
    static constexpr unsigned recording_seconds = 60;
    Recorder recorder {recording_seconds * rate};
    UsbAudioInput usb;
    std::atomic<uint8_t> source {uint8_t(InputSource::Off)}, channels {uint8_t(InputChannels::Stereo)};
    std::atomic<float> input_left {0}, input_right {0}; // Decaying peaks of the selected source.
    std::atomic<bool> recorder_page {false};
};
struct RenderContext { Shared& state; Synth& synth; float meter_left = 0, meter_right = 0; };
void render_audio(void* pointer, const AudioBlock& input, AudioBlock& output) {
    auto& context = *static_cast<RenderContext*>(pointer);
    auto& state = context.state;
    MidiEvent event;
    if (state.queue.take_overflow()) {
        for (unsigned n = 0; n < 256 && state.queue.pop(event); ++n) {}
        context.synth.midi({0xb0,120,0}); ++state.overruns;
    }
    for (unsigned n = 0; n < 256 && state.queue.pop(event); ++n) context.synth.midi(event);
    context.synth.render(output.data());

    auto source = InputSource(state.source.load(std::memory_order_relaxed));
    auto channels = InputChannels(state.channels.load(std::memory_order_relaxed));
    std::array<float, block * 2> recorded {}, playback {};
    if (source == InputSource::Usb) state.usb.pull(recorded.data(), block);
    else if (source != InputSource::Off) for (unsigned i = 0; i < recorded.size(); ++i) recorded[i] = float(input[i] / 2147483648.0);
    // About 20 dB per 300 ms of fall, so the 75 ms screen refresh sees peaks.
    context.meter_left *= 0.978f; context.meter_right *= 0.978f;
    for (unsigned n = 0; n < block; ++n) {
        context.meter_left = std::max(context.meter_left, std::abs(recorded[2 * n]));
        context.meter_right = std::max(context.meter_right, std::abs(recorded[2 * n + 1]));
    }
    state.input_left.store(context.meter_left, std::memory_order_relaxed);
    state.input_right.store(context.meter_right, std::memory_order_relaxed);
    select_input_channels(source, channels, recorded.data(), block);
    state.recorder.process(recorded.data(), playback.data(), block, input_route(source, channels), source != InputSource::Off);

    float gain = state.gain.load(), monitor = state.monitor.load();
    for (unsigned i = 0; i < output.size(); ++i) {
        double sample = (output[i] / 2147483648.0 + playback[i] + input[i] / 2147483648.0 * monitor) * gain;
        output[i] = int32_t(std::clamp(sample, -0.99, 0.99) * 2147483647.0);
    }
}
template <class Integer> bool unsigned_value(const std::string& text, Integer& value) {
    if (text.empty()) return false;
    auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    return result.ec == std::errc() && result.ptr == text.data() + text.size();
}

std::string status(const Shared& state) {
    char reply[1536];
    snprintf(reply, sizeof(reply), "OK frames=%llu sounding_blocks=%llu hardware_events=%u external_events=%u synthetic_events=%u "
             "held=%04x last_pad=%d velocity=%d last_midi=%06x peak=%.6f input_peak=%.6f capture_frames=%llu capture_active=%u "
             "xruns=%u capture_xruns=%u queue_overruns=%u clocks=%u gain=%.2f monitor=%.2f push_exit=%u "
             "source=%s channels=%s recorder=%s recorded_frames=%u play_frame=%u takes=%u usb=%s",
             static_cast<unsigned long long>(state.audio.frames.load()), static_cast<unsigned long long>(state.audio.sounding_blocks.load()),
             state.hardware_events.load(), state.external_events.load(), state.synthetic_events.load(), state.held.load(), state.last_pad.load() + 1,
             state.velocity.load(), state.last_midi.load(), double(state.audio.peak.load()), double(state.audio.input_peak.load()),
             static_cast<unsigned long long>(state.audio.capture_frames.load()), unsigned(state.audio.capture_active.load()),
             state.audio.xruns.load(), state.audio.capture_xruns.load(), state.overruns.load(), state.clocks.load(), double(state.gain.load()), double(state.monitor.load()), unsigned(state.push_exit.load()),
             input_source_names[state.source.load()], input_channel_names[state.channels.load()],
             Recorder::state_names[size_t(state.recorder.state())], state.recorder.length(), state.recorder.position(),
             state.recorder.takes(), usb_input_state_names[size_t(state.usb.state())]);
    return reply;
}
std::string recorder_json(const Shared& state) {
    auto& recorder = state.recorder;
    unsigned route = recorder.route();
    std::ostringstream out;
    out << "{\"source\":" << json_string(input_source_names[state.source.load()])
        << ",\"channels\":" << json_string(input_channel_names[state.channels.load()])
        << ",\"state\":" << json_string(Recorder::state_names[size_t(recorder.state())])
        << ",\"rate\":" << rate << ",\"frames\":" << recorder.length() << ",\"position\":" << recorder.position()
        << ",\"capacity\":" << recorder.capacity() << ",\"takes\":" << recorder.takes() << ",\"refused\":" << recorder.refused()
        << ",\"take_source\":" << json_string(recorder.takes() ? input_source_names[(route >> 4) % input_source_names.size()] : "")
        << ",\"take_channels\":" << json_string(recorder.takes() ? input_channel_names[(route & 15) % input_channel_names.size()] : "")
        << ",\"input_peaks\":[" << state.input_left.load() << ',' << state.input_right.load() << ']'
        << ",\"usb\":" << state.usb.json() << '}';
    return out.str();
}
std::string surface_json(const SurfaceState& state) {
    std::ostringstream out; out << "{\"timestamp_ns\":" << state.last.timestamp_ns << ",\"pitch\":" << state.pitch << ",\"channel_pressure\":" << unsigned(state.channel_pressure);
    for (const auto& pair : {std::make_pair("notes", &state.notes), std::make_pair("pressure", &state.pressure), std::make_pair("controls", &state.controls)}) {
        out << ',' << json_string(pair.first) << ":[";
        for (unsigned i = 0; i < pair.second->size(); ++i) { if (i) out << ','; out << unsigned((*pair.second)[i]); }
        out << ']';
    }
    out << ",\"last\":[";
    for (unsigned i = 0; i < state.last.size; ++i) { if (i) out << ','; out << unsigned(state.last.bytes[i]); }
    return out.str() + "]}";
}
// Entries are [sequence, timestamp_ns, size, [bytes]]. `next` is the cursor
// for the following request; a first sequence above since+1 means lost history.
template <unsigned Size> std::string surface_log_json(const SurfaceLog<Size>& log, uint64_t since, unsigned overflows) {
    std::ostringstream out; out << "{\"oldest\":" << log.oldest() << ",\"newest\":" << log.newest()
        << ",\"diagnostic_overflows\":" << overflows << ",\"packets\":[";
    bool first = true;
    auto next = log.read(since, 256, [&](const typename SurfaceLog<Size>::Entry& entry) {
        if (!first) out << ',';
        first = false;
        out << '[' << entry.sequence << ',' << entry.timestamp_ns << ',' << entry.size << ",[";
        for (unsigned n = 0; n < entry.size && n < entry.bytes.size(); ++n) { if (n) out << ','; out << unsigned(entry.bytes[n]); }
        out << "]]";
    });
    out << "],\"next\":" << next << '}';
    return out.str();
}

struct MidiSendRequest { std::array<char, 64> address {}; MidiPacket packet; };
struct MidiCatalog { std::string json; std::vector<std::string> outputs; };
class Controller {
public:
    Controller(const std::string& runtime, bool required, const std::string& usb)
        : socket_path(runtime + "/control.sock"), surface_required(required), usb_device(usb) {
        catalog = {midi.json(), midi.outputs()};
        require(!required || midi.has_surface(), "Control surface unavailable; use --no-surface only for a headless/host test");
        require(socket_path.size() < sizeof(sockaddr_un::sun_path), "Runtime path is too long");
        std::filesystem::create_directories(runtime);
        struct stat directory {};
        require(stat(runtime.c_str(), &directory) == 0 && directory.st_uid == geteuid() && (directory.st_mode & 077) == 0,
                "Runtime directory must be private and owned by this user");
        fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
        require(fd >= 0, "Create control socket");
        sockaddr_un address {}; address.sun_family = AF_UNIX;
        strcpy(address.sun_path, socket_path.c_str());
        unlink(socket_path.c_str());
        if (bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
            close(fd); fd = -1; throw std::runtime_error("Bind control socket");
        }
        chmod(socket_path.c_str(), 0600);
    }
    ~Controller() { if (fd >= 0) close(fd); unlink(socket_path.c_str()); }
    void run(Shared& state) {
        std::thread io_thread;
        try {
            io_thread = std::thread([&] { run_midi(state); });
            while (state.running.load()) {
                MidiPacket packet;
                if (surface_events.take_overflow()) {
                    for (unsigned n = 0; n < 256 && surface_events.pop(packet); ++n) {}
                    surface = {}; ++surface_overflows;
                }
                for (unsigned n = 0; n < 256 && surface_events.pop(packet); ++n) { surface.apply(packet); surface_log.push(packet); }
                MidiCatalog update;
                while (catalog_updates.pop(update)) catalog = std::move(update);
                clock.poll(); services.poll();
                int requested = source_request.exchange(-1);
                if (requested >= 0) select_source(state, InputSource(requested));
                if (power.poll()) state.last_input.store(pack_last_input(InputKind::Power, 0, power.pressed()));
                if (services.stopping()) state.running.store(false);
                pollfd ready {fd, POLLIN, 0};
                int result = poll(&ready, 1, 5);
                if (result < 0 && errno != EINTR) throw std::runtime_error("Control poll failed");
                if (ready.revents & POLLIN) {
                    sockaddr_un sender {}; socklen_t size = sizeof(sender);
                    char request[4096] {};
                    auto length = recvfrom(fd, request, sizeof(request) - 1, MSG_TRUNC, reinterpret_cast<sockaddr*>(&sender), &size);
                    if (length > 0) {
                        std::string reply = length >= ssize_t(sizeof(request)) ? "ERR command too long" : command(state, request);
                        if (reply.size() >= 32768) reply = "ERR response too large";
                        sendto(fd, reply.data(), reply.size(), MSG_DONTWAIT, reinterpret_cast<sockaddr*>(&sender), size);
                    }
                }
            }
        } catch (const std::exception& error) {
            std::cerr << error.what() << '\n'; state.failed.store(true); state.running.store(false);
        }
        state.running.store(false);
        if (io_thread.joinable()) io_thread.join();
    }
private:
    void run_midi(Shared& state) {
        try {
            auto release = std::chrono::steady_clock::time_point::max();
            auto scan = std::chrono::steady_clock::now();
            // Absolute controls only report when moved, so ask for their
            // current positions. The reply is applied like any other CC but
            // is not shown as the last control the player touched.
            auto syncing_until = std::chrono::steady_clock::time_point::min();
            auto request_positions = [&] {
                if (midi.send_surface(surface_position_packet()))
                    syncing_until = std::chrono::steady_clock::now() + std::chrono::milliseconds(300);
            };
            request_positions();
            while (state.running.load()) {
                midi.read([&](const MidiPortInfo& port, const MidiPacket& packet) {
                    if (port.surface) {
                        surface_events.push(packet); ++state.hardware_events;
                        if (packet.size >= 2 && packet.bytes[0] < 0xf0) {
                            MidiEvent event {packet.bytes[0], packet.bytes[1], packet.size == 3 ? packet.bytes[2] : uint8_t(0)};
                            state.last_midi.store((unsigned(event.status) << 16) | (unsigned(event.key) << 8) | event.value);
                            auto input = surface_input(event);
                            if (is_button(input, SurfaceButton::WheelPush) && input.pressed && state.push_exit.load()) { state.running.store(false); return; }
                            if (is_control(input, SurfaceControl::MainVolume)) state.gain.store(main_volume_gain(input.value));
                            bool moved = input.kind == SurfaceInput::Kind::Control &&
                                (is_control(input, SurfaceControl::Wheel) ? input.delta != 0 : std::chrono::steady_clock::now() >= syncing_until);
                            if (moved || input.pressed) state.last_input.store(pack_last_input(input));
                            if (input.pressed && input.kind == SurfaceInput::Kind::Button && !state.prompting.load()) recorder_button(state, SurfaceButton(input.index));
                            route(state, event);
                        }
                    } else {
                        ++state.external_events;
                        if (packet.size == 1 && packet.bytes[0] == 0xf8) ++state.clocks;
                        if (packet.size >= 2 && packet.bytes[0] < 0xf0)
                            state.queue.push({packet.bytes[0], packet.bytes[1], packet.size == 3 ? packet.bytes[2] : uint8_t(0)});
                    }
                });
                auto now = std::chrono::steady_clock::now();
                if (now >= scan) {
                    if (midi.rescan()) state.queue.push({0xb0,120,0});
                    require(!surface_required || midi.has_surface(), "Control surface disconnected");
                    MidiCatalog update {midi.json(), midi.outputs()}; catalog_updates.push(update);
                    scan = now + std::chrono::seconds(1);
                }
                if (sync_requested.exchange(false)) request_positions();
                if (audition_requested.exchange(false)) {
                    route(state, {0x99,surface_pad_notes[0],100}); ++state.synthetic_events;
                    release = now + std::chrono::milliseconds(300);
                }
                if (now >= release) { route(state, {0x89,surface_pad_notes[0],0}); ++state.synthetic_events; release = std::chrono::steady_clock::time_point::max(); }
                MidiSendRequest request;
                for (unsigned n = 0; n < 32 && midi_output.pop(request); ++n)
                    if (!midi.send(request.address.data(), request.packet)) ++state.midi_send_errors;
                recorder_lights(state);
                lights.update([&](const MidiPacket& packet) { return midi.send_surface(packet); }, 48);
                midi.flush();
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        } catch (const std::exception& error) {
            std::cerr << error.what() << '\n'; state.failed.store(true); state.running.store(false);
        }
    }
    std::string command(Shared& state, const std::string& text) {
        std::istringstream input(text); std::string action, argument;
        input >> action;
        std::getline(input >> std::ws, argument);
        if (action == "status" && argument.empty()) return status(state);
        if (action == "stock" && argument.empty()) { state.running.store(false); return "OK returning to stock app"; }
        if (action == "screenshot" && argument.empty()) { state.screenshot.store(true); return "OK screenshot requested"; }
        if (action == "audition" && argument.empty()) {
            audition_requested.store(true); return "OK audition C4";
        }
        if (action == "clock" && argument.empty()) return "OK " + clock.json();
        if (action == "surface-sync" && argument.empty()) {
            sync_requested.store(true); return "OK surface position request queued";
        }
        if (action == "power" && argument.empty()) return "OK " + power.json();
        if (action == "clock-in" && (argument == "on" || argument == "off"))
            return clock.input(argument == "on") ? "OK sync input updated" : "ERR sync input unavailable";
        if (action == "clock-out" && (argument == "0" || argument == "1"))
            return clock.output(argument == "1") ? "OK sync output updated" : "ERR sync output unavailable";
        if (action == "surface" && argument.empty()) return "OK {\"diagnostic_overflows\":" + std::to_string(surface_overflows) + ",\"state\":" + surface_json(surface) + '}';
        if (action == "surface-log") {
            uint64_t since = 0;
            if (!argument.empty() && !unsigned_value(argument, since)) return "ERR use surface-log [SEQUENCE]";
            return "OK " + surface_log_json(surface_log, since, surface_overflows);
        }
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
        if (action == "midi" && argument.empty()) return "OK {\"async_send_errors\":" + std::to_string(state.midi_send_errors.load()) + ",\"state\":" + catalog.json + '}';
        if (action == "mixer" && argument.empty()) return "OK " + mixer.json();
        if (action == "storage" && argument.empty()) return "OK {\"generation\":" + std::to_string(services.storage_generation()) + ",\"state\":" + services.storage_json() + '}';
        if (action == "hardware" && argument.empty()) return "OK {\"platform\":" + platform.json() + ",\"midi\":" + catalog.json +
            ",\"mixer\":" + mixer.json() + ",\"audio\":" + AudioDevice::devices_json() + '}';
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
        if (action == "page" && (argument == "synth" || argument == "recorder")) {
            state.recorder_page.store(argument == "recorder"); return "OK page " + argument;
        }
        if (action == "recorder" && argument.empty()) return "OK " + recorder_json(state);
        if (action == "storage-mount" || action == "storage-unmount")
            return services.mount(argument, action == "storage-mount") ? "OK storage request completed" : "ERR storage service request failed";
        unsigned value = 0;
        if (action == "gain" || action == "monitor") {
            if (!unsigned_value(argument, value) || value > 100) return "ERR use a percentage from 0 to 100";
            (action == "gain" ? state.gain : state.monitor).store(value / 100.0f); return "OK " + action + ' ' + argument;
        }
        if (action == "brightness") return unsigned_value(argument, value) && platform.brightness(value) ? "OK brightness " + argument : "ERR brightness unavailable or outside device range";
        if (action == "midi-send") {
            std::istringstream fields(argument); std::string address, bytes;
            fields >> address; std::getline(fields >> std::ws, bytes); MidiPacket packet;
            if (!parse_midi_hex(bytes, packet) || address.size() >= MidiSendRequest{}.address.size() ||
                std::find(catalog.outputs.begin(), catalog.outputs.end(), address) == catalog.outputs.end()) return "ERR invalid message or unavailable port";
            MidiSendRequest request; strcpy(request.address.data(), address.c_str()); request.packet = packet;
            return midi_output.push(request) ? "OK MIDI queued" : "ERR full output queue";
        }
        if (action == "led") return led(argument);
        if (action == "mixer-set") {
            std::istringstream fields(argument); std::string id;
            fields >> id; std::vector<long> values; long item;
            while (fields >> item && values.size() < 128) values.push_back(item);
            if (!fields.eof()) return "ERR invalid mixer values";
            return unsigned_value(id, value) && mixer.set(value, values) ? "OK mixer updated" : "ERR unavailable control, wrong value count or range";
        }
        return "ERR use status, hardware, surface, surface-log, surface-sync, push-exit, prompt, power, led, midi, midi-send, mixer, mixer-set, input, "
               "source, channels, transport, page, recorder, gain, monitor, brightness, storage, storage-mount, storage-unmount, audition, screenshot, clock, clock-in, clock-out or stock";
    }
    // Service thread only. Recording stops before any route changes, and the
    // new source is published only once its route is in place. Without the
    // I2S mixer (a host test or another --audio-input) routes are left alone.
    bool select_source(Shared& state, InputSource source) {
        state.source.store(uint8_t(InputSource::Off));
        if (source != InputSource::Usb) state.usb.stop();
        if (mixer.available()) {
            const char* profile = source == InputSource::Line ? "line" : source == InputSource::Mic ? "mic" : "off";
            if (!mixer.input(profile)) { mixer.input("off"); return false; }
        }
        if (source == InputSource::Usb && state.usb.state() == UsbInputState::Stopped) state.usb.start(usb_device);
        state.source.store(uint8_t(source));
        return true;
    }
    // MIDI thread. The buttons above the screen are soft keys for the labels
    // drawn under them, so on the synth page they only open the recorder.
    void recorder_button(Shared& state, SurfaceButton button) {
        bool page = state.recorder_page.load();
        switch (button) {
        case SurfaceButton::Sample: state.recorder_page.store(!page); break;
        case SurfaceButton::TopLeft: {
            if (page) {
                int pending = source_request.load();
                auto current = pending >= 0 ? InputSource(pending) : InputSource(state.source.load());
                source_request.store(int(next_input_source(current)));
            }
            state.recorder_page.store(true); break;
        }
        case SurfaceButton::TopMiddle:
            if (page && input_has_channels(InputSource(state.source.load())))
                state.channels.store(uint8_t(next_input_channels(InputChannels(state.channels.load()))));
            state.recorder_page.store(true); break;
        case SurfaceButton::TopRight: state.recorder_page.store(!page); break;
        case SurfaceButton::SampleRecord: state.recorder.record(); state.recorder_page.store(true); break;
        case SurfaceButton::Play: state.recorder.play(); state.recorder_page.store(true); break;
        case SurfaceButton::Stop: state.recorder.stop(); break;
        default: break;
        }
    }
    // MIDI thread. Sets the transport lights when the recorder's state calls
    // for a different light, so a manual `led` command stays until then.
    void recorder_lights(Shared& state) {
        auto mode = state.recorder.state();
        bool recording = mode == Recorder::State::Recording, playing = mode == Recorder::State::Playing;
        bool source = state.source.load() != uint8_t(InputSource::Off);
        const std::array<std::pair<SurfaceButton, ButtonLight>, 4> wanted {{
            {SurfaceButton::Sample, state.recorder_page.load() ? ButtonLight::Bright : ButtonLight::Off},
            {SurfaceButton::SampleRecord, recording ? ButtonLight::Bright : source ? ButtonLight::Dim : ButtonLight::Off},
            {SurfaceButton::Play, playing ? ButtonLight::Bright : state.recorder.length() ? ButtonLight::Dim : ButtonLight::Off},
            {SurfaceButton::Stop, recording || playing ? ButtonLight::Dim : ButtonLight::Off},
        }};
        for (unsigned n = 0; n < wanted.size(); ++n)
            if (shown_lights[n] != uint8_t(wanted[n].second) && lights.button(wanted[n].first, wanted[n].second))
                shown_lights[n] = uint8_t(wanted[n].second);
    }
    // led pad 1-16 R G B | led fader R G B | led bank A-H on|off |
    // led button NAME off|dim|bright|red-dim|red-bright | led off
    std::string led(const std::string& text) {
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
    static void route(Shared& state, MidiEvent event) {
        int pad = pad_index(event);
        if (pad < 0) return;
        unsigned kind = event.status & 0xf0;
        if (kind == 0xa0) return;
        bool down = kind == 0x90 && event.value != 0;
        if (down) { state.held.fetch_or(1u << pad); state.last_pad.store(pad); state.velocity.store(event.value); }
        else state.held.fetch_and(~(1u << pad));
        state.queue.push({uint8_t(down ? 0x90 : 0x80), pad_scale_notes[pad], down ? event.value : uint8_t(0)});
    }
    std::string socket_path;
    bool surface_required;
    std::string usb_device;
    std::atomic<int> source_request {-1}; // MIDI thread asks, service thread applies.
    std::array<uint8_t, 4> shown_lights {{0xff, 0xff, 0xff, 0xff}};
    int fd = -1;
    HardwareMidi midi;
    HardwareMixer mixer;
    Platform platform;
    SystemServices services;
    SurfaceState surface;
    SurfaceLog<1024> surface_log;
    ClockIo clock;
    PowerButton power;
    MidiCatalog catalog;
    MidiQueue<4, MidiCatalog> catalog_updates;
    MidiQueue<256, MidiPacket> surface_events;
    MidiQueue<32, MidiSendRequest> midi_output;
    SurfaceLights lights;
    std::atomic<bool> audition_requested {false}, sync_requested {false};
    unsigned surface_overflows = 0;
};

class ControlClient {
public:
    explicit ControlClient(const std::string& runtime) {
        fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
        require(fd >= 0, "Create client socket");
        local.sun_family = remote.sun_family = AF_UNIX;
        std::string local_path = runtime + "/client-" + std::to_string(getpid()) + ".sock", remote_path = runtime + "/control.sock";
        require(local_path.size() < sizeof(local.sun_path) && remote_path.size() < sizeof(remote.sun_path), "Runtime path is too long");
        strcpy(local.sun_path, local_path.c_str()); strcpy(remote.sun_path, remote_path.c_str());
        unlink(local.sun_path);
        if (bind(fd, reinterpret_cast<sockaddr*>(&local), sizeof(local)) != 0) { close(fd); throw std::runtime_error("Bind client socket"); }
    }
    ~ControlClient() { unlink(local.sun_path); close(fd); }
    ControlClient(const ControlClient&) = delete;
    ControlClient& operator=(const ControlClient&) = delete;
    // Empty when the app does not answer. A late reply to an earlier request
    // is discarded by draining before each send.
    std::string request(const std::string& command) {
        char reply[32768];
        while (recv(fd, reply, sizeof(reply), MSG_DONTWAIT) > 0) {}
        if (sendto(fd, command.data(), command.size(), 0, reinterpret_cast<sockaddr*>(&remote), sizeof(remote)) < 0) return {};
        pollfd ready {fd, POLLIN, 0};
        if (poll(&ready, 1, 3500) <= 0) return {};
        auto size = recv(fd, reply, sizeof(reply) - 1, 0);
        return size > 0 ? std::string(reply, size_t(size)) : std::string();
    }
private:
    int fd = -1;
    sockaddr_un local {}, remote {};
};

int control(const std::string& runtime, const std::string& command) {
    auto reply = ControlClient(runtime).request(command);
    if (reply.empty()) { std::cerr << "No reply from sample-base\n"; return 1; }
    std::cout << reply << '\n';
    return reply.compare(0, 2, "OK") != 0;
}
// One command per stdin line and one reply per stdout line, so a single SSH
// session can drive interactive tools such as control calibration.
int control_stream(const std::string& runtime) {
    ControlClient client(runtime);
    for (std::string command; std::getline(std::cin, command);) {
        auto reply = client.request(command);
        if (reply.empty()) { std::cout << "ERR no reply from sample-base" << std::endl; return 1; }
        std::replace(reply.begin(), reply.end(), '\n', ' ');
        std::cout << reply << std::endl;
    }
    return 0;
}

std::string source_detail(const Shared& state, InputSource source) {
    bool capture = state.audio.capture_active.load();
    switch (source) {
    case InputSource::Line: return capture ? "Line inputs L and R" : "Audio capture is unavailable";
    case InputSource::Mic: return capture ? "Built-in microphone, recorded mono" : "Audio capture is unavailable";
    case InputSource::Usb: break;
    default: return "The left button above the screen chooses an input";
    }
    switch (state.usb.state()) {
    case UsbInputState::Missing: return "No USB audio gadget card was found";
    case UsbInputState::Waiting: return "USB-C: play audio from the computer";
    case UsbInputState::Failed: return "USB-C audio could not be opened";
    case UsbInputState::Running: {
        char text[96];
        snprintf(text, sizeof(text), "USB-C: %.1f kHz, %u-bit, %u ch%s", state.usb.rate() / 1000.0, state.usb.bits(),
                 state.usb.channels(), state.usb.rate() == rate ? "" : ", converted to 44.1 kHz");
        return text;
    }
    default: return "USB-C input is starting";
    }
}
RecorderView recorder_view(const Shared& state) {
    RecorderView view;
    const auto& recorder = state.recorder;
    view.source = state.source.load(); view.channels = state.channels.load(); view.state = uint8_t(recorder.state());
    view.frames = recorder.length(); view.position = recorder.position(); view.capacity = recorder.capacity(); view.rate = rate;
    view.left = state.input_left.load(); view.right = state.input_right.load();
    // While recording the waveform spans at least ten seconds, so it grows.
    view.scale = view.state == uint8_t(Recorder::State::Recording) ? std::max(view.frames, 10 * rate) : std::max(view.frames, 1u);
    for (unsigned column = 0; column < RecorderView::columns; ++column) {
        unsigned first = unsigned(uint64_t(column) * view.scale / RecorderView::columns);
        unsigned last = unsigned(uint64_t(column + 1) * view.scale / RecorderView::columns);
        view.waveform[column] = recorder.overview_peak(first, std::max(last, first + 1));
    }
    view.detail = source_detail(state, InputSource(view.source));
    return view;
}
// A few seconds of a decaying drum-like take, for the offline screen preview.
RecorderView preview_recorder() {
    RecorderView view;
    view.source = uint8_t(InputSource::Line); view.channels = uint8_t(InputChannels::Stereo);
    view.state = uint8_t(Recorder::State::Playing);
    view.frames = view.scale = 286650; view.position = 101430; view.capacity = 60 * rate;
    view.left = 0.42f; view.right = 0.18f; view.detail = "Line inputs L and R";
    for (unsigned column = 0; column < RecorderView::columns; ++column) {
        double beat = std::fmod(column / 37.0, 1.0), envelope = std::exp(-beat * 5.0);
        view.waveform[column] = uint8_t(std::clamp(220.0 * envelope * (0.75 + 0.25 * std::sin(column * 1.7)) + 6, 0.0, 255.0));
    }
    return view;
}

void save_png(const juce::Image& image, const std::string& path) {
    juce::FileOutputStream output {juce::File(path)};
    require(output.openedOk() && output.setPosition(0) && output.truncate().wasOk() &&
            juce::PNGImageFormat().writeImageToStream(image, output), "Write screen preview");
}
void little(std::ofstream& file, uint32_t value, unsigned bytes) {
    for (unsigned n = 0; n < bytes; ++n) file.put(char((value >> (n * 8)) & 255));
}
int self_test(const std::string& output) {
    Synth engine;
    constexpr unsigned frames = rate * 2;
    std::ofstream file(output, std::ios::binary);
    require(bool(file), "Create test audio");
    file.write("RIFF", 4); little(file, 36 + frames * 4, 4); file.write("WAVEfmt ", 8);
    little(file, 16, 4); little(file, 1, 2); little(file, 2, 2); little(file, rate, 4);
    little(file, rate * 4, 4); little(file, 4, 2); little(file, 16, 2);
    file.write("data", 4); little(file, frames * 4, 4);
    std::array<int32_t, block * 2> samples {};
    engine.midi({0x90,60,100});
    float peak = 0; double energy = 0, tail = 0;
    bool released = false;
    for (unsigned offset = 0; offset < frames; offset += block) {
        if (!released && offset >= rate / 2) { engine.midi({0x80,60,0}); released = true; }
        peak = std::max(peak, engine.render(samples.data()));
        for (unsigned i = 0; i < std::min(block, frames - offset) * 2; ++i) {
            double value = samples[i] / 2147483648.0;
            energy += value * value;
            if (offset > rate * 3 / 2) tail += value * value;
            little(file, uint16_t(samples[i] >> 16), 2);
        }
    }
    double rms = std::sqrt(energy / (frames * 2)), tail_rms = std::sqrt(tail / rate);
    std::cout << "SELF_TEST peak=" << peak << " rms=" << rms << " tail_rms=" << tail_rms << '\n';
    require(file.good() && peak > 0.001f && rms > 0.0001 && tail_rms < 0.001, "Synth note/release self-test failed");
    return 0;
}
} // namespace

int main(int argc, char** argv) {
    umask(0077);
    try {
        const std::string font = juce::File("/usr/share/fonts/ttf/LiberationSans-Regular.ttf").existsAsFile()
            ? "/usr/share/fonts/ttf/LiberationSans-Regular.ttf"
            : "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf";
        if (argc == 3 && !strcmp(argv[1], "--self-test")) return self_test(argv[2]);
        if ((argc == 3 || argc == 4) && !strcmp(argv[1], "--preview")) {
            Screen screen(font); ScreenState state; state.last_pad = 0; state.velocity = 100; state.held = 1; state.peak = 0.17f;
            state.last_input = describe_last_input(pack_last_input(InputKind::Control, uint8_t(SurfaceControl::Wheel), uint8_t(-1)));
            if (argc == 4) state.prompt = argv[3];
            save_png(screen.draw(state), argv[2]); return 0;
        }
        if (argc == 3 && !strcmp(argv[1], "--preview-recorder")) {
            Screen screen(font); ScreenState state; state.recorder_page = true; state.recorder = preview_recorder();
            state.last_input = surface_buttons[size_t(SurfaceButton::Play)].name;
            save_png(screen.draw(state), argv[2]); return 0;
        }
        std::string output = "hw:I2S,0", input = "hw:I2S,0", runtime = "/run/sample-base", usb_input = "auto";
        bool headless = false, surface_required = true; unsigned seconds = 0;
        for (int i = 1; i < argc; ++i) {
            std::string option = argv[i];
            if (option == "--headless") headless = true;
            else if (option == "--no-surface") surface_required = false;
            else if (option == "--hardware" && argc == 2) {
                Platform platform; HardwareMixer mixer; SystemServices services;
                std::cout << "{\"platform\":" << platform.json() << ",\"audio\":" << AudioDevice::devices_json()
                          << ",\"mixer\":" << mixer.json() << ",\"storage\":" << services.storage_json() << ",\"midi\":[";
                bool first = true;
                for (const auto& port : HardwareMidi::enumerate()) {
                    if (!first) std::cout << ',';
                    first = false;
                    std::cout << "{\"address\":" << json_string(port.address) << ",\"card\":" << json_string(port.card)
                              << ",\"name\":" << json_string(port.name) << ",\"input\":" << (port.input ? "true" : "false")
                              << ",\"output\":" << (port.output ? "true" : "false") << '}';
                }
                std::cout << "]}\n"; return 0;
            } else if (option == "--ctl-stream") {
                return control_stream(runtime);
            } else if (option == "--ctl") {
                require(i + 1 < argc, "Control command is missing");
                std::string command;
                for (++i; i < argc; ++i) { if (!command.empty()) command += ' '; command += argv[i]; }
                return control(runtime, command);
            } else {
                require(i + 1 < argc, "Missing option value");
                std::string value = argv[++i];
                if (option == "--audio-output") output = value;
                else if (option == "--audio-input") input = value;
                else if (option == "--usb-input") usb_input = value;
                else if (option == "--runtime-dir") runtime = value;
                else if (option == "--seconds") require(unsigned_value(value, seconds) && seconds <= 86400, "Invalid session duration");
                else throw std::runtime_error("Unknown option: " + option);
            }
        }
        signal(SIGINT, signal_handler); signal(SIGTERM, signal_handler);
        Synth engine;
        std::unique_ptr<Screen> screen; std::unique_ptr<Display> display;
        if (!headless) { screen = std::make_unique<Screen>(font); display = std::make_unique<Display>(); }
        AudioDevice audio(output, input); Controller controller(runtime, surface_required, usb_input);
        Shared state; RenderContext context {state, engine};
        auto deadline = seconds ? std::chrono::steady_clock::now() + std::chrono::seconds(seconds) : std::chrono::steady_clock::time_point::max();
        std::thread audio_thread, control_thread;
        try {
            audio_thread = std::thread([&] { audio.run(state.running, state.failed, state.audio, render_audio, &context); });
            control_thread = std::thread([&] { controller.run(state); });
            std::cout << "READY sample-base JUCE " << JUCE_MAJOR_VERSION << '.' << JUCE_MINOR_VERSION << '.' << JUCE_BUILDNUMBER << std::endl;
            unsigned refusals_seen = 0;
            auto hint_until = std::chrono::steady_clock::time_point::min();
            while (state.running.load() && !interrupted && std::chrono::steady_clock::now() < deadline) {
                if (screen && display) {
                    ScreenState view {state.held.load(), state.last_pad.load(), state.velocity.load(), state.audio.peak.load(), state.audio.xruns.load(), {}, describe_last_input(state.last_input.load()), state.recorder_page.load(), {}};
                    { std::lock_guard<std::mutex> lock(state.prompt_lock); view.prompt = state.prompt; }
                    if (view.recorder_page) view.recorder = recorder_view(state);
                    auto now = std::chrono::steady_clock::now();
                    if (state.recorder.refused() != refusals_seen) { refusals_seen = state.recorder.refused(); hint_until = now + std::chrono::milliseconds(2500); }
                    if (now < hint_until) { view.recorder.detail = "Choose an input first: left button above the screen"; view.recorder.hint = true; }
                    auto image = screen->draw(view);
                    display->present(image);
                    if (state.screenshot.exchange(false)) save_png(image, runtime + "/screen.png");
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(75));
            }
        } catch (...) {
            state.running.store(false);
            if (audio_thread.joinable()) audio_thread.join();
            if (control_thread.joinable()) control_thread.join();
            throw;
        }
        state.running.store(false); audio_thread.join(); control_thread.join();
        std::cout << status(state) << std::endl;
        return state.failed.load() ? 1 : 0;
    } catch (const std::exception& error) {
        std::cerr << "sample-base: " << error.what() << '\n'; return 1;
    }
}
