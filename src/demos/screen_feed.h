// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include "runtime/session.h"
#include "demos/demo_screen.h"
#include "demos/demo_state.h"
#include "demos/pad_touch/pad_touch_demo.h"
#include "demos/recorder/recorder_demo.h"
#include "demos/sequencer/sequencer_demo.h"
#include "demos/system/system_demo.h"

// Screen thread. Copies what the screen shows out of the shared state once
// per frame (see DemoView). The recorder's refusal hint, the pad page's
// pressure history and the drum hits the SEQ page has shown keep state
// between frames.
class ScreenFeed {
public:
    DemoView frame(DemoState& state, const Session& session);
private:
    RecorderFeed recorder;
    PadTrace pad_trace;
    SeqFeed seq;
};
