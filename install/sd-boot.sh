#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-only
# SD autorun entry for an installed app: hand off at once so the stock
# app's autorun runner never waits. $1 is the card's sample-base folder.
set -eu
DIR=${1:?SD sample-base folder required}
[ -f "$DIR/sd-worker.sh" ] || exit 0
# The stock app runs the card's autorun again each time it starts; after
# leaving the app (or a failed start) it stays stock until the next boot.
[ ! -e /run/sample-base-sd-started ] || exit 0
: > /run/sample-base-sd-started
exec systemd-run --unit=sample-base-sd-boot --collect --no-block \
    --property=Type=oneshot --property=After=ac50.service --property=TimeoutStartSec=90 \
    /bin/sh "$DIR/sd-worker.sh" "$DIR"
