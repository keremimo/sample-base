// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include "runtime/app.h"
#include "runtime/options.h"

// Runs `app` on the MPC until wheel Push, the `stock` command, SIGINT or
// SIGTERM, the --seconds deadline or a failure. Opens the audio device,
// display, MIDI ports and the control socket, starts the audio, MIDI and
// service threads and draws the screen on the calling thread. Prints
// "READY ..." once every thread runs and the final status line on the way
// out. Returns the exit code.
int run_app(App& app, const Options& options);
