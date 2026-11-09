// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <cstdint>
#include <string>

// Opt-in access to the two sync lines named in the AC50 device tree. No pin
// numbers, mux, reset, codec or power GPIOs are changed. Output is a level API,
// not a sample-accurate clock generator; edge times come from the kernel.
class ClockIo {
public:
    ~ClockIo();
    bool input(bool enabled);
    bool output(bool high);
    void poll();
    std::string json() const;
private:
    static int request(const char* name, bool output, bool high);
    int input_fd = -1, output_fd = -1;
    bool output_high = false, input_high = false;
    uint64_t timestamp = 0, edges = 0, lost_edges = 0;
    uint32_t sequence = 0;
};
