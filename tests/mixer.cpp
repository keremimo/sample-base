// SPDX-License-Identifier: AGPL-3.0-only
#include "drivers/mixer.h"
#include <cassert>
#include <cstring>

static long values[5][2] = {{0,0}, {80,81}, {1,0}, {0,0}, {1,0}};
static const char* names[] = {"", "Playback Volume", "Builtin Microphone Switch", "TRS Switch", "Headphone Jack"};
extern "C" int __wrap_snd_ctl_open(snd_ctl_t** ctl, const char* name, int) {
    if (strcmp(name, "hw:I2S")) return -1;
    *ctl = reinterpret_cast<snd_ctl_t*>(1); return 0;
}
extern "C" int __wrap_snd_ctl_close(snd_ctl_t*) { return 0; }
extern "C" int __wrap_snd_ctl_elem_info(snd_ctl_t*, snd_ctl_elem_info_t* info) {
    unsigned id = snd_ctl_elem_info_get_numid(info);
    if (!id) for (unsigned n = 1; n < 5; ++n) if (!strcmp(snd_ctl_elem_info_get_name(info), names[n])) id = n;
    if (id == 0 || id >= 5) return -1;
    snd_ctl_elem_info_set_numid(info, id); snd_ctl_elem_info_set_name(info, names[id]); return 0;
}
extern "C" snd_ctl_elem_type_t __wrap_snd_ctl_elem_info_get_type(const snd_ctl_elem_info_t* info) {
    return snd_ctl_elem_info_get_numid(info) == 1 ? SND_CTL_ELEM_TYPE_INTEGER : SND_CTL_ELEM_TYPE_BOOLEAN;
}
extern "C" unsigned __wrap_snd_ctl_elem_info_get_count(const snd_ctl_elem_info_t* info) { return snd_ctl_elem_info_get_numid(info) == 1 ? 2 : 1; }
extern "C" int __wrap_snd_ctl_elem_info_is_readable(const snd_ctl_elem_info_t*) { return 1; }
extern "C" int __wrap_snd_ctl_elem_info_is_writable(const snd_ctl_elem_info_t* info) { return snd_ctl_elem_info_get_numid(info) != 4; }
extern "C" long __wrap_snd_ctl_elem_info_get_min(const snd_ctl_elem_info_t*) { return 0; }
extern "C" long __wrap_snd_ctl_elem_info_get_max(const snd_ctl_elem_info_t*) { return 255; }
extern "C" int __wrap_snd_ctl_elem_read(snd_ctl_t*, snd_ctl_elem_value_t* value) {
    unsigned id = snd_ctl_elem_value_get_numid(value); assert(id > 0 && id < 5);
    if (id == 1) for (unsigned n = 0; n < 2; ++n) snd_ctl_elem_value_set_integer(value, n, values[id][n]);
    else snd_ctl_elem_value_set_boolean(value, 0, values[id][0]);
    return 0;
}
extern "C" int __wrap_snd_ctl_elem_write(snd_ctl_t*, snd_ctl_elem_value_t* value) {
    unsigned id = snd_ctl_elem_value_get_numid(value); assert(id > 0 && id < 4);
    if (id == 1) for (unsigned n = 0; n < 2; ++n) values[id][n] = snd_ctl_elem_value_get_integer(value, n);
    else values[id][0] = snd_ctl_elem_value_get_boolean(value, 0);
    assert(!(values[2][0] && values[3][0])); // Including every restore write.
    return 0;
}
int main() {
    {
        HardwareMixer mixer; assert(mixer.available());
        assert(mixer.set(1, {100,101}));
        assert(!mixer.set(1, {256,0}) && !mixer.set(1, {1}) && !mixer.set(1, {}));
        assert(!mixer.set(4, {0}) && !mixer.set(3, {1}));
        assert(mixer.input("line") && values[2][0] == 0 && values[3][0] == 1);
        assert(!mixer.input("invalid"));
        assert(values[1][0] == 100 && values[1][1] == 101);
    }
    assert(values[1][0] == 80 && values[1][1] == 81 && values[2][0] == 1 && values[3][0] == 0);
    HardwareMixer absent("missing"); assert(!absent.available() && !absent.input("off"));
}
