// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <string>
#include <vector>

// Kernel modules for drivers the MPC's kernel lacks or leaves off. sample-base
// builds its own under kernel/<name>/ (tools/build_kernel_modules.sh) and
// keeps them inside the binary (src/drivers/panel_module.S,
// src/drivers/kernel_modules.S); load() puts one into the kernel from memory,
// with no file on any disk. probe() loads modules the MPC ships in
// /lib/modules (with their dependencies) by modprobe.
namespace kernel_module {
// The module is in the kernel now (/sys/module/<name>, underscores).
bool loaded(const std::string& name);
// Loads the module held in [begin, end) unless `name` is loaded already. A
// weak symbol missing from the build (begin or end null) is a failure.
bool load(const std::string& name, const unsigned char* begin, const unsigned char* end, std::string* error = nullptr);
// Removes the module; true once it is gone.
bool unload(const std::string& name);
// modprobe -a for in-tree modules; true when modprobe exits 0.
bool probe(const std::vector<std::string>& names, std::string* error = nullptr);
}
