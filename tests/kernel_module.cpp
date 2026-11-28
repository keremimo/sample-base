// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#include "drivers/kernel_module.h"
#include <cassert>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using kernel_module::Image;

static Image image_of(const std::string& bytes) {
    auto begin = reinterpret_cast<const unsigned char*>(bytes.data());
    return {begin, begin + bytes.size()};
}

static std::string read(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    assert(file);
    return {std::istreambuf_iterator<char>(file), {}};
}

int main(int argc, char** argv) {
    assert(argc == 2);
    const std::string root = argv[1];
    const std::string v612 = "6.12.60-imb-2025-12-03-rt13", v618 = "6.18.59-imb-2026-10-26-rt9";
    const std::string old_module = std::string("\x7f" "ELF", 4) + std::string("license=GPL\0vermagic=", 21) + v612 +
                                   std::string(" SMP preempt mod_unload aarch64\0name=x\0", 39);
    const std::string new_module = std::string("license=GPL\0vermagic=", 21) + v618 + std::string(" SMP preempt_rt aarch64\0", 24);
    const std::string ends_at_word = "vermagic=" + v618;
    assert(kernel_module::built_for(image_of(old_module)) == v612);
    assert(kernel_module::built_for(image_of(new_module)) == v618);
    assert(kernel_module::built_for(image_of(ends_at_word)) == v618);
    assert(kernel_module::built_for(image_of("no magic here")).empty());
    assert(kernel_module::built_for({}).empty());

    // Each firmware's kernel gets its own build; a build missing one (weak symbols, null) is skipped.
    const Image a = image_of(old_module), b = image_of(new_module);
    assert(kernel_module::pick({a, b}, v612).begin == a.begin);
    assert(kernel_module::pick({a, b}, v618).begin == b.begin);
    assert(kernel_module::pick({{}, b}, v618).begin == b.begin);
    assert(!kernel_module::pick({a, {}}, v618).begin);
    assert(!kernel_module::pick({a, b}, "6.18.60-imb-2026-12-01-rt9").begin);
    assert(!kernel_module::pick({a, b}, "").begin);

    // The committed builds: each is for the kernel its name says.
    const std::vector<std::pair<std::string, std::string>> builds {
        {"kernel/panel/sample_base_panel.ko", v612}, {"kernel/panel/sample_base_panel-6.18.ko", v618},
        {"kernel/uvc/sample_base_uvc.ko", v612}, {"kernel/uvc-6.18/sample_base_uvc-6.18.ko", v618},
        {"kernel/vepu/sample_base_vepu.ko", v612}, {"kernel/vepu/sample_base_vepu-6.18.ko", v618},
    };
    for (const auto& [path, release] : builds) {
        const std::string bytes = read(root + "/" + path);
        assert(kernel_module::built_for(image_of(bytes)) == release);
    }
    return 0;
}
