#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Kerem Kilic (Ellic Studio)
# SAMPLE_BASE_TOP_AUTORUN
# The card's autorun on MPC Sample firmware 1.4, which looks for autorun.sh
# at the top of a card. With Developer Mode on, the stock app asks whether to
# run it each time it starts with the card in, then runs it with /bin/sh.
# It runs the card's autorun/autorun.sh, the one firmware 1.3.0.12 runs by
# itself; SAMPLE_BASE_ASKED tells sd-boot.sh that the start was asked for.
SAMPLE_BASE_ASKED=1 exec /bin/sh "$(dirname -- "$0")/autorun/autorun.sh"
