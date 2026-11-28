// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include <initializer_list>
#include <string>
#include <vector>

// Kernel modules for drivers the MPC's kernel lacks or leaves off. sample-base
// builds its own under kernel/<name>/ (tools/build_kernel_modules.sh) and
// keeps them inside the binary (src/drivers/panel_module.S,
// src/drivers/kernel_modules.S); load() puts one into the kernel from memory,
// with no file on any disk. probe() loads modules the MPC ships in
// /lib/modules (with their dependencies) by modprobe. Each module is built for
// each firmware's kernel (1.3.0.12: 6.12.60, 1.4: 6.18.59); load() with
// several images picks the one built for the running kernel.
namespace kernel_module {
// A module held in the binary, [begin, end); both null when the build has none
// (weak symbols).
struct Image {
    const unsigned char* begin = nullptr;
    const unsigned char* end = nullptr;
};
// The kernel release the module was built for (its vermagic's first word), or
// "" when there is none.
std::string built_for(const Image& image);
// Of `images`, the one built for `release`; an empty Image when none is.
Image pick(std::initializer_list<Image> images, const std::string& release);
// The running kernel's release (uname -r).
std::string running_release();
// The module is in the kernel now (/sys/module/<name>, underscores).
bool loaded(const std::string& name);
// Loads the module held in [begin, end) unless `name` is loaded already. A
// weak symbol missing from the build (begin or end null) is a failure.
bool load(const std::string& name, const unsigned char* begin, const unsigned char* end, std::string* error = nullptr);
// Loads whichever of `images` was built for the running kernel, unless `name`
// is loaded already; none built for it is a failure that names both releases.
bool load(const std::string& name, std::initializer_list<Image> images, std::string* error = nullptr);
// Removes the module; true once it is gone.
bool unload(const std::string& name);
// modprobe -a for in-tree modules; true when modprobe exits 0.
bool probe(const std::vector<std::string>& names, std::string* error = nullptr);
}
