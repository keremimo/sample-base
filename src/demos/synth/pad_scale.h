// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <array>
#include <cstdint>

// The synth demo plays the pads as C major: pad 1 (bottom left) is C4 and
// pad 16 (top right) is D6.
constexpr std::array<uint8_t, 16> pad_scale_notes {
    60, 62, 64, 65, 67, 69, 71, 72, 74, 76, 77, 79, 81, 83, 84, 86
};
constexpr std::array<const char*, 16> pad_note_labels {
    "C4", "D4", "E4", "F4", "G4", "A4", "B4", "C5",
    "D5", "E5", "F5", "G5", "A5", "B5", "C6", "D6"
};
