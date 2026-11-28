// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once

// --hardware: prints platform, audio, mixer, storage and MIDI ports as JSON
// without opening the MPC's audio output, display or control surface.
// Returns the exit code.
int print_hardware();
