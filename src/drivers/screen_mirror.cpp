// SPDX-License-Identifier: AGPL-3.0-only
#include "drivers/screen_mirror.h"
#include "drivers/jpeg_encoder.h"
#include "drivers/kernel_module.h"
#include "drivers/mirror_frame.h"
#include <juce_graphics/juce_graphics.h>
#include <fcntl.h>
#include <linux/usb/ch9.h>
#include <linux/usb/g_uvc.h>
#include <linux/usb/video.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/timerfd.h>
#include <unistd.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <iostream>
#include <mutex>
#include <thread>
#include <vector>

// Weak: a build without the modules (base.mk) has no Screen Mirror.
extern "C" const unsigned char sample_base_uvc_module[] __attribute__((weak));
extern "C" const unsigned char sample_base_uvc_module_end[] __attribute__((weak));
extern "C" const unsigned char sample_base_vepu_module[] __attribute__((weak));
extern "C" const unsigned char sample_base_vepu_module_end[] __attribute__((weak));

const char* mirror_state_name(MirrorState state) {
    switch (state) {
    case MirrorState::Off: return "off";
    case MirrorState::Starting: return "starting";
    case MirrorState::Waiting: return "waiting";
    case MirrorState::Streaming: return "streaming";
    case MirrorState::CardShared: return "card-shared";
    case MirrorState::Failed: return "failed";
    }
    return "off";
}

namespace {
using Clock = std::chrono::steady_clock;
constexpr unsigned interval_100ns = 333333;  // 30 pictures a second, in UVC's units
constexpr unsigned buffer_count = 4;
// The screen's flat colours and hard edges: 95 keeps them clean. A still settings page is about 55 kB at
// 1280 x 720, a playing project about 340 kB at 1920 x 1440 (three packets a microframe carry that at 30 fps).
constexpr int jpeg_quality = 95;

int xioctl(int fd, unsigned long request, void* argument) {
    int result;
    do result = ioctl(fd, request, argument); while (result < 0 && errno == EINTR);
    return result;
}

unsigned frame_bytes(const MirrorFormat& f) { return f.frame_bytes(); }
} // namespace

struct ScreenMirror::Worker {
    // Shared with the other threads.
    mutable std::mutex lock;
    ScreenMirrorMode wanted;
    std::vector<uint32_t> offered = std::vector<uint32_t>(size_t(mirror_frame::screen_width) * mirror_frame::screen_height, 0xff000000u);
    uint64_t offered_serial = 0;
    std::condition_variable offered_changed; // wakes the JPEG maker
    std::atomic<bool> taking {false}, need_frame {false}, running {true};
    mutable std::mutex status_lock;
    ScreenMirrorStatus shown;

    // The worker thread's own.
    ScreenMirrorMode applied;        // what the gadget has now (on: the webcam linked for that size)
    Clock::time_point settle_until;  // a webcam an earlier run left: no change until a stream it had could resume
    MirrorFormat format;
    bool fallback = false;
    int fd = -1, timer = -1;
    bool streaming = false;
    std::array<void*, buffer_count> memory {};
    std::array<size_t, buffer_count> lengths {};
    unsigned buffers = 0;
    std::vector<unsigned> idle;      // buffers back from the computer, to fill
    JpegEncoder encoder;
    std::vector<uint32_t> screen = std::vector<uint32_t>(size_t(mirror_frame::screen_width) * mirror_frame::screen_height, 0xff000000u);
    std::vector<uint8_t> payload;    // the picture as sent: YUYV or a JPEG file
    uint64_t payload_serial = ~0ull;
    uvc_streaming_control probe {}, commit {};
    int pending_control = 0;         // the control a SET_CUR's data is for
    Clock::time_point retry = Clock::now();

    // JPEG sizes: a thread of their own makes the pictures, as fast as the encoder takes them (the CPU filling one
    // slot while the encoder makes the other), and leaves the newest here; this thread sends whatever is newest at
    // every tick, so the stream keeps its 30 pictures a second whatever one costs to make.
    std::thread maker;
    std::atomic<bool> making {false};
    std::mutex jpeg_lock;
    std::vector<uint8_t> jpeg;       // the newest JPEG made
    std::atomic<unsigned> made_count {0}, made_max_us {0};
    std::atomic<uint64_t> made_us {0};
    std::thread thread;

