// SPDX-License-Identifier: AGPL-3.0-only
#include "demos/light_show/light_show_demo.h"
#include "runtime/util.h"

// A held pad glows white and fades back over 400 ms after release. A manual
// `led` command lasts until the next frame.
void LightShowLights::update(SurfaceLights& lights, DemoState& state, uint32_t held, Clock::time_point now) {
    using namespace std::chrono;
    if (!showing) { showing = true; show_start = next_frame = now; }
    int scene = state.scene_request.exchange(-1);
    if (scene >= 0) { show_start = now - milliseconds(light_scene_start(unsigned(scene))); next_frame = now; }
    if (now < next_frame) return;
    next_frame = now + milliseconds(25);
    auto position = unsigned(duration_cast<milliseconds>(now - show_start).count() % light_show_length());
    state.light_position.store(position);
    auto frame = light_frame(position);
    for (unsigned pad = 0; pad < surface_pad_lights; ++pad) {
        if (held & (1u << pad)) touched[pad] = now;
        float glow = 1 - duration<float, std::milli>(now - touched[pad]).count() / 400.0f;
        if (glow > 0) frame.pads[pad] = mix_light(frame.pads[pad], {127, 127, 127}, glow);
    }
    show_light_frame(lights, frame);
}

bool LightShowLights::close(SurfaceLights& lights, DemoState& state) {
    if (!showing) return false;
    showing = false; lights.all_off();
    state.scene_request.store(-1); state.light_position.store(0);
    return true;
}

bool light_show_button(DemoState& state, SurfaceButton button) {
    if (button == SurfaceButton::TopLeft) { state.scene_request.store(next_light_scene(state, -1)); return true; }
    if (button == SurfaceButton::TopMiddle) { state.scene_request.store(next_light_scene(state, 1)); return true; }
    return false;
}

int next_light_scene(const DemoState& state, int step) {
    if (state.page.load() != Page::Lights) return 0;
    int count = int(light_scenes.size()), current = int(light_scene_at(state.light_position.load()));
    return ((current + step) % count + count) % count;
}

std::string light_show_command(DemoState& state, const std::string& argument) {
    int count = int(light_scenes.size()), scene = -1;
    unsigned number = 0;
    if (argument == "next" || argument == "previous") scene = next_light_scene(state, argument == "next" ? 1 : -1);
    else if (parse_unsigned(argument, number) && number >= 1 && number <= light_scenes.size()) scene = int(number) - 1;
    else return "ERR use lights 1-" + std::to_string(count) + ", next or previous";
    state.page.store(Page::Lights); state.scene_request.store(scene);
    return "OK lights scene " + std::to_string(scene + 1) + ' ' + light_scenes[size_t(scene)].name;
}
