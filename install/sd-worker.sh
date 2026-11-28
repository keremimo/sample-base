#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Kerem Kilic (Ellic Studio)
# Starts the installed app from the card, as device.py start does: the
# binary and recovery script are checked, copied into a new RAM directory
# and run by a session unit that stops the stock app and starts it again on
# any exit. Only /run changes; on any doubt the stock app keeps running.
set -eu
DIR=${1:?SD sample-base folder required}
# The stock firmwares known to work (device.py STOCK_FIRMWARES): 1.3.0.12, 1.4.0.7, 1.4.0.8.
case "$(sha256sum '/usr/bin/MPC Sample' | cut -d ' ' -f 1)" in
    b6584cba76e416e432ad202509099d34373dc2c3d6a8fb625f5678bf0193b508) ;;
    2271252ffded26830546d4ee1852e39a2c1f4c574a40c47fba10b5a397727ec2) ;;
    473fb7260e597e61cc55d9d30595eb9d9aa1304ff1b88960e633ad19457dc91a) ;;
    *) echo 'Unknown stock firmware; stock left running' >&2; exit 1 ;;
esac
(cd "$DIR" && sha256sum -c SHA256SUMS) || { echo 'Card files changed; stock left running' >&2; exit 1; }
APP=$(cat "$DIR/app" 2>/dev/null || echo demos)
case "$APP" in ''|*[!A-Za-z0-9_-]*) echo 'Bad app name' >&2; exit 1 ;; esac
systemctl is-active --quiet sample-base.service && exit 0
# Each start copies the app into RAM. On firmware 1.4 every yes is a start, so
# the copies earlier starts left go first, once no app runs from them.
case "$(systemctl is-active sample-base.service 2>/dev/null || true)" in
    inactive|failed|unknown)
        for OLD in /run/sample-base-release-*; do
            [ ! -d "$OLD" ] || rm -rf "$OLD"
        done ;;
esac
ID=$(tr -d '-' < /proc/sys/kernel/random/uuid | cut -c 1-12)
case "$ID" in ''|*[!0-9a-f]*) echo 'No id for the RAM folder' >&2; exit 1 ;; esac
RELEASE=/run/sample-base-release-sd$ID
mkdir -m 700 "$RELEASE"
# Copy the binary, the recovery script and, if the release has it, sd-update.sh.
FILES="sample-base restore-stock.sh"
[ ! -f "$DIR/sd-update.sh" ] || FILES="$FILES sd-update.sh"
(cd "$DIR" && cp $FILES "$RELEASE/")
(cd "$DIR" && sha256sum $FILES) | (cd "$RELEASE" && sha256sum -c -)
# The card folder they came from, for the app's files left there: a boot menu
# (SampleBoot's autorun/mods/<name>/) may keep it anywhere on the card.
(cd "$DIR" && pwd) > "$RELEASE/sd-folder"
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