    explicit Worker(const ScreenMirrorMode& initial) : wanted(initial) {
        taking.store(initial.on); need_frame.store(initial.on);
        thread = std::thread([this] { run(); });
    }
    ~Worker() { running.store(false); thread.join(); }

    void set_state(MirrorState state, const std::string& error = {}) {
        std::lock_guard<std::mutex> guard(status_lock);
        if (state != shown.state || error != shown.error) {
            std::cout << "Screen mirror: " << mirror_state_name(state);
            if (state != MirrorState::Off) std::cout << ' ' << format.width << 'x' << format.height << (format.mjpeg ? " MJPEG" : " YUYV");
            if (!error.empty()) std::cout << " (" << error << ')';
            std::cout << std::endl;
        }
        shown.state = state; shown.error = error; shown.format = format; shown.fallback = fallback;
    }
    MirrorState state() const { std::lock_guard<std::mutex> guard(status_lock); return shown.state; }
    void count_frame() { std::lock_guard<std::mutex> guard(status_lock); ++shown.frames; }
    void set_held(bool held) {
        std::lock_guard<std::mutex> guard(status_lock);
        if (held && !shown.held) std::cout << "Screen mirror: a computer shows the camera; the change waits until it closes it" << std::endl;
        shown.held = held;
    }

    // A computer is showing the camera: the marker tells the gadget's other users (the card's lending, the USB
    // rate) and install/restore-stock.sh to leave the webcam in place (gadget_mirror_live()).
    void mark_live(bool live) {
        if (!live) { unlink(mirror_live_marker); return; }
        int marker = open(mirror_live_marker, O_WRONLY | O_CREAT | O_CLOEXEC, 0644);
        if (marker >= 0) close(marker);
    }

    // ---- Modules, encoder and gadget ----

    bool load_webcam(std::string& error) {
        if (kernel_module::loaded("sample_base_uvc")) return true;
        // The module was built without the kernel's symbol list: its videobuf2 dependencies go in first.
        if (!kernel_module::probe({"videobuf2-v4l2", "videobuf2-vmalloc", "videobuf2-dma-sg"}, &error)) return false;
        return kernel_module::load("sample_base_uvc", sample_base_uvc_module, sample_base_uvc_module_end, &error);
    }

