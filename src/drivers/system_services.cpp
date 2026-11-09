// SPDX-License-Identifier: AGPL-3.0-only
#include "drivers/system_services.h"
#include "drivers/platform.h"
#include <map>
#include <sstream>

namespace {
constexpr const char* service = "io.github.inmusicdev.edisksd";
constexpr const char* root = "/io/github/inmusicdev/edisksd";
constexpr const char* filesystem_interface = "io.github.inmusicdev.edisksd.Filesystem";
std::string bytes_string(const void* bytes, size_t size) {
    auto* text = static_cast<const char*>(bytes);
    if (!size) return {};
    if (text[size - 1] == 0) --size;
    return std::string(text, size);
}
std::string property_json(sd_bus_message* message, const std::string& type) {
    if (type == "s" || type == "o") {
        const char* value = nullptr;
        if (sd_bus_message_read_basic(message, type[0], &value) < 0) return "null";
        return json_string(value ? value : "");
    }
    if (type == "b") {
        int value = 0; if (sd_bus_message_read_basic(message, 'b', &value) < 0) return "null";
        return value ? "true" : "false";
    }
    if (type == "ay") {
        const void* bytes = nullptr; size_t size = 0;
        if (sd_bus_message_read_array(message, 'y', &bytes, &size) < 0) return "null";
        return json_string(bytes_string(bytes, size));
    }
    if (type == "aay") {
        if (sd_bus_message_enter_container(message, 'a', "ay") < 0) return "null";
        std::string value = "["; bool first = true;
        const void* bytes = nullptr; size_t size = 0;
        while (sd_bus_message_read_array(message, 'y', &bytes, &size) > 0) {
            if (!first) value += ',';
            first = false; value += json_string(bytes_string(bytes, size));
        }
        sd_bus_message_exit_container(message);
        return value + ']';
    }
    sd_bus_message_skip(message, type.c_str()); return "null";
}
}
SystemServices::SystemServices() {
    if (sd_bus_open_system(&bus) < 0) { bus = nullptr; return; }
    sd_bus_set_method_call_timeout(bus, 500000);
    sd_bus_match_signal(bus, &sleep_slot, "org.freedesktop.login1", "/org/freedesktop/login1",
                        "org.freedesktop.login1.Manager", "PrepareForSleep", lifecycle, this);
    sd_bus_match_signal(bus, &shutdown_slot, "org.freedesktop.login1", "/org/freedesktop/login1",
                        "org.freedesktop.login1.Manager", "PrepareForShutdown", lifecycle, this);
    sd_bus_match_signal(bus, &added_slot, service, nullptr, "org.freedesktop.DBus.ObjectManager", "InterfacesAdded", storage_event, this);
    sd_bus_match_signal(bus, &removed_slot, service, nullptr, "org.freedesktop.DBus.ObjectManager", "InterfacesRemoved", storage_event, this);
    sd_bus_match_signal(bus, &property_slot, service, nullptr, "org.freedesktop.DBus.Properties", "PropertiesChanged", storage_event, this);
}
SystemServices::~SystemServices() {
    sd_bus_slot_unref(sleep_slot); sd_bus_slot_unref(shutdown_slot);
    sd_bus_slot_unref(added_slot); sd_bus_slot_unref(removed_slot); sd_bus_slot_unref(property_slot);
    sd_bus_unref(bus);
}
int SystemServices::storage_event(sd_bus_message*, void* userdata, sd_bus_error*) {
    ++static_cast<SystemServices*>(userdata)->generation; return 0;
}
int SystemServices::lifecycle(sd_bus_message* message, void* userdata, sd_bus_error*) {
    int active = 0;
    if (sd_bus_message_read(message, "b", &active) >= 0 && active) static_cast<SystemServices*>(userdata)->stop_requested = true;
    return 0;
}
void SystemServices::poll() {
    if (!bus) return;
    for (unsigned n = 0; n < 32; ++n) if (sd_bus_process(bus, nullptr) <= 0) break;
}
bool SystemServices::mount(const std::string& object, bool mounted) {
    // Accept only the service object namespace, never a shell command or
    // filesystem path. The daemon validates the object and enforces policy.
    if (!bus || object.rfind(std::string(root) + '/', 0) != 0 || object.find_first_not_of("/ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_") != std::string::npos) return false;
    sd_bus_error error = SD_BUS_ERROR_NULL;
    sd_bus_message* reply = nullptr;
    sd_bus_set_method_call_timeout(bus, 2000000);
    int result = sd_bus_call_method(bus, service, object.c_str(), filesystem_interface,
                                    mounted ? "Mount" : "Unmount", &error, &reply, "a{sv}", 0);
    sd_bus_set_method_call_timeout(bus, 500000);
    sd_bus_error_free(&error); sd_bus_message_unref(reply);
    return result >= 0;
}
std::string SystemServices::storage_json() {
    if (!bus) return "{\"available\":false,\"volumes\":[]}";
    sd_bus_error error = SD_BUS_ERROR_NULL;
    sd_bus_message* reply = nullptr;
    int result = sd_bus_call_method(bus, service, "/", "org.freedesktop.DBus.ObjectManager",
                                    "GetManagedObjects", &error, &reply, "");
    sd_bus_error_free(&error);
    if (result < 0) { sd_bus_message_unref(reply); return "{\"available\":false,\"volumes\":[]}"; }
    std::ostringstream out; out << "{\"available\":true,\"volumes\":[";
    bool first = true;
    bool valid = sd_bus_message_enter_container(reply, 'a', "{oa{sa{sv}}}") > 0;
    unsigned count = 0;
    while (valid && count++ < 128 && sd_bus_message_enter_container(reply, 'e', "oa{sa{sv}}") > 0) {
        const char* object = nullptr;
        valid = sd_bus_message_read_basic(reply, 'o', &object) >= 0 && sd_bus_message_enter_container(reply, 'a', "{sa{sv}}") > 0;
        std::map<std::string, std::string> properties;
        bool volume = false;
        while (valid && sd_bus_message_enter_container(reply, 'e', "sa{sv}") > 0) {
            const char* interface = nullptr;
            valid = sd_bus_message_read_basic(reply, 's', &interface) >= 0;
            if (valid && std::string(interface) == filesystem_interface) volume = true;
            valid = valid && sd_bus_message_enter_container(reply, 'a', "{sv}") > 0;
            while (valid && sd_bus_message_enter_container(reply, 'e', "sv") > 0) {
                const char *key = nullptr, *type = nullptr; char container = 0;
                valid = sd_bus_message_read_basic(reply, 's', &key) >= 0 && sd_bus_message_peek_type(reply, &container, &type) > 0 && container == 'v';
                std::string signature = type ? type : "";
                valid = valid && sd_bus_message_enter_container(reply, 'v', signature.c_str()) > 0;
                if (!valid) break;
                auto value = property_json(reply, signature);
                if (value != "null") properties[key] = value;
                valid = sd_bus_message_exit_container(reply) >= 0 && sd_bus_message_exit_container(reply) >= 0;
            }
            valid = valid && sd_bus_message_exit_container(reply) >= 0 && sd_bus_message_exit_container(reply) >= 0;
        }
        valid = valid && sd_bus_message_exit_container(reply) >= 0 && sd_bus_message_exit_container(reply) >= 0;
        if (valid && volume) {
            if (!first) out << ',';
            first = false;
            out << "{\"object\":" << json_string(object);
            for (const auto& property : properties) out << ',' << json_string(property.first) << ':' << property.second;
            out << '}';
        }
    }
    sd_bus_message_unref(reply);
    if (!valid) return "{\"available\":false,\"volumes\":[],\"error\":\"invalid service reply\"}";
    return out.str() + "]}";
}
