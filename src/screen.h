// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <juce_graphics/juce_graphics.h>

#include <array>
#include <string>

// Snapshot of the input recording showcase for one frame.
struct RecorderView {
    static constexpr unsigned columns = 296; // Waveform width in pixels.
    uint8_t source = 0, channels = 0, state = 0; // InputSource, InputChannels, Recorder::State
    unsigned frames = 0, position = 0, capacity = 0, rate = 44100;
    unsigned scale = 0; // Frames across the waveform; at least `frames`.
    float left = 0, right = 0; // Input peaks before channel selection.
    std::string detail; // Source status, or a hint after a refused action.
    bool hint = false;
    std::array<uint8_t, columns> waveform {};
};

enum class Page : uint8_t { Synth, Recorder, Lights };

struct ScreenState {
    uint32_t held = 0;
    int last_pad = -1, velocity = 0;
    float peak = 0;
    unsigned xruns = 0;
    // Remote instruction, `|`-separated: header, title, detail, status, footer.
    // Non-empty text replaces the instrument view, e.g. during calibration.
    std::string prompt;
    std::string last_input; // Name of the most recent non-pad control.
    Page page = Page::Synth;
    RecorderView recorder;
    unsigned light_position = 0; // Milliseconds into the light show loop.
};
class Screen {
public:
    explicit Screen(const juce::String& font_path);
    juce::Image draw(const ScreenState& state);
private:
    void draw_recorder(juce::Graphics& g, const ScreenState& state);
    void draw_lights(juce::Graphics& g, const ScreenState& state);
    juce::Typeface::Ptr typeface;
    juce::Image image {juce::Image::ARGB, 320, 240, true};
};
