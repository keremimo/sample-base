// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <optional>

// The demos' one-shot tools for build checks. None opens the MPC's audio
// output, display or control surface.
//   --self-test FILE.wav             renders a synth note and its release and
//                                    checks that it sounds, then falls silent
//   --preview FILE.png [PROMPT]      the synth page, or a remote prompt
//   --preview-recorder FILE.png      the recorder page
//   --preview-lights FILE.png [MS]   the light show page at MS into the loop
//   --preview-pads FILE.png          the pad touch page
//   --preview-seq FILE.png           the step sequencer page
// Returns the exit code, or nothing when argv names none of them.
std::optional<int> run_demo_tool(int argc, char** argv);
