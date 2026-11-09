// SPDX-License-Identifier: AGPL-3.0-only
#include "display.h"
#include "midi.h"
#include "screen.h"
#include "synth.h"
#include <alsa/asoundlib.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <poll.h>
#include <unistd.h>
#include <pthread.h>
#include <algorithm>
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

namespace {
constexpr unsigned rate = Synth::sample_rate, block = Synth::block_size;
constexpr char socket_path[] = "/run/sample-base/control.sock";
constexpr int scale[] = {60,62,64,65,67,69,71,72,74,76,77,79,81,83,84,86};
volatile std::sig_atomic_t interrupted = 0;
void signal_handler(int) { interrupted = 1; }
void require(bool ok, const std::string& message) { if (!ok) throw std::runtime_error(message); }
void alsa(int error, const char* action) {
    if (error < 0) throw std::runtime_error(std::string(action) + ": " + snd_strerror(error));
}

struct Shared {
    MidiQueue<> queue;
    std::atomic<bool> running {true}, failed {false}, screenshot {false};
    std::atomic<uint32_t> held {0}, last_midi {0};
    std::atomic<int> last_pad {-1}, velocity {0};
    std::atomic<unsigned> xruns {0}, hardware_events {0}, synthetic_events {0}, overruns {0};
    std::atomic<uint64_t> frames {0}, sounding_blocks {0};
    std::atomic<float> peak {0};
};

class Audio {
public:
    Audio() {
        alsa(snd_pcm_open(&pcm, "hw:I2S,0", SND_PCM_STREAM_PLAYBACK, SND_PCM_NONBLOCK), "Open I2S output");
        try {
            snd_pcm_hw_params_t* hw; snd_pcm_hw_params_alloca(&hw);
            alsa(snd_pcm_hw_params_any(pcm, hw), "Read audio capabilities");
            alsa(snd_pcm_hw_params_set_access(pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED), "Set interleaved audio");
            alsa(snd_pcm_hw_params_set_format(pcm, hw, SND_PCM_FORMAT_S32_LE), "Set S32 audio");
            alsa(snd_pcm_hw_params_set_channels(pcm, hw, 2), "Set stereo audio");
            alsa(snd_pcm_hw_params_set_rate(pcm, hw, rate, 0), "Set 44100 Hz");
            snd_pcm_uframes_t period = block, buffer = block * 4;
            alsa(snd_pcm_hw_params_set_period_size_near(pcm, hw, &period, nullptr), "Set audio period");
            alsa(snd_pcm_hw_params_set_buffer_size_near(pcm, hw, &buffer), "Set audio buffer");
            alsa(snd_pcm_hw_params(pcm, hw), "Apply audio format");
            snd_pcm_sw_params_t* sw; snd_pcm_sw_params_alloca(&sw);
            alsa(snd_pcm_sw_params_current(pcm, sw), "Read audio software settings");
            alsa(snd_pcm_sw_params_set_start_threshold(pcm, sw, buffer - period), "Set audio prefill");
            alsa(snd_pcm_sw_params_set_avail_min(pcm, sw, period), "Set audio wakeup");
            alsa(snd_pcm_sw_params(pcm, sw), "Apply audio software settings");
            alsa(snd_pcm_prepare(pcm), "Prepare audio");
            std::cout << "AUDIO rate=" << rate << " period=" << period << " buffer=" << buffer << std::endl;
        } catch (...) { snd_pcm_close(pcm); pcm = nullptr; throw; }
    }
    ~Audio() { if (pcm) { snd_pcm_drop(pcm); snd_pcm_close(pcm); } }
    void run(Shared& state, Synth& engine) {
        sched_param priority {}; priority.sched_priority = 30;
        int scheduling = pthread_setschedparam(pthread_self(), SCHED_FIFO, &priority);
        if (scheduling) std::cerr << "Audio priority: " << strerror(scheduling) << '\n';
        try {
            std::array<int32_t, block * 2> output {};
            while (state.running.load()) {
                MidiEvent event;
                if (state.queue.take_overflow()) {
                    for (unsigned n = 0; n < 256 && state.queue.pop(event); ++n) {}
                    engine.midi({0xb0,120,0}); state.overruns.fetch_add(1);
                }
                for (unsigned n = 0; n < 256 && state.queue.pop(event); ++n) engine.midi(event);
                float peak = engine.render(output.data());
                state.peak.store(peak);
                if (peak > 0.0001f) state.sounding_blocks.fetch_add(1);
                unsigned offset = 0;
                while (offset < block && state.running.load()) {
                    auto written = snd_pcm_writei(pcm, output.data() + offset * 2, block - offset);
                    if (written == -EAGAIN || written == 0) {
                        int ready = snd_pcm_wait(pcm, 100);
                        if (ready < 0) alsa(ready, "Wait for audio");
                    } else if (written < 0) {
                        state.xruns.fetch_add(1);
                        alsa(snd_pcm_recover(pcm, int(written), 1), "Recover audio output");
                    } else {
                        offset += unsigned(written);
                        state.frames.fetch_add(uint64_t(written));
                    }
                }
            }
            engine.midi({0xb0,120,0});
        } catch (const std::exception& error) {
            std::cerr << error.what() << '\n'; state.failed.store(true); state.running.store(false);
        }
    }
private:
    snd_pcm_t* pcm = nullptr;
};

std::string status(const Shared& state) {
    char reply[512];
    snprintf(reply, sizeof(reply), "OK frames=%llu sounding_blocks=%llu hardware_events=%u synthetic_events=%u "
             "held=%04x last_pad=%d velocity=%d last_midi=%06x peak=%.6f xruns=%u queue_overruns=%u",
             static_cast<unsigned long long>(state.frames.load()), static_cast<unsigned long long>(state.sounding_blocks.load()),
             state.hardware_events.load(), state.synthetic_events.load(), state.held.load(), state.last_pad.load() + 1,
             state.velocity.load(), state.last_midi.load(), double(state.peak.load()), state.xruns.load(), state.overruns.load());
    return reply;
}

class Controller {
public:
    Controller() {
        alsa(snd_rawmidi_open(&midi, nullptr, "hw:Surface,0", SND_RAWMIDI_NONBLOCK), "Open control surface");
        try {
            fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
            require(fd >= 0, "Create control socket");
            sockaddr_un address {}; address.sun_family = AF_UNIX;
            strcpy(address.sun_path, socket_path);
            unlink(socket_path);
            require(bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0, "Bind control socket");
            chmod(socket_path, 0600);
        } catch (...) { if (fd >= 0) close(fd); snd_rawmidi_close(midi); throw; }
    }
    ~Controller() { close(fd); unlink(socket_path); snd_rawmidi_close(midi); }
    void run(Shared& state) {
        try {
            MidiParser parser;
            int count = snd_rawmidi_poll_descriptors_count(midi);
            require(count > 0 && count < 16, "Invalid control surface poll descriptors");
            std::array<pollfd, 17> polls {};
            snd_rawmidi_poll_descriptors(midi, polls.data(), unsigned(count));
            polls[count] = {fd, POLLIN, 0};
            auto release = std::chrono::steady_clock::time_point::max();
            while (state.running.load()) {
                int result = poll(polls.data(), unsigned(count + 1), 5);
                if (result < 0 && errno != EINTR) throw std::runtime_error("Control poll failed");
                unsigned char bytes[256];
                for (unsigned batch = 0; batch < 8; ++batch) {
                    int length = int(snd_rawmidi_read(midi, bytes, sizeof(bytes)));
                    if (length == -EAGAIN || length == 0) break;
                    alsa(length, "Read control surface");
                    for (int i = 0; i < length; ++i) parser.feed(bytes[i], [&](MidiEvent event) {
                        state.hardware_events.fetch_add(1);
                        state.last_midi.store((unsigned(event.status) << 16) | (unsigned(event.key) << 8) | event.value);
                        unsigned kind = event.status & 0xf0;
                        // Push is the main action. No stock event handlers are involved.
                        if (event.key == 111 && event.value && (kind == 0x90 || kind == 0xb0)) {
                            state.running.store(false); return;
                        }
                        route(state, event);
                    });
                }
                if (polls[count].revents & POLLIN) {
                    sockaddr_un sender {}; socklen_t size = sizeof(sender);
                    char request[128] {};
                    auto length = recvfrom(fd, request, sizeof(request) - 1, 0, reinterpret_cast<sockaddr*>(&sender), &size);
                    if (length > 0) {
                        std::string reply;
                        if (!strcmp(request, "status")) reply = status(state);
                        else if (!strcmp(request, "stock")) { state.running.store(false); reply = "OK returning to stock app"; }
                        else if (!strcmp(request, "screenshot")) { state.screenshot.store(true); reply = "OK screenshot requested"; }
                        else if (!strcmp(request, "audition")) {
                            route(state, {0x90,36,100}); state.synthetic_events.fetch_add(1);
                            release = std::chrono::steady_clock::now() + std::chrono::milliseconds(300);
                            reply = "OK audition C4";
                        } else reply = "ERR use status, audition, screenshot or stock";
                        sendto(fd, reply.data(), reply.size(), MSG_DONTWAIT, reinterpret_cast<sockaddr*>(&sender), size);
                    }
                }
                if (std::chrono::steady_clock::now() >= release) {
                    route(state, {0x80,36,0}); state.synthetic_events.fetch_add(1);
                    release = std::chrono::steady_clock::time_point::max();
                }
            }
        } catch (const std::exception& error) {
            std::cerr << error.what() << '\n'; state.failed.store(true); state.running.store(false);
        }
    }
private:
    static void route(Shared& state, MidiEvent event) {
        int pad = pad_index(event);
        if (pad < 0) return;
        unsigned kind = event.status & 0xf0;
        if (kind == 0xa0) return;
        bool down = kind == 0x90 && event.value != 0;
        if (down) {
            state.held.fetch_or(1u << pad); state.last_pad.store(pad); state.velocity.store(event.value);
        } else state.held.fetch_and(~(1u << pad));
        state.queue.push({uint8_t(down ? 0x90 : 0x80), uint8_t(scale[pad]), down ? event.value : uint8_t(0)});
    }
    snd_rawmidi_t* midi = nullptr;
    int fd = -1;
};

int control(const std::string& command) {
    int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    require(fd >= 0, "Create client socket");
    sockaddr_un local {}, remote {}; local.sun_family = remote.sun_family = AF_UNIX;
    snprintf(local.sun_path, sizeof(local.sun_path), "/run/sample-base/client-%d.sock", int(getpid()));
    strcpy(remote.sun_path, socket_path);
    int result = 1;
    if (bind(fd, reinterpret_cast<sockaddr*>(&local), sizeof(local)) == 0 &&
        sendto(fd, command.data(), command.size(), 0, reinterpret_cast<sockaddr*>(&remote), sizeof(remote)) >= 0) {
        pollfd ready {fd, POLLIN, 0};
        if (poll(&ready, 1, 1500) > 0) {
            char reply[1024] {}; auto size = recv(fd, reply, sizeof(reply) - 1, 0);
            if (size > 0) { std::cout << reply << '\n'; result = strncmp(reply, "OK", 2) != 0; }
        }
    }
    unlink(local.sun_path); close(fd);
    if (result) std::cerr << "No reply from sample-base\n";
    return result;
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
        if (argc == 3 && !strcmp(argv[1], "--ctl")) return control(argv[2]);
        if (argc == 3 && !strcmp(argv[1], "--self-test")) return self_test(argv[2]);
        if (argc == 3 && !strcmp(argv[1], "--preview")) {
            Screen screen(font); ScreenState state; state.last_pad = 0; state.velocity = 100; state.held = 1; state.peak = 0.17f;
            save_png(screen.draw(state), argv[2]); return 0;
        }
        require(argc == 1, "Use --self-test WAV, --preview PNG or --ctl COMMAND");
        signal(SIGINT, signal_handler); signal(SIGTERM, signal_handler);
        Synth engine; Screen screen(font); Display display; Audio audio; Controller controller;
        Shared state;
        std::thread audio_thread, control_thread;
        try {
            audio_thread = std::thread([&] { audio.run(state, engine); });
            control_thread = std::thread([&] { controller.run(state); });
            std::cout << "READY sample-base JUCE " << JUCE_MAJOR_VERSION << '.' << JUCE_MINOR_VERSION << '.' << JUCE_BUILDNUMBER << std::endl;
            while (state.running.load() && !interrupted) {
                ScreenState view {state.held.load(), state.last_pad.load(), state.velocity.load(), state.peak.load(), state.xruns.load()};
                auto image = screen.draw(view);
                display.present(image);
                if (state.screenshot.exchange(false)) save_png(image, "/run/sample-base/screen.png");
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
