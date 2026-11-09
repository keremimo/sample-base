// SPDX-License-Identifier: AGPL-3.0-only
#include "demos/screen_feed.h"
#include "surface/controls.h"

DemoView ScreenFeed::frame(DemoState& state, const Session& session) {
    DemoView view;
    view.page = state.page.load();
    view.held = session.held.load(); view.last_pad = session.last_pad.load(); view.velocity = session.velocity.load();
    view.peak = session.audio.peak.load(); view.xruns = session.audio.xruns.load();
    view.last_input = describe_last_input(session.last_input.load());
    view.light_position = state.light_position.load();
    view.recorder = recorder.frame(state, session.audio.capture_active.load(), view.page == Page::Recorder);
    pad_trace.advance(state.pads, view.velocity);
    if (view.page == Page::Pads) view.pads = pad_trace.view(state.pads);
    view.seq = seq.frame(state, view.page == Page::Seq);
    view.system = system_view(state, session, view.page == Page::System);
    return view;
}
