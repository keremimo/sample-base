// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

// The RK3566's hardware JPEG encoder (the RK3568 VEPU, through hantro-vpu's
// V4L2 mem2mem device "rockchip,rk3568-vepu-enc"; kernel/vepu switches it on).
// A YUYV picture in, a whole JPEG file out: the encoder takes about 9.3 ns a
// pixel (11 ms at 1280 x 960, 26 ms at 1920 x 1440) and 0.15 ms of the CPU
// (2026-11-14, docs/HARDWARE.md). Two slots: the CPU fills one picture while
// the encoder makes the other, so a picture takes the longer of the two, not
// both.
class JpegEncoder {
public:
    static constexpr unsigned slots = 2;

    JpegEncoder() = default;
    ~JpegEncoder() { close(); }
    JpegEncoder(const JpegEncoder&) = delete;
    JpegEncoder& operator=(const JpegEncoder&) = delete;

    // Finds the encoder and sets it up for width x height pictures (multiples
    // of 16) at `quality` (1-100).
    bool open(unsigned width, unsigned height, int quality, std::string& error);
    void close();
    bool ready() const { return fd >= 0; }
    // A slot's picture: height rows of stride() bytes, filled before submit().
    uint8_t* input(unsigned slot) { return static_cast<uint8_t*>(in[slot % slots]); }
    size_t stride() const { return input_stride; }
    // Hands the slot's picture to the encoder, which makes them in turn.
    bool submit(unsigned slot, std::string& error);
    // The oldest submitted picture's JPEG, waited for up to `timeout_ms`:
    // `slot` says whose; `jpeg` and `size` stay valid until that slot is
    // submitted again. Pending: not done within the time.
    enum class Collect { Done, Pending, Failed };
    Collect collect(unsigned& slot, const uint8_t*& jpeg, size_t& size, int timeout_ms, std::string& error);

private:
    int fd = -1;
    unsigned width = 0, height = 0;
    size_t input_stride = 0;
    void* in[slots] {};
    void* out[slots] {};
    size_t in_length[slots] {}, out_length[slots] {};
};