    bool start_encoder(std::string& error) {
        if (!kernel_module::load("sample_base_vepu", sample_base_vepu_module, sample_base_vepu_module_end, &error)) return false;
        if (!kernel_module::loaded("hantro_vpu") && !kernel_module::probe({"hantro-vpu"}, &error)) return false;
        // hantro registers its device a moment after it loads.
        for (int attempt = 0; attempt < 20 && running.load(); ++attempt) {
            if (encoder.open(format.width, format.height, jpeg_quality, error)) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        return false;
    }

    void apply(const ScreenMirrorMode& mode) {
        close_device();
        encoder.close();
        payload.clear(); payload_serial = ~0ull;
        std::string error;
        if (!mode.on) {
            if (!gadget_remove_mirror(error)) { set_state(MirrorState::Failed, error); return; }
            kernel_module::unload("sample_base_uvc");
            set_state(MirrorState::Off);
            return;
        }
        format = {mode.width, mode.height, !ScreenMirror::uncompressed(mode.width)};
        fallback = false;
        set_state(MirrorState::Starting);
        if (!load_webcam(error)) { set_state(MirrorState::Failed, error); return; }
        if (format.mjpeg && !start_encoder(error)) {
            std::cout << "Screen mirror: " << error << ", 640 x 480 instead" << std::endl;
            format = {640, 480, false}; fallback = true;
        }
        link();
    }

    // At start: a webcam an earlier run left in the gadget (Melodee restarted) is taken as it is, for its size, so a
    // computer showing it keeps it (kernel/uvc tells the new process the stream goes on); changes wait as below.
    void adopt() {
        MirrorFormat linked;
        if (!gadget_mirror_format(linked)) { applied = {}; set_state(MirrorState::Off); return; }
        format = linked; fallback = false;
        applied = {true, linked.width, linked.height};
        // Made by an older build (another packet size): made anew, once no computer shows it.
        if (!gadget_mirror_made_for(linked)) applied.width = 0;
        std::string error;
        if (format.mjpeg) start_encoder(error);
        settle_until = Clock::now() + std::chrono::milliseconds(1500);
        set_state(gadget_mirror_linked() ? MirrorState::Waiting : MirrorState::CardShared);
    }

    void link() {
        std::string error;
        switch (gadget_add_mirror(format, error)) {
        case MirrorLink::Linked: set_state(MirrorState::Waiting); break;
        case MirrorLink::CardShared: set_state(MirrorState::CardShared); break;
        case MirrorLink::Failed: set_state(MirrorState::Failed, error); break;
        }
    }

    // ---- The webcam's video device ----

    bool open_device() {
        for (int n = 0; n < 32 && fd < 0; ++n) {
            std::string path = "/dev/video" + std::to_string(n);
            int candidate = open(path.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
            if (candidate < 0) continue;
            v4l2_capability capability {};
            if (xioctl(candidate, VIDIOC_QUERYCAP, &capability) == 0 && std::strcmp(reinterpret_cast<const char*>(capability.driver), "g_uvc") == 0)
                fd = candidate;
            else close(candidate);
        }
        if (fd < 0) return false;
        // Taking SETUP makes this the process that answers the computer (kernel/uvc refuses it until then).
        for (unsigned type : {UVC_EVENT_CONNECT, UVC_EVENT_DISCONNECT, UVC_EVENT_STREAMON, UVC_EVENT_STREAMOFF, UVC_EVENT_DATA, UVC_EVENT_SETUP}) {
            v4l2_event_subscription subscription {};
            subscription.type = type;
            if (xioctl(fd, VIDIOC_SUBSCRIBE_EVENT, &subscription) < 0) {
                std::string error = std::string("webcam events: ") + std::strerror(errno);
                close(fd); fd = -1;
                set_state(MirrorState::Failed, error);
                return false;
            }
        }
        set_format();
        probe = commit = defaults();
        set_state(MirrorState::Waiting);
        // A stream the computer kept through a restart comes as a STREAMON at once (kernel/uvc); without one the
        // marker an earlier run left is stale.
        events();
        if (fd >= 0 && !streaming) mark_live(false);
        return true;
    }

    void set_format() {
        v4l2_format f {};
        f.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
        f.fmt.pix.width = format.width; f.fmt.pix.height = format.height;
        f.fmt.pix.pixelformat = format.mjpeg ? V4L2_PIX_FMT_MJPEG : V4L2_PIX_FMT_YUYV;
        f.fmt.pix.field = V4L2_FIELD_NONE;
        f.fmt.pix.bytesperline = format.mjpeg ? 0 : format.width * 2;
        f.fmt.pix.sizeimage = frame_bytes(format);
        xioctl(fd, VIDIOC_S_FMT, &f);
    }

    // The device closed without STREAMOFF. A computer still streaming keeps the camera and sees its picture stand
    // still; the next process opening the device sends again (kernel/uvc), or the computer stops by itself.
    void close_device() {
        stop_maker();
        unmap();
        if (fd >= 0) close(fd);
        fd = -1; streaming = false;
    }

    void unmap() {
        for (unsigned n = 0; n < buffers; ++n) if (memory[n] && memory[n] != MAP_FAILED) munmap(memory[n], lengths[n]);
        memory.fill(nullptr); buffers = 0; idle.clear();
    }

    // ---- What the computer asks ----

    uvc_streaming_control defaults() const {
        uvc_streaming_control c {};
        c.bmHint = 1; c.bFormatIndex = 1; c.bFrameIndex = 1;
        c.dwFrameInterval = interval_100ns;
        c.dwMaxVideoFrameSize = frame_bytes(format);
        c.dwMaxPayloadTransferSize = format.packet_bytes(); // a microframe's, the endpoint's packets together
        c.bmFramingInfo = 3; c.bPreferedVersion = 1; c.bMinVersion = 1; c.bMaxVersion = 1;
        return c;
    }

    // kernel/uvc numbers the interfaces 0 (control) and 1 (streaming) in its events, whatever the gadget made them.
    uvc_request_data answer(const usb_ctrlrequest& request) {
        uvc_request_data response {};
        response.length = -EL2HLT; // a stall, unless answered below
        unsigned interface = request.wIndex & 0xff, entity = request.wIndex >> 8, control = request.wValue >> 8;
        unsigned length = request.wLength;
        if ((request.bRequestType & USB_TYPE_MASK) != USB_TYPE_CLASS || (request.bRequestType & USB_RECIP_MASK) != USB_RECIP_INTERFACE)
            return response;
        if (interface == 1 && entity == 0 && (control == UVC_VS_PROBE_CONTROL || control == UVC_VS_COMMIT_CONTROL)) {
            uvc_streaming_control c {};
            switch (request.bRequest) {
            case UVC_SET_CUR: pending_control = int(control); response.length = int(length); return response; // the data follows
            case UVC_GET_CUR: c = control == UVC_VS_PROBE_CONTROL ? probe : commit; break;
            case UVC_GET_MIN: case UVC_GET_MAX: case UVC_GET_DEF: c = defaults(); break;
            case UVC_GET_RES: break;
            case UVC_GET_LEN: response.data[0] = sizeof c; response.data[1] = 0; response.length = 2; return response;
            case UVC_GET_INFO: response.data[0] = 3; response.length = 1; return response; // GET and SET
            default: return response;
            }
            std::memcpy(response.data, &c, sizeof c);
            response.length = int(std::min<size_t>(length, sizeof c));
        } else if (interface == 0 && entity == 0 && control == UVC_VC_REQUEST_ERROR_CODE_CONTROL) {
            if (request.bRequest == UVC_GET_CUR) { response.data[0] = 0x06; response.length = 1; } // "invalid control"
            else if (request.bRequest == UVC_GET_INFO) { response.data[0] = 1; response.length = 1; }
        }
        return response;
    }

    // One picture size and rate is offered: whatever the computer proposes, the answer is ours.
    void take_data(const uvc_request_data& data) {
        if (pending_control == UVC_VS_PROBE_CONTROL) probe = defaults();
        else if (pending_control == UVC_VS_COMMIT_CONTROL) commit = defaults();
        (void)data;
    }

    void events() {
        v4l2_event event {};
        while (fd >= 0 && xioctl(fd, VIDIOC_DQEVENT, &event) == 0) {
            auto* uvc = reinterpret_cast<uvc_event*>(&event.u.data);
            switch (event.type) {
            case UVC_EVENT_SETUP: {
                uvc_request_data response = answer(uvc->req);
                xioctl(fd, UVCIOC_SEND_RESPONSE, &response);
                break;
            }
            case UVC_EVENT_DATA: take_data(uvc->data); break;
            case UVC_EVENT_STREAMON: stream_on(); break;
            case UVC_EVENT_STREAMOFF: stream_off(); break;
            case UVC_EVENT_DISCONNECT:
                // The computer went (unplugged, or the gadget rebinding: the card lent, a rate, another size). No
                // STREAMOFF: with no request of the computer's waiting it only trips usb_composite_setup_continue's
                // warning. Closing frees the buffers; the device opens again within a second.
                close_device();
                mark_live(false);
                set_state(MirrorState::Waiting);
                return;
            default: break;
            }
        }
        if (fd >= 0 && errno == ENODEV) { close_device(); mark_live(false); set_state(MirrorState::Waiting); } // the gadget rebound
    }

    void stream_on() {
        if (format.mjpeg) {
            if (!encoder.ready()) { std::string error; start_encoder(error); } // a stream resumed after a restart
            start_maker();
            // Its first picture, so that the buffers queued below carry one (an encoder takes under 50 ms).
            for (int n = 0; n < 50; ++n) {
                { std::lock_guard<std::mutex> guard(jpeg_lock); if (!jpeg.empty()) break; }
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        }
        set_format();
        v4l2_requestbuffers request {};
        request.count = buffer_count; request.type = V4L2_BUF_TYPE_VIDEO_OUTPUT; request.memory = V4L2_MEMORY_MMAP;
        if (xioctl(fd, VIDIOC_REQBUFS, &request) < 0 || request.count == 0) { set_state(MirrorState::Failed, "webcam buffers"); return; }
        buffers = std::min(request.count, buffer_count);
        for (unsigned n = 0; n < buffers; ++n) {
            v4l2_buffer buffer {};
            buffer.type = V4L2_BUF_TYPE_VIDEO_OUTPUT; buffer.memory = V4L2_MEMORY_MMAP; buffer.index = n;
            if (xioctl(fd, VIDIOC_QUERYBUF, &buffer) < 0) { unmap(); set_state(MirrorState::Failed, "webcam buffer"); return; }
            lengths[n] = buffer.length;
            memory[n] = mmap(nullptr, buffer.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, buffer.m.offset);
            if (memory[n] == MAP_FAILED) { unmap(); set_state(MirrorState::Failed, "webcam buffer map"); return; }
            send(n);
        }
        int type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
        // STREAMON also finishes the computer's request to start (kernel/uvc held its status for this).
        if (xioctl(fd, VIDIOC_STREAMON, &type) < 0) { set_state(MirrorState::Failed, std::string("webcam start: ") + std::strerror(errno)); return; }
        streaming = true;
        window = Window {};
        mark_live(true);
        set_state(MirrorState::Streaming);
    }

    void stream_off() {
        report(true);
        stop_maker();
        int type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
        xioctl(fd, VIDIOC_STREAMOFF, &type); // also answers the computer's request to stop
        mark_live(false);
        unmap();
        v4l2_requestbuffers none {};
        none.count = 0; none.type = V4L2_BUF_TYPE_VIDEO_OUTPUT; none.memory = V4L2_MEMORY_MMAP;
        xioctl(fd, VIDIOC_REQBUFS, &none);
        streaming = false;
        set_state(MirrorState::Waiting);
    }

    // ---- Pictures ----

    // How the stream went over the last few seconds, in the service log while streaming: pictures sent and made
    // (the screen changed), what making one cost, timer ticks with no buffer back from the computer yet or missed
    // while a picture was being made, buffers the kernel gave back marked bad (dropped on the way), and the
    // longest wait between two buffers coming back.
    struct Window {
        Clock::time_point start = Clock::now(), last_back;
        unsigned sent = 0, made = 0, late = 0, missed = 0, bad = 0;
        double make_ms = 0, make_max_ms = 0, gap_max_ms = 0;
        size_t bytes = 0;
    } window;

    void report(bool force = false) {
        auto now = Clock::now();
        double seconds = std::chrono::duration<double>(now - window.start).count();
        if (!force && seconds < 5) return;
        if (format.mjpeg) { // the maker's pictures: made from the screen to the JPEG, the encoder's queue included
            window.made = made_count.exchange(0);
            window.make_ms = made_us.exchange(0) / 1000.0;
            window.make_max_ms = made_max_us.exchange(0) / 1000.0;
        }
        if (window.sent || window.late || window.missed) {
            char line[320];
            std::snprintf(line, sizeof line, "Screen mirror: %.1f s %ux%u: %u sent (%.1f fps), %u made (%.1f ms, max %.1f), "
                          "%u late, %u missed ticks, %u bad, longest gap %.0f ms, %zu kB a picture",
                          seconds, format.width, format.height, window.sent, window.sent / std::max(seconds, 0.001), window.made,
                          window.made ? window.make_ms / window.made : 0.0, window.make_max_ms, window.late, window.missed, window.bad,
                          window.gap_max_ms, window.sent ? window.bytes / window.sent / 1024 : 0);
            std::cout << line << std::endl;
        }
        window = Window {};
        window.last_back = now;
    }

    // YUYV: the picture for the screen offered last, made again (on this thread, under a millisecond) only when
    // it changed.
    void refresh() {
        {
            std::lock_guard<std::mutex> guard(lock);
            if (offered_serial == payload_serial && !payload.empty()) return;
            screen = offered;
            payload_serial = offered_serial;
        }
        auto started = Clock::now();
        payload.resize(frame_bytes(format));
        mirror_frame::to_yuyv(screen.data(), format.width, format.height, payload.data(), size_t(format.width) * 2);
        double ms = std::chrono::duration<double, std::milli>(Clock::now() - started).count();
        ++window.made; window.make_ms += ms; window.make_max_ms = std::max(window.make_max_ms, ms);
    }

    void send(unsigned n) {
        size_t size = 0;
        if (format.mjpeg) { // the newest JPEG the maker left
            std::lock_guard<std::mutex> guard(jpeg_lock);
            size = std::min(jpeg.size(), lengths[n]);
            if (size) std::memcpy(memory[n], jpeg.data(), size);
        } else {
            refresh();
            size = std::min(payload.size(), lengths[n]);
            if (size) std::memcpy(memory[n], payload.data(), size);
        }
        if (!size) { idle.push_back(n); return; } // nothing made yet
        v4l2_buffer buffer {};
        buffer.type = V4L2_BUF_TYPE_VIDEO_OUTPUT; buffer.memory = V4L2_MEMORY_MMAP; buffer.index = n;
        buffer.bytesused = unsigned(size);
        if (xioctl(fd, VIDIOC_QBUF, &buffer) == 0) { count_frame(); ++window.sent; window.bytes += size; }
    }

    // ---- The JPEG maker ----

    void start_maker() {
        if (making.load() || !encoder.ready()) return;
        { std::lock_guard<std::mutex> guard(jpeg_lock); jpeg.clear(); }
        making.store(true);
        maker = std::thread([this] { make_pictures(); });
    }

    void stop_maker() {
        if (!maker.joinable()) return;
        { std::lock_guard<std::mutex> guard(lock); making.store(false); }
        offered_changed.notify_all();
        maker.join();
    }

    // The maker's thread: whenever the screen changed and a slot is free, fill it and hand it to the encoder; take
    // each JPEG as it is done. With both slots busy it waits for the older one, so the CPU's part of the next
    // picture overlaps the encoder's part of this one.
    void make_pictures() {
        std::vector<uint32_t> picture(screen.size());
        uint64_t taken = ~0ull;                           // the screen's serial last handed to the encoder
        bool busy[JpegEncoder::slots] {};
        Clock::time_point started[JpegEncoder::slots] {};
        unsigned in_flight = 0;
        std::string error, said;
        auto complain = [&](const std::string& what) { if (what != said) std::cout << "Screen mirror: " << what << std::endl; said = what; };
        while (making.load()) {
            bool fresh = false;
            {
                std::unique_lock<std::mutex> guard(lock);
                if (!in_flight)
                    offered_changed.wait_for(guard, std::chrono::milliseconds(100), [&] { return !making.load() || offered_serial != taken; });
                if (in_flight < JpegEncoder::slots && offered_serial != taken) { picture = offered; taken = offered_serial; fresh = true; }
            }
            if (!making.load()) break;
            if (fresh) {
                unsigned slot = busy[0] ? 1 : 0;
                started[slot] = Clock::now();
                mirror_frame::to_yuyv(picture.data(), format.width, format.height, encoder.input(slot), encoder.stride());
                if (encoder.submit(slot, error)) { busy[slot] = true; ++in_flight; }
                else complain(error);
                if (in_flight < JpegEncoder::slots) continue; // the screen may have changed again: fill the other slot
            }
            if (!in_flight) continue;
            unsigned slot = 0;
            const uint8_t* data = nullptr;
            size_t size = 0;
            auto result = encoder.collect(slot, data, size, in_flight == JpegEncoder::slots ? 1000 : 5, error);
            if (result == JpegEncoder::Collect::Pending) continue;
            if (result == JpegEncoder::Collect::Done) {
                { std::lock_guard<std::mutex> guard(jpeg_lock); jpeg.assign(data, data + std::min<size_t>(size, frame_bytes(format))); }
                auto us = unsigned(std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - started[slot]).count());
                made_count.fetch_add(1); made_us.fetch_add(us);
                if (us > made_max_us.load()) made_max_us.store(us);
            } else complain(error);
            busy[slot] = false; --in_flight;
        }
    }

    void returned() {
        v4l2_buffer buffer {};
        buffer.type = V4L2_BUF_TYPE_VIDEO_OUTPUT; buffer.memory = V4L2_MEMORY_MMAP;
        while (xioctl(fd, VIDIOC_DQBUF, &buffer) == 0) {
            idle.push_back(buffer.index);
            auto now = Clock::now();
            if (buffer.flags & V4L2_BUF_FLAG_ERROR) ++window.bad; // a request missed its microframe: the computer drops it
            if (window.last_back != Clock::time_point {})
                window.gap_max_ms = std::max(window.gap_max_ms, std::chrono::duration<double, std::milli>(now - window.last_back).count());
            window.last_back = now;
        }
    }

    // ---- The thread ----

    void run() {
        timer = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
        itimerspec every {{0, long(interval_100ns) * 100}, {0, long(interval_100ns) * 100}};
        timerfd_settime(timer, 0, &every, nullptr);
        adopt();
        while (running.load()) {
            ScreenMirrorMode mode;
            { std::lock_guard<std::mutex> guard(lock); mode = wanted; }
            auto now = Clock::now();
            // Never unplug the camera under a computer showing it (its app may not survive that): turning it off or
            // another size waits until the computer stops the stream.
            if (mode != applied && now >= settle_until) {
                if (streaming) set_held(true);
                else { set_held(false); applied = mode; apply(mode); retry = Clock::now() + std::chrono::seconds(1); }
            } else if (mode == applied) set_held(false);
            now = Clock::now();
            // Not open: the webcam was just linked, or the gadget rebound (the card lent and back, a rate change).
            if (applied.on && fd < 0 && now >= retry && state() != MirrorState::Failed) {
                retry = now + std::chrono::seconds(1);
                if (gadget_mirror_linked()) open_device();
                else link();
            }
            pollfd waits[2] {{fd, short(POLLPRI | (streaming && idle.size() < buffers ? POLLOUT : 0)), 0}, {timer, POLLIN, 0}};
            if (poll(waits, 2, 200) < 0) continue;
            if (fd >= 0 && (waits[0].revents & POLLPRI)) events();
            if (fd >= 0 && (waits[0].revents & POLLHUP)) { close_device(); mark_live(false); set_state(MirrorState::Waiting); continue; }
            if (fd >= 0 && streaming && (waits[0].revents & POLLOUT)) returned();
            if (waits[1].revents & POLLIN) {
                uint64_t ticks = 0;
                if (read(timer, &ticks, sizeof ticks) < 0) ticks = 0;
                if (fd >= 0 && streaming) {
                    if (ticks > 1) window.missed += unsigned(ticks - 1); // this thread was busy past a tick
                    if (idle.empty()) ++window.late;                      // every buffer still with the computer
                    else { unsigned n = idle.back(); idle.pop_back(); send(n); }
                    report();
                }
            }
        }
        close_device();
        encoder.close();
        if (timer >= 0) close(timer);
    }
};

ScreenMirror::ScreenMirror(const ScreenMirrorMode& initial) : worker(std::make_unique<Worker>(initial)) {}
ScreenMirror::~ScreenMirror() = default;

void ScreenMirror::set_mode(const ScreenMirrorMode& mode) {
    std::lock_guard<std::mutex> guard(worker->lock);
    if (mode.on && (mode != worker->wanted || !worker->taking.load())) worker->need_frame.store(true);
    worker->wanted = mode;
    worker->taking.store(mode.on);
}

bool ScreenMirror::wants_frame() const { return worker->need_frame.load(); }

void ScreenMirror::offer(const juce::Image& image) {
    if (!worker->taking.load() || image.getWidth() != int(mirror_frame::screen_width) || image.getHeight() != int(mirror_frame::screen_height)) return;
    juce::Image::BitmapData bitmap(image, juce::Image::BitmapData::readOnly);
    if (bitmap.pixelStride != 4) return;
    std::lock_guard<std::mutex> guard(worker->lock);
    for (unsigned row = 0; row < mirror_frame::screen_height; ++row)
        std::memcpy(worker->offered.data() + size_t(row) * mirror_frame::screen_width, bitmap.getLinePointer(int(row)), mirror_frame::screen_width * 4);
    ++worker->offered_serial;
    worker->need_frame.store(false);
    worker->offered_changed.notify_one();
}

ScreenMirrorStatus ScreenMirror::status() const {
    std::lock_guard<std::mutex> guard(worker->status_lock);
    return worker->shown;
}
