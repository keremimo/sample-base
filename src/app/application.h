// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include "app/options.h"

// Runs the beep/boop app on the MPC until wheel Push, the `stock` command,
// SIGINT/SIGTERM, the --seconds deadline or a failure. Prints "READY ..."
// once every thread runs and the final status line on the way out. Returns
// the exit code.
int run_application(const Options& options);
