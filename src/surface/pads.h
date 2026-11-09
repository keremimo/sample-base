// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <array>
#include <cstdint>

// Physical pads 1..16 in printed-number order, measured on AC50 1.3.0.12.
// Surface MIDI uses channel 10. The wire notes are neither sequential nor
// limited to 36..51: pad 4 sends 82, pad 14 sends 55 and pad 16 sends 53.
constexpr std::array<uint8_t, 16> surface_pad_notes {
    37, 36, 42, 82, 40, 38, 46, 44, 48, 47, 45, 43, 49, 55, 51, 53
};
constexpr std::array<uint8_t, 16> pad_scale_notes {
    60, 62, 64, 65, 67, 69, 71, 72, 74, 76, 77, 79, 81, 83, 84, 86
};
constexpr std::array<const char*, 16> pad_note_labels {
    "C4", "D4", "E4", "F4", "G4", "A4", "B4", "C5",
    "D5", "E5", "F5", "G5", "A5", "B5", "C6", "D6"
};
inline int surface_pad_index(uint8_t note) {
    for (unsigned pad = 0; pad < surface_pad_notes.size(); ++pad)
        if (surface_pad_notes[pad] == note) return int(pad);
    return -1;
}
