// SPDX-License-Identifier: AGPL-3.0-only
#include "drivers/jpeg_encoder.h"
#include <fcntl.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>

namespace {
constexpr unsigned kinds[2] {V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE}; // picture in, JPEG out

int xioctl(int fd, unsigned long request, void* argument) {
    int result;
    do result = ioctl(fd, request, argument); while (result < 0 && errno == EINTR);
    return result;
}

// The hantro device that makes JPEG.
int find_encoder() {
    for (int n = 0; n < 32; ++n) {
        std::string path = "/dev/video" + std::to_string(n);
        int fd = ::open(path.c_str(), O_RDWR | O_CLOEXEC);
        if (fd < 0) continue;
        v4l2_capability capability {};
        if (xioctl(fd, VIDIOC_QUERYCAP, &capability) == 0 && std::strcmp(reinterpret_cast<const char*>(capability.driver), "hantro-vpu") == 0) {
            v4l2_fmtdesc format {};
            format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
            for (format.index = 0; xioctl(fd, VIDIOC_ENUM_FMT, &format) == 0; ++format.index)
                if (format.pixelformat == V4L2_PIX_FMT_JPEG) return fd;
        }
        ::close(fd);
    }
    return -1;
}
} // namespace

bool JpegEncoder::open(unsigned w, unsigned h, int quality, std::string& error) {
    close();
    auto fail = [&](const char* step) { error = std::string("JPEG encoder: ") + step + ": " + std::strerror(errno); close(); return false; };
    fd = find_encoder();
    if (fd < 0) { error = "JPEG encoder: none (hantro-vpu)"; return false; }
    width = w; height = h;
    // The coded format first: hantro sets the picture's format from it.
    v4l2_format coded {};
    coded.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    coded.fmt.pix_mp.width = w; coded.fmt.pix_mp.height = h;
    coded.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_JPEG; coded.fmt.pix_mp.num_planes = 1;
    if (xioctl(fd, VIDIOC_S_FMT, &coded) < 0) return fail("coded format");
    v4l2_format picture {};
    picture.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    picture.fmt.pix_mp.width = w; picture.fmt.pix_mp.height = h;
    picture.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_YUYV; picture.fmt.pix_mp.num_planes = 1;
    picture.fmt.pix_mp.plane_fmt[0].bytesperline = w * 2;
    if (xioctl(fd, VIDIOC_S_FMT, &picture) < 0) return fail("picture format");
    if (picture.fmt.pix_mp.width != w || picture.fmt.pix_mp.height != h || picture.fmt.pix_mp.pixelformat != V4L2_PIX_FMT_YUYV) {
        error = "JPEG encoder: refuses " + std::to_string(w) + " x " + std::to_string(h) + " YUYV"; close(); return false;
    }
    input_stride = picture.fmt.pix_mp.plane_fmt[0].bytesperline;
    v4l2_control control {V4L2_CID_JPEG_COMPRESSION_QUALITY, quality};
    xioctl(fd, VIDIOC_S_CTRL, &control); // its default when refused
    for (int k = 0; k < 2; ++k) {
        v4l2_requestbuffers request {};
        request.count = slots; request.type = kinds[k]; request.memory = V4L2_MEMORY_MMAP;
        if (xioctl(fd, VIDIOC_REQBUFS, &request) < 0 || request.count < slots) return fail("buffers");
        for (unsigned n = 0; n < slots; ++n) {
            v4l2_plane plane {};
            v4l2_buffer buffer {};
            buffer.type = kinds[k]; buffer.memory = V4L2_MEMORY_MMAP; buffer.index = n; buffer.m.planes = &plane; buffer.length = 1;
            if (xioctl(fd, VIDIOC_QUERYBUF, &buffer) < 0) return fail("buffer");
            void*& memory = k == 0 ? in[n] : out[n];
            (k == 0 ? in_length[n] : out_length[n]) = plane.length;
            memory = mmap(nullptr, plane.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, plane.m.mem_offset);
            if (memory == MAP_FAILED) { memory = nullptr; return fail("map"); }
        }
        unsigned kind = kinds[k];
        if (xioctl(fd, VIDIOC_STREAMON, &kind) < 0) return fail("start");
    }
    return true;
}

void JpegEncoder::close() {
    for (unsigned n = 0; n < slots; ++n) {
        if (in[n]) munmap(in[n], in_length[n]);
        if (out[n]) munmap(out[n], out_length[n]);
        in[n] = out[n] = nullptr; in_length[n] = out_length[n] = 0;
    }
    if (fd >= 0) ::close(fd); // closing stops both queues and frees their buffers
    fd = -1;
}

bool JpegEncoder::submit(unsigned slot, std::string& error) {
    if (fd < 0) { error = "JPEG encoder: not open"; return false; }
    slot %= slots;
    // The picture and the buffer its JPEG goes to, in the same slot: mem2mem pairs them in the order queued.
    for (int k = 0; k < 2; ++k) {
        v4l2_plane plane {};
        v4l2_buffer buffer {};
        buffer.type = kinds[k]; buffer.memory = V4L2_MEMORY_MMAP; buffer.index = slot; buffer.m.planes = &plane; buffer.length = 1;
        plane.bytesused = k == 0 ? unsigned(input_stride * height) : 0;
        plane.length = unsigned(k == 0 ? in_length[slot] : out_length[slot]);
        if (xioctl(fd, VIDIOC_QBUF, &buffer) < 0) { error = std::string("JPEG encoder: queue: ") + std::strerror(errno); return false; }
    }
    return true;
}

JpegEncoder::Collect JpegEncoder::collect(unsigned& slot, const uint8_t*& jpeg, size_t& size, int timeout_ms, std::string& error) {
    if (fd < 0) { error = "JPEG encoder: not open"; return Collect::Failed; }
    pollfd waiting {fd, POLLIN, 0};
    int ready;
    do ready = poll(&waiting, 1, timeout_ms); while (ready < 0 && errno == EINTR);
    if (ready == 0) return Collect::Pending;
    if (ready < 0) { error = std::string("JPEG encoder: wait: ") + std::strerror(errno); return Collect::Failed; }
    bool ok = true;
    for (int k = 1; k >= 0; --k) {
        v4l2_plane plane {};
        v4l2_buffer buffer {};
        buffer.type = kinds[k]; buffer.memory = V4L2_MEMORY_MMAP; buffer.m.planes = &plane; buffer.length = 1;
        if (xioctl(fd, VIDIOC_DQBUF, &buffer) < 0) { error = std::string("JPEG encoder: take: ") + std::strerror(errno); return Collect::Failed; }
        if (k == 1) {
            slot = buffer.index % slots;
            jpeg = static_cast<const uint8_t*>(out[slot]); size = plane.bytesused;
            ok = !(buffer.flags & V4L2_BUF_FLAG_ERROR) && size > 2 && jpeg[0] == 0xff && jpeg[1] == 0xd8;
        }
    }
    if (!ok) { error = "JPEG encoder: bad picture"; return Collect::Failed; }
    return Collect::Done;
}
