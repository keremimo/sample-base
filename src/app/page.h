// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

// The app's screens. Sample opens the recorder and Pad FX the light show;
// `page_names` are the names the `page` control command accepts.
enum class Page : uint8_t { Synth, Recorder, Lights, Count };
constexpr std::array<const char*, size_t(Page::Count)> page_names {{"synth", "recorder", "lights"}};
