#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-only
set -eu
# Shutdown already owns service teardown; do not restart an application then.
state=$(/bin/systemctl is-system-running 2>/dev/null || true)
case "$state" in
    stopping|offline) exit 0 ;;
esac
exec /bin/systemctl start ac50.service
