// SPDX-License-Identifier: AGPL-3.0-only
#include "drivers/clock_io.h"
#include <linux/gpio.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <cstring>
#include <cerrno>
#include <sstream>

int ClockIo::request(const char* name, bool output, bool high) {
    for (unsigned chip = 0; chip < 64; ++chip) {
        std::string path = "/dev/gpiochip" + std::to_string(chip);
        int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0) continue;
        gpiochip_info info {};
        if (ioctl(fd, GPIO_GET_CHIPINFO_IOCTL, &info) == 0) for (unsigned offset = 0; offset < info.lines && offset < 1024; ++offset) {
            gpio_v2_line_info line {}; line.offset = offset;
            if (ioctl(fd, GPIO_V2_GET_LINEINFO_IOCTL, &line) < 0 || strcmp(line.name, name)) continue;
            gpio_v2_line_request request {};
            request.offsets[0] = offset; request.num_lines = 1;
            strcpy(request.consumer, "sample-base-sync");
            request.config.flags = output ? GPIO_V2_LINE_FLAG_OUTPUT :
                GPIO_V2_LINE_FLAG_INPUT | GPIO_V2_LINE_FLAG_EDGE_RISING | GPIO_V2_LINE_FLAG_EDGE_FALLING;
            if (output) {
                request.config.num_attrs = 1;
                request.config.attrs[0].mask = 1;
                request.config.attrs[0].attr.id = GPIO_V2_LINE_ATTR_ID_OUTPUT_VALUES;
                request.config.attrs[0].attr.values = high ? 1 : 0;
            }
            int result = ioctl(fd, GPIO_V2_GET_LINE_IOCTL, &request);
            close(fd);
            if (result < 0) return -1; // Never steal a line owned by a driver.
            if (fcntl(request.fd, F_SETFL, O_NONBLOCK) < 0) { close(request.fd); return -1; }
            return request.fd;
        }
        close(fd);
    }
    return -1;
}
ClockIo::~ClockIo() {
    std::lock_guard<std::mutex> guard(lock);
    if (input_fd >= 0) close(input_fd);
    if (output_fd >= 0) {
        gpio_v2_line_values value {}; value.mask = 1;
        ioctl(output_fd, GPIO_V2_LINE_SET_VALUES_IOCTL, &value);
        close(output_fd);
    }
}
bool ClockIo::input(bool enabled) {
    std::lock_guard<std::mutex> guard(lock);
    if (!enabled) { if (input_fd >= 0) close(input_fd); input_fd = -1; sequence = 0; return true; }
    if (input_fd < 0) input_fd = request("CV_CLK_IN", false, false);
    return input_fd >= 0;
}
bool ClockIo::output(bool high) {
    std::lock_guard<std::mutex> guard(lock);
    if (output_fd < 0) output_fd = request("CV_CLK_OUT", true, high);
    if (output_fd < 0) return false;
    gpio_v2_line_values value {}; value.mask = 1; value.bits = high ? 1 : 0;
    if (ioctl(output_fd, GPIO_V2_LINE_SET_VALUES_IOCTL, &value) < 0) return false;
    output_high = high; return true;
}
void ClockIo::poll() {
    std::lock_guard<std::mutex> guard(lock);
    if (input_fd < 0) return;
    gpio_v2_line_event events[16];
    for (unsigned batch = 0; batch < 4; ++batch) {
        ssize_t size = read(input_fd, events, sizeof(events));
        if (size < 0 && errno != EAGAIN && errno != EINTR) { close(input_fd); input_fd = -1; break; }
        if (size <= 0) break;
        for (unsigned i = 0; i < unsigned(size) / sizeof(events[0]); ++i) {
            const auto& event = events[i];
            if (sequence && uint32_t(event.seqno - sequence) > 1) lost_edges += uint32_t(event.seqno - sequence) - 1;
            sequence = event.seqno; timestamp = event.timestamp_ns;
            input_high = event.id == GPIO_V2_LINE_EVENT_RISING_EDGE; ++edges;
        }
    }
}
std::string ClockIo::json() const {
    std::lock_guard<std::mutex> guard(lock);
    std::ostringstream out;
    out << "{\"input_enabled\":" << (input_fd >= 0 ? "true" : "false") << ",\"input_high\":" << (input_high ? "true" : "false")
        << ",\"output_enabled\":" << (output_fd >= 0 ? "true" : "false") << ",\"output_high\":" << (output_high ? "true" : "false")
        << ",\"edges\":" << edges << ",\"lost_edges\":" << lost_edges << ",\"timestamp_ns\":" << timestamp << '}';
    return out.str();
}
