// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

// The app's screens. Sample opens the recorder, Pad FX the light show,
// 16 Levels the pad touch page, SEQ the step sequencer and Knob FX the
// system dashboard; `page_names` are the names the `page` control command
// accepts.
enum class Page : uint8_t { Synth, Recorder, Lights, Pads, Seq, System, Count };
constexpr std::array<const char*, size_t(Page::Count)> page_names {{"synth", "recorder", "lights", "pads", "seq", "system"}};
