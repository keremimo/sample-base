// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include "app/app_state.h"
#include "app/screen.h"
#include <array>
#include <chrono>

// Screen thread. Copies what the screen shows out of AppState once per frame,
// so Screen::draw() never reads shared state while it draws. A refused
// recording shows a hint for a few frames and the pad page draws a pressure
// history, which is why this keeps state.
class ScreenFeed {
public:
    ScreenState frame(AppState& state);
private:
    unsigned refusals_seen = 0;
    std::chrono::steady_clock::time_point hint_until = std::chrono::steady_clock::time_point::min();
    // Pressure history, sampled every frame whichever page is open.
    std::array<uint8_t, PadsView::trace_length> trace {}, strike_marks {};
    unsigned strikes_seen = 0;
};

// The recorder page's view of AppState.
RecorderView recorder_view(const AppState& state);
