// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include "app/app_state.h"
#include "app/screen.h"
#include <chrono>

// Screen thread. Copies what the screen shows out of AppState once per frame,
// so Screen::draw() never reads shared state while it draws. A refused
// recording shows a hint for a few frames, which is why this keeps state.
class ScreenFeed {
public:
    ScreenState frame(AppState& state);
private:
    unsigned refusals_seen = 0;
    std::chrono::steady_clock::time_point hint_until = std::chrono::steady_clock::time_point::min();
};

// The recorder page's view of AppState.
RecorderView recorder_view(const AppState& state);
