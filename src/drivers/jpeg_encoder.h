// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

// The RK3566's hardware JPEG encoder (the RK3568 VEPU, through hantro-vpu's
// V4L2 mem2mem device "rockchip,rk3568-vepu-enc"; kernel/vepu switches it on).
// A YUYV picture in, a whole JPEG file out: a 1280 x 960 picture takes about
// 11 ms of the encoder and 0.15 ms of the CPU (2026-11-14, docs/HARDWARE.md).
class JpegEncoder {
public:
    JpegEncoder() = default;
    ~JpegEncoder() { close(); }
    JpegEncoder(const JpegEncoder&) = delete;
    JpegEncoder& operator=(const JpegEncoder&) = delete;

    // Finds the encoder and sets it up for width x height pictures (multiples
    // of 16) at `quality` (1-100).
    bool open(unsigned width, unsigned height, int quality, std::string& error);
    void close();
    bool ready() const { return fd >= 0; }
    // The picture to encode next: height rows of stride() bytes.
    uint8_t* input() { return static_cast<uint8_t*>(memory[0]); }
    size_t stride() const { return input_stride; }
    // Encodes input(); `jpeg` and `size` stay valid until the next call.
    bool encode(const uint8_t*& jpeg, size_t& size, std::string& error);

private:
    int fd = -1;
    unsigned width = 0, height = 0;
    size_t input_stride = 0;
    void* memory[2] {nullptr, nullptr};
    size_t lengths[2] {0, 0};
};
