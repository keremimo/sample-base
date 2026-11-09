// SPDX-License-Identifier: AGPL-3.0-only
#include "mixer.h"
#include "platform.h"
#include <sstream>
#include <algorithm>
#include <cstring>

HardwareMixer::HardwareMixer(const std::string& card) {
    if (snd_ctl_open(&ctl, card.c_str(), SND_CTL_NONBLOCK) < 0) ctl = nullptr;
}
HardwareMixer::~HardwareMixer() {
    if (routes_changed) silence_inputs();
    for (auto& entry : original) {
        if (ctl) snd_ctl_elem_write(ctl, entry.second);
        snd_ctl_elem_value_free(entry.second);
    }
    if (ctl) snd_ctl_close(ctl);
}
void HardwareMixer::silence_inputs() {
    for (const char* name : {"Builtin Microphone Switch", "TRS Switch"}) {
        snd_ctl_elem_info_t* info; snd_ctl_elem_info_alloca(&info);
        snd_ctl_elem_info_set_interface(info, SND_CTL_ELEM_IFACE_MIXER);
        snd_ctl_elem_info_set_name(info, name);
        if (!ctl || snd_ctl_elem_info(ctl, info) < 0 || snd_ctl_elem_info_get_type(info) != SND_CTL_ELEM_TYPE_BOOLEAN) continue;
        snd_ctl_elem_value_t* value; snd_ctl_elem_value_alloca(&value);
        snd_ctl_elem_value_set_numid(value, snd_ctl_elem_info_get_numid(info));
        if (snd_ctl_elem_read(ctl, value) < 0) continue;
        for (unsigned i = 0; i < snd_ctl_elem_info_get_count(info) && i < 128; ++i) snd_ctl_elem_value_set_boolean(value, i, 0);
        snd_ctl_elem_write(ctl, value);
    }
}
bool HardwareMixer::set(unsigned id, const std::vector<long>& values) {
    if (!ctl || values.empty()) return false;
    snd_ctl_elem_info_t* info; snd_ctl_elem_info_alloca(&info);
    snd_ctl_elem_info_set_numid(info, id);
    if (snd_ctl_elem_info(ctl, info) < 0 || !snd_ctl_elem_info_is_readable(info) || !snd_ctl_elem_info_is_writable(info)) return false;
    auto type = snd_ctl_elem_info_get_type(info);
    unsigned count = snd_ctl_elem_info_get_count(info);
    if (count > 128 || values.size() != count || (type != SND_CTL_ELEM_TYPE_BOOLEAN && type != SND_CTL_ELEM_TYPE_INTEGER && type != SND_CTL_ELEM_TYPE_ENUMERATED)) return false;
    long min = type == SND_CTL_ELEM_TYPE_INTEGER ? snd_ctl_elem_info_get_min(info) : 0;
    long max = type == SND_CTL_ELEM_TYPE_INTEGER ? snd_ctl_elem_info_get_max(info) :
               type == SND_CTL_ELEM_TYPE_ENUMERATED ? long(snd_ctl_elem_info_get_items(info)) - 1 : 1;
    for (long value : values) if (value < min || value > max) return false;
    const char* name = snd_ctl_elem_info_get_name(info);
    bool mic = !strcmp(name, "Builtin Microphone Switch"), line = !strcmp(name, "TRS Switch");
    if (mic || line) {
        snd_ctl_elem_info_t* other; snd_ctl_elem_info_alloca(&other);
        snd_ctl_elem_info_set_interface(other, SND_CTL_ELEM_IFACE_MIXER);
        snd_ctl_elem_info_set_name(other, mic ? "TRS Switch" : "Builtin Microphone Switch");
        if (std::any_of(values.begin(), values.end(), [](long value) { return value != 0; })) {
            if (snd_ctl_elem_info(ctl, other) < 0) return false;
            snd_ctl_elem_value_t* other_value; snd_ctl_elem_value_alloca(&other_value);
            snd_ctl_elem_value_set_numid(other_value, snd_ctl_elem_info_get_numid(other));
            if (snd_ctl_elem_read(ctl, other_value) < 0) return false;
            for (unsigned i = 0; i < snd_ctl_elem_info_get_count(other) && i < 128; ++i)
                if (snd_ctl_elem_value_get_boolean(other_value, i)) return false;
        }
        routes_changed = true;
    }
    snd_ctl_elem_value_t* current; snd_ctl_elem_value_alloca(&current);
    snd_ctl_elem_value_set_numid(current, id);
    if (snd_ctl_elem_read(ctl, current) < 0) return false;
    if (!original.count(id)) {
        snd_ctl_elem_value_t* saved = nullptr;
        if (snd_ctl_elem_value_malloc(&saved) < 0) return false;
        snd_ctl_elem_value_copy(saved, current); original[id] = saved;
    }
    for (unsigned i = 0; i < count; ++i) {
        if (type == SND_CTL_ELEM_TYPE_BOOLEAN) snd_ctl_elem_value_set_boolean(current, i, values[i]);
        else if (type == SND_CTL_ELEM_TYPE_INTEGER) snd_ctl_elem_value_set_integer(current, i, values[i]);
        else snd_ctl_elem_value_set_enumerated(current, i, unsigned(values[i]));
    }
    return snd_ctl_elem_write(ctl, current) >= 0;
}
bool HardwareMixer::named_switch(const char* name, bool enabled) {
    snd_ctl_elem_info_t* info; snd_ctl_elem_info_alloca(&info);
    snd_ctl_elem_info_set_interface(info, SND_CTL_ELEM_IFACE_MIXER);
    snd_ctl_elem_info_set_name(info, name);
    if (!ctl || snd_ctl_elem_info(ctl, info) < 0 || snd_ctl_elem_info_get_type(info) != SND_CTL_ELEM_TYPE_BOOLEAN) return false;
    return set(snd_ctl_elem_info_get_numid(info), std::vector<long>(snd_ctl_elem_info_get_count(info), enabled ? 1 : 0));
}
bool HardwareMixer::input(const std::string& source) {
    if (!ctl || (source != "off" && source != "mic" && source != "line")) return false;
    // Never enable both physical sources. Disable both before selecting one;
    // a failure leaves inputs off instead of silently connecting both.
    if (!named_switch("Builtin Microphone Switch", false) || !named_switch("TRS Switch", false)) return false;
    return source == "off" || named_switch(source == "mic" ? "Builtin Microphone Switch" : "TRS Switch", true);
}
std::string HardwareMixer::json() const {
    if (!ctl) return "{\"available\":false,\"controls\":[]}";
    snd_ctl_elem_list_t* list; snd_ctl_elem_list_alloca(&list);
    if (snd_ctl_elem_list(ctl, list) < 0) return "{\"available\":false,\"controls\":[]}";
    unsigned count = snd_ctl_elem_list_get_count(list);
    if (count > 512 || snd_ctl_elem_list_alloc_space(list, count) < 0) return "{\"available\":false,\"controls\":[]}";
    std::ostringstream out; out << "{\"available\":true,\"controls\":["; bool first = true;
    if (snd_ctl_elem_list(ctl, list) >= 0) for (unsigned n = 0; n < snd_ctl_elem_list_get_used(list); ++n) {
        unsigned id = snd_ctl_elem_list_get_numid(list, n);
        snd_ctl_elem_info_t* info; snd_ctl_elem_info_alloca(&info);
        snd_ctl_elem_info_set_numid(info, id);
        if (snd_ctl_elem_info(ctl, info) < 0) continue;
        auto type = snd_ctl_elem_info_get_type(info);
        if (!first) out << ',';
        first = false;
        out << "{\"id\":" << id << ",\"name\":" << json_string(snd_ctl_elem_info_get_name(info))
            << ",\"type\":" << json_string(snd_ctl_elem_type_name(type)) << ",\"writable\":" << (snd_ctl_elem_info_is_writable(info) ? "true" : "false");
        if (type == SND_CTL_ELEM_TYPE_INTEGER) out << ",\"minimum\":" << snd_ctl_elem_info_get_min(info) << ",\"maximum\":" << snd_ctl_elem_info_get_max(info);
        if (type == SND_CTL_ELEM_TYPE_ENUMERATED) {
            unsigned items = snd_ctl_elem_info_get_items(info);
            out << ",\"choices\":[";
            for (unsigned item = 0; item < items && item < 256; ++item) {
                snd_ctl_elem_info_set_item(info, item);
                if (item) out << ',';
                out << (snd_ctl_elem_info(ctl, info) < 0 ? "null" : json_string(snd_ctl_elem_info_get_item_name(info)));
            }
            out << ']';
        }
        snd_ctl_elem_value_t* value; snd_ctl_elem_value_alloca(&value);
        snd_ctl_elem_value_set_numid(value, id);
        out << ",\"values\":[";
        if (snd_ctl_elem_info_is_readable(info) && snd_ctl_elem_read(ctl, value) >= 0) for (unsigned i = 0; i < snd_ctl_elem_info_get_count(info) && i < 128; ++i) {
            if (i) out << ',';
            if (type == SND_CTL_ELEM_TYPE_BOOLEAN) out << snd_ctl_elem_value_get_boolean(value, i);
            else if (type == SND_CTL_ELEM_TYPE_INTEGER) out << snd_ctl_elem_value_get_integer(value, i);
            else if (type == SND_CTL_ELEM_TYPE_ENUMERATED) out << snd_ctl_elem_value_get_enumerated(value, i);
            else if (type == SND_CTL_ELEM_TYPE_INTEGER64) out << snd_ctl_elem_value_get_integer64(value, i);
            else out << "null";
        }
        out << "]}";
    }
    snd_ctl_elem_list_free_space(list);
    return out.str() + "]}";
}
