// SPDX-License-Identifier: AGPL-3.0-only
#include "drivers/system_services.h"
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstring>
#include <thread>

struct Fixture { std::atomic<bool> mounted {false}, stop {false}, emit {false}; };
static int mount_call(sd_bus_message* message, void* userdata, sd_bus_error*) {
    auto& fixture = *static_cast<Fixture*>(userdata);
    assert(sd_bus_message_skip(message, "a{sv}") >= 0);
    bool mount = !strcmp(sd_bus_message_get_member(message), "Mount");
    fixture.mounted.store(mount);
    return mount ? sd_bus_reply_method_return(message, "s", "/media/fixture") : sd_bus_reply_method_return(message, "");
}
static int property(sd_bus*, const char*, const char*, const char* name, sd_bus_message* reply, void*, sd_bus_error*) {
    if (!strcmp(name, "Label")) return sd_bus_message_append(reply, "s", "Fixture \"SD\"");
    if (!strcmp(name, "Type")) return sd_bus_message_append(reply, "s", "exfat");
    if (!strcmp(name, "Mountpoints")) {
        int result = sd_bus_message_open_container(reply, 'a', "ay");
        if (result < 0) return result;
        result = sd_bus_message_append_array(reply, 'y', "/media/fixture", sizeof("/media/fixture"));
        if (result < 0) return result;
        return sd_bus_message_close_container(reply);
    }
    return -1;
}
static const sd_bus_vtable vtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_METHOD("Mount", "a{sv}", "s", mount_call, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("Unmount", "a{sv}", "", mount_call, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_PROPERTY("Label", "s", property, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("Type", "s", property, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("Mountpoints", "aay", property, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
    SD_BUS_VTABLE_END
};
int main() {
    sd_bus* server = nullptr; sd_bus_slot *manager = nullptr, *volume = nullptr;
    assert(sd_bus_open_system(&server) >= 0);
    assert(sd_bus_request_name(server, "io.github.inmusicdev.edisksd", 0) >= 0);
    assert(sd_bus_request_name(server, "org.freedesktop.login1", 0) >= 0);
    assert(sd_bus_add_object_manager(server, &manager, "/") >= 0);
    Fixture fixture;
    const char* object = "/io/github/inmusicdev/edisksd/filesystems/fixture";
    assert(sd_bus_add_object_vtable(server, &volume, object, "io.github.inmusicdev.edisksd.Filesystem", vtable, &fixture) >= 0);
    std::thread thread([&] {
        while (!fixture.stop.load()) {
            if (fixture.emit.exchange(false)) {
                assert(sd_bus_emit_signal(server, "/org/freedesktop/login1", "org.freedesktop.login1.Manager", "PrepareForSleep", "b", 1) >= 0);
                assert(sd_bus_emit_properties_changed(server, object, "io.github.inmusicdev.edisksd.Filesystem", "Mountpoints", nullptr) >= 0);
                sd_bus_flush(server);
            }
            while (sd_bus_process(server, nullptr) > 0) {}
            sd_bus_wait(server, 1000);
        }
    });
    {
        SystemServices services;
        assert(services.available());
        auto json = services.storage_json();
        assert(json.find("\"available\":true") != std::string::npos);
        assert(json.find("Fixture \\\"SD\\\"") != std::string::npos);
        assert(json.find("/media/fixture") != std::string::npos);
        assert(!services.mount("/tmp/invalid", true));
        assert(services.mount(object, true) && fixture.mounted.load());
        assert(services.mount(object, false) && !fixture.mounted.load());
        assert(!services.mount("/io/github/inmusicdev/edisksd/absent", true));
        assert(!services.stopping()); fixture.emit.store(true);
        for (unsigned n = 0; n < 1000 && !services.stopping(); ++n) {
            services.poll(); std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        assert(services.stopping());
        for (unsigned n = 0; n < 1000 && services.storage_generation() == 0; ++n) {
            services.poll(); std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        assert(services.storage_generation() > 0);
    }
    fixture.stop.store(true); thread.join();
    sd_bus_slot_unref(volume); sd_bus_slot_unref(manager); sd_bus_unref(server);
}
