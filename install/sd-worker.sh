#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-only
# Starts the installed app from the card, as device.py start does: the
# binary and recovery script are checked, copied into a new RAM directory
# and run by a session unit that stops the stock app and starts it again on
# any exit. Only /run changes; on any doubt the stock app keeps running.
set -eu
DIR=${1:?SD sample-base folder required}
EXPECTED=b6584cba76e416e432ad202509099d34373dc2c3d6a8fb625f5678bf0193b508
[ "$(sha256sum '/usr/bin/MPC Sample' | cut -d ' ' -f 1)" = "$EXPECTED" ] || { echo 'Unknown stock firmware; stock left running' >&2; exit 1; }
(cd "$DIR" && sha256sum -c SHA256SUMS) || { echo 'Card files changed; stock left running' >&2; exit 1; }
APP=$(cat "$DIR/app" 2>/dev/null || echo demos)
case "$APP" in ''|*[!A-Za-z0-9_-]*) echo 'Bad app name' >&2; exit 1 ;; esac
systemctl is-active --quiet sample-base.service && exit 0
ID=$(tr -d '-' < /proc/sys/kernel/random/uuid | cut -c 1-12)
case "$ID" in ''|*[!0-9a-f]*) echo 'No id for the RAM folder' >&2; exit 1 ;; esac
RELEASE=/run/sample-base-release-sd$ID
mkdir -m 700 "$RELEASE"
cp "$DIR/sample-base" "$DIR/restore-stock.sh" "$RELEASE/"
(cd "$DIR" && sha256sum sample-base restore-stock.sh) | (cd "$RELEASE" && sha256sum -c -)
chmod 755 "$RELEASE/sample-base"
"$RELEASE/sample-base" --self-test "$RELEASE/test.wav" || { echo 'Self-test failed; stock left running' >&2; exit 1; }
OPTION=""
[ "$APP" = demos ] || OPTION=" --app $APP"
cat > /run/systemd/system/sample-base.service <<UNIT
[Unit]
Description=sample-base standalone JUCE instrument (from the SD card)

[Service]
Type=exec
ExecStartPre=/bin/systemctl stop ac50.service
ExecStart=$RELEASE/sample-base$OPTION
ExecStopPost=/bin/sh $RELEASE/restore-stock.sh
Restart=no
TimeoutStartSec=30
TimeoutStopSec=10
RuntimeMaxSec=infinity
UMask=0077
UNIT
ln -s "$RELEASE" /run/sample-base.next && mv -Tf /run/sample-base.next /run/sample-base
systemctl daemon-reload
systemctl start --no-block sample-base.service
