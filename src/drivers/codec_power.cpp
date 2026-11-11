// SPDX-License-Identifier: AGPL-3.0-only
#include "drivers/codec_power.h"
#include <fcntl.h>
#include <unistd.h>
#include <array>
#include <cmath>
#include <cstdlib>

namespace {
// A sysfs or debugfs file's text (one page at most) without its trailing newline; empty when unreadable.
std::string read_small(const std::string& path) {
    int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return {};
    char buffer[4096];
    ssize_t count = read(fd, buffer, sizeof buffer);
    close(fd);
    std::string text(buffer, count > 0 ? size_t(count) : 0);
    while (!text.empty() && (text.back() == '\n' || text.back() == ' ')) text.pop_back();
    return text;
}

// sysfs takes a value in one write() and reports a refusal from it (O_TRUNC as a shell's `>`).
bool write_small(const std::string& path, const std::string& text) {
    int fd = open(path.c_str(), O_WRONLY | O_TRUNC | O_CLOEXEC);
    if (fd < 0) return false;
    bool ok = write(fd, text.data(), text.size()) == ssize_t(text.size());
    return close(fd) == 0 && ok;
}

// The rates an ASoC codec is set up for; a fractional clock divider may land a few Hz off them.
constexpr std::array<unsigned, 14> standard_rates {{8000, 11025, 12000, 16000, 22050, 24000, 32000, 44100, 48000,
                                                    88200, 96000, 128000, 176400, 192000}};
} // namespace

CodecLink codec_link(int card, const std::string& pcm_id, const std::string& sys, const std::string& clock) {
    std::string link = pcm_id.substr(0, pcm_id.find(' '));
    if (card < 0 || link.empty() || link.find('/') != std::string::npos || link == "." || link == "..") return {};
    std::string dir = sys + "/class/sound/card" + std::to_string(card) + "/device/" + link;
    if (access((dir + "/pmdown_time").c_str(), W_OK) != 0) return {};
    return {dir, clock};
}

bool codec_powered(const CodecLink& link) {
    if (!link.found()) return false;
    // The codec's widgets, one "DACL: On" a line, then its bias: "PM State: On", "Prepare", "Standby" or "Off".
    std::string text = read_small(link.dir + "/dapm_widget");
    auto at = text.rfind("PM State: ");
    if (at == std::string::npos) return false;
    at += 10;
    std::string state = text.substr(at, text.find('\n', at) - at);
    return state == "On" || state == "Prepare" || state == "Standby";
}

unsigned codec_clock_rate(const CodecLink& link) {
    if (!link.found() || link.clock.empty()) return 0;
    std::string text = read_small(link.clock);
    char* end = nullptr;
    unsigned long long hz = std::strtoull(text.c_str(), &end, 10);
    if (text.empty() || *end || hz == 0) return 0;
    double fs = double(hz) / codec_mclk_fs;
    for (unsigned rate : standard_rates)
        if (std::fabs(fs - rate) <= rate * 0.0005) return rate;
    return 0;
}

ImmediatePowerDown::ImmediatePowerDown(const CodecLink& link) {
    if (!link.found()) return;
    std::string file = link.dir + "/pmdown_time", before = read_small(file);
    if (before.empty() || before == "0" || !write_small(file, "0")) return;
    path = file; saved = before;
}

ImmediatePowerDown::~ImmediatePowerDown() {
    if (!path.empty()) write_small(path, saved);
}
