// SPDX-License-Identifier: AGPL-3.0-only
#include "drivers/clock_io.h"
#include <linux/gpio.h>
#include <fcntl.h>
#include <cassert>
#include <cstdarg>
#include <cerrno>
#include <cstring>

static bool high = false, busy = false;
static unsigned reads = 0, requests = 0;
extern "C" int __wrap_open(const char* path, int, ...) {
    if (!strcmp(path, "/dev/gpiochip0")) return 100;
    errno = ENOENT; return -1;
}
extern "C" int __wrap_close(int fd) { assert(fd >= 100 && fd <= 102); return 0; }
extern "C" int __wrap_fcntl(int fd, int command, ...) { assert(fd == 101 || fd == 102); assert(command == F_SETFL); return 0; }
extern "C" int __wrap_ioctl(int fd, unsigned long command, ...) {
    va_list args; va_start(args, command); void* data = va_arg(args, void*); va_end(args);
    if (command == GPIO_GET_CHIPINFO_IOCTL) { assert(fd == 100); static_cast<gpiochip_info*>(data)->lines = 3; return 0; }
    if (command == GPIO_V2_GET_LINEINFO_IOCTL) {
        auto* line = static_cast<gpio_v2_line_info*>(data);
        strcpy(line->name, line->offset == 0 ? "DO_NOT_TOUCH_RESET" : line->offset == 1 ? "CV_CLK_IN" : "CV_CLK_OUT"); return 0;
    }
    if (command == GPIO_V2_GET_LINE_IOCTL) {
        auto* line = static_cast<gpio_v2_line_request*>(data);
        assert(line->num_lines == 1 && (line->offsets[0] == 1 || line->offsets[0] == 2));
        ++requests;
        if (busy) { errno = EBUSY; return -1; }
        if (line->offsets[0] == 1) {
            assert(line->config.flags & GPIO_V2_LINE_FLAG_INPUT);
            assert(line->config.flags & GPIO_V2_LINE_FLAG_EDGE_RISING);
            assert(line->config.flags & GPIO_V2_LINE_FLAG_EDGE_FALLING);
            line->fd = 101;
        } else {
            assert(line->config.flags == GPIO_V2_LINE_FLAG_OUTPUT && line->config.num_attrs == 1);
            assert(line->config.attrs[0].attr.id == GPIO_V2_LINE_ATTR_ID_OUTPUT_VALUES);
            high = line->config.attrs[0].attr.values != 0; line->fd = 102;
        }
        return 0;
    }
    assert(command == GPIO_V2_LINE_SET_VALUES_IOCTL && fd == 102);
    auto* value = static_cast<gpio_v2_line_values*>(data); assert(value->mask == 1);
    high = value->bits != 0; return 0;
}
extern "C" ssize_t __wrap_read(int fd, void* buffer, size_t size) {
    assert(fd == 101 && size >= 2 * sizeof(gpio_v2_line_event));
    if (reads++) { errno = EAGAIN; return -1; }
    auto* events = static_cast<gpio_v2_line_event*>(buffer);
    events[0] = {}; events[0].id = GPIO_V2_LINE_EVENT_RISING_EDGE; events[0].seqno = 1; events[0].timestamp_ns = 100;
    events[1] = {}; events[1].id = GPIO_V2_LINE_EVENT_FALLING_EDGE; events[1].seqno = 3; events[1].timestamp_ns = 300;
    return 2 * sizeof(gpio_v2_line_event);
}
int main() {
    {
        ClockIo clock;
        assert(requests == 0); // Construction never acquires hardware.
        assert(clock.input(true)); clock.poll();
        assert(clock.json().find("\"lost_edges\":1") != std::string::npos);
        assert(clock.json().find("\"timestamp_ns\":300") != std::string::npos);
        assert(clock.output(true) && high);
        assert(clock.output(false) && !high);
        assert(clock.output(true) && high);
        assert(clock.input(false));
    }
    assert(!high);
    busy = true;
    ClockIo owned; assert(!owned.input(true) && !owned.output(true));
}
