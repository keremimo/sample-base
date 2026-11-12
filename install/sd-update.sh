#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-only
# Installs an app update from a release zip on the SD card (docs/APPS.md,
# "Updates from the card").
#
# The zip contains the app's autorun folder. autorun/<package>/RELEASE.sha256
# lists the SHA-256 of every other file in the zip, and RELEASE.sha256.sig is
# an Ed25519 signature of that list. The signature is verified with KEY
# before any other file is read. Only files named in the list are extracted
# (with unzip -p, so unzip never chooses an output path), and each one is
# checked against its SHA-256.
#
#   sd-update.sh check ZIP PACKAGE KEY
#       Prints the version from the zip's VERSION file if the zip is a signed
#       release of PACKAGE. Exits 1 otherwise. Changes nothing.
#   sd-update.sh install ZIP CARD PACKAGE APP KEY
#       1. Runs the new binary's self-test in RAM.
#       2. Copies the new package to CARD/autorun/PACKAGE.next and checks it,
#          renames PACKAGE to PACKAGE.prev and PACKAGE.next to PACKAGE, and
#          replaces the autorun files.
#       3. Stops the running app without starting the stock app, then starts
#          the new app with the package's sd-worker.sh.
#       4. If the new app does not answer --ctl status within READY_SECONDS,
#          restores the card and restarts the previous app.
#       Writes progress to $STATUS (state=, version=, error=). Deletes the
#       zip after a successful install.
#
# Run it with systemd-run, because it stops the app that started it.
set -eu
RUN=/run
SYSTEMCTL=/bin/systemctl
OPENSSL=openssl
READY_SECONDS=30
STATUS=$RUN/sample-base-update.status
ACTIVE=$RUN/sample-base
UNIT=$RUN/systemd/system/sample-base.service
DROPIN=$UNIT.d/swap.conf

name_ok() { case "$1" in ''|*[!A-Za-z0-9_-]*) return 1 ;; esac; }

# Accepts a path from the list: it must start with autorun/, use only
# letters, digits and . _ + - /, and have no empty, "." or ".." component.
path_ok() {
    case "$1" in autorun/?*) ;; *) return 1 ;; esac
    case "$1" in *[!A-Za-z0-9._+/-]*|*//*|*/|*/./*|*/../*|*/.|*/..) return 1 ;; esac
}

# ZIP PACKAGE KEY WORK: extracts the list to WORK/list and verifies its
# signature. Fails unless every line is "<sha256>  <path>" with a valid path.
signed() {
    unzip -p "$1" "autorun/$2/RELEASE.sha256" > "$4/list" 2>/dev/null || return 1
    unzip -p "$1" "autorun/$2/RELEASE.sha256.sig" > "$4/list.sig" 2>/dev/null || return 1
    [ -s "$4/list" ] && [ -s "$4/list.sig" ] || return 1
    "$OPENSSL" pkeyutl -verify -pubin -inkey "$3" -rawin -in "$4/list" -sigfile "$4/list.sig" >/dev/null 2>&1 || return 1
    while IFS= read -r line; do
        sum=${line%%  *} path=${line#*  }
        [ "${#sum}" -eq 64 ] || return 1
        case "$sum" in *[!0-9a-f]*) return 1 ;; esac
        path_ok "$path" || return 1
    done < "$4/list"
}

# ZIP PATH WORK: extracts one file to WORK/zip/PATH.
take() {
    mkdir -p "$3/zip/${2%/*}" && unzip -p "$1" "$2" > "$3/zip/$2"
}

# WORK PATH: checks an extracted file against its line in the list.
listed() {
    line=$(grep -x "[0-9a-f]\{64\}  $2" "$1/list") || return 1
    (cd "$1/zip" && echo "$line" | sha256sum -c - >/dev/null 2>&1)
}

# FILE: prints the first word of a VERSION file. Fails on unexpected characters.
version_of() {
    v=
    read -r v _ < "$1" || true
    case "$v" in ''|*[!A-Za-z0-9._+-]*) return 1 ;; esac
    echo "$v"
}

check() {
    [ $# -eq 3 ] || { echo "usage: sd-update.sh check ZIP PACKAGE KEY" >&2; exit 2; }
    name_ok "$2" || exit 2
    work=$(mktemp -d "$RUN/sample-base-check.XXXXXX")
    trap 'rm -rf "$work"' EXIT
    signed "$1" "$2" "$3" "$work" || exit 1
    take "$1" "autorun/$2/VERSION" "$work" || exit 1
    listed "$work" "autorun/$2/VERSION" || exit 1
    version_of "$work/zip/autorun/$2/VERSION"
}

# ---- install ----------------------------------------------------------------

status() { # STATE [ERROR]
    printf 'state=%s\nversion=%s\nerror=%s\n' "$1" "$VERSION" "${2:-}" > "$STATUS.next"
    mv -f "$STATUS.next" "$STATUS"
}

# Stops the running app without starting the stock app: a runtime drop-in
# clears ExecStopPost (restore-stock.sh) for this stop, as device.py start
# does.
stop_quietly() {
    mkdir -p "${DROPIN%/*}"
    printf '[Service]\nExecStopPost=\n' > "$DROPIN"
    "$SYSTEMCTL" daemon-reload
    "$SYSTEMCTL" stop sample-base.service || true
    rm -f "$DROPIN"
    "$SYSTEMCTL" daemon-reload
}

# DIR: starts a release that is already in RAM, with the same unit sd-worker.sh writes.
start_release() {
    option=
    [ "$APP" = demos ] || option=" --app $APP"
    cat > "$UNIT" <<UNIT
[Unit]
Description=sample-base standalone JUCE instrument (from the SD card)

[Service]
Type=exec
ExecStartPre=/bin/systemctl stop ac50.service
ExecStart=$1/sample-base$option
ExecStopPost=/bin/sh $1/restore-stock.sh
Restart=no
TimeoutStartSec=30
TimeoutStopSec=10
RuntimeMaxSec=infinity
UMask=0077
UNIT
    ln -s "$1" "$ACTIVE.next" && mv -Tf "$ACTIVE.next" "$ACTIVE"
    "$SYSTEMCTL" daemon-reload
    "$SYSTEMCTL" start --no-block sample-base.service
}

# Waits up to READY_SECONDS for the started app to answer on its control socket.
ready() {
    n=0
    while [ "$n" -lt "$READY_SECONDS" ]; do
        sleep 1
        n=$((n + 1))
        "$ACTIVE/sample-base" --ctl status >/dev/null 2>&1 && return 0
        case "$("$SYSTEMCTL" is-active sample-base.service 2>/dev/null || true)" in
            failed) return 1 ;;
            inactive) [ "$n" -lt 3 ] || return 1 ;;
        esac
    done
    return 1
}

# Removes every release in /run except the running one: the previous app's
# and any left by earlier starts or SD boots (device.py start keeps SD boot
# releases).
drop_stale() {
    keep=$(readlink "$ACTIVE" 2>/dev/null || true)
    for release in "$RUN"/sample-base-release-*; do
        if [ -d "$release" ] && [ "$release" != "$keep" ]; then rm -rf "$release"; fi
    done
}

# Restores the card's package folder and autorun files from before the install.
card_back() {
    [ -n "$CARD_CHANGED" ] || return 0
    rm -rf "$TARGET"
    [ ! -d "$TARGET.prev" ] || mv "$TARGET.prev" "$TARGET"
    for name in autorun.json autorun.sh autorun.check; do
        if [ -f "$WORK/old-$name" ]; then
            cp "$WORK/old-$name" "$AUTORUN/$name.next" && mv -f "$AUTORUN/$name.next" "$AUTORUN/$name"
        else
            rm -f "$AUTORUN/$name"
        fi
    done
    sync
    CARD_CHANGED=
}

# ERROR: restores the card, restarts the previous app if it was stopped,
# writes the error to $STATUS and exits.
fail() {
    trap - EXIT
    rm -rf "$TARGET.next" 2>/dev/null || true
    card_back || true
    if [ -n "$STOPPED" ]; then
        stop_quietly || true
        if [ -n "$OLD" ] && [ -x "$OLD/sample-base" ]; then
            start_release "$OLD" || "$SYSTEMCTL" start ac50.service || true
            drop_stale || true # including the new app's RAM copy
        else
            "$SYSTEMCTL" start ac50.service || true # no previous app: start the stock app
        fi
    fi
    status failed "$1"
    rm -rf "$WORK"
    exit 1
}

install() {
    [ $# -eq 5 ] || { echo "usage: sd-update.sh install ZIP CARD PACKAGE APP KEY" >&2; exit 2; }
    ZIP=$1 CARD=$2 PACKAGE=$3 APP=$4 KEY=$5
    VERSION= CARD_CHANGED= STOPPED= OLD=
    name_ok "$PACKAGE" && name_ok "$APP" || exit 2
    AUTORUN=$CARD/autorun
    TARGET=$AUTORUN/$PACKAGE
    WORK=$(mktemp -d "$RUN/sample-base-update.XXXXXX")
    trap 'fail error' EXIT # any other exit restores everything
    status checking

    # Verify the list, then extract and check each listed file.
    signed "$ZIP" "$PACKAGE" "$KEY" "$WORK" || fail signature
    while IFS= read -r line; do
        take "$ZIP" "${line#*  }" "$WORK" || fail damaged
    done < "$WORK/list"
    (cd "$WORK/zip" && sha256sum -c "$WORK/list" >/dev/null 2>&1) || fail damaged
    NEW=$WORK/zip/autorun/$PACKAGE
    for name in sample-base restore-stock.sh sd-boot.sh sd-worker.sh sd-update.sh SHA256SUMS app VERSION; do
        [ -s "$NEW/$name" ] || fail damaged
    done
    for name in autorun.sh autorun.json autorun.check; do
        [ -s "$WORK/zip/autorun/$name" ] || fail damaged
    done
    VERSION=$(version_of "$NEW/VERSION") || fail damaged
    [ "$(cat "$NEW/app")" = "$APP" ] || fail other-app
    (cd "$NEW" && sha256sum -c SHA256SUMS >/dev/null 2>&1) || fail damaged
    status checking

    # Self-test the new binary in RAM before writing anything to the card.
    chmod 755 "$NEW/sample-base"
    "$NEW/sample-base" --self-test "$WORK/test.wav" >/dev/null 2>&1 || fail self-test
    rm -f "$WORK/test.wav"

    # Write the card. Refuse if its autorun.sh starts a different package.
    # The new folder is written and checked next to the old one, then the
    # two are renamed.
    [ -d "$CARD" ] || fail no-card
    if [ -f "$AUTORUN/autorun.sh" ] && ! grep -qF "/$PACKAGE/sd-boot.sh" "$AUTORUN/autorun.sh"; then
        fail other-app
    fi
    need=$(du -sk "$NEW" | cut -f 1)
    set -- $(df -Pk "$CARD" | tail -n 1)
    [ "${4:-0}" -gt $((need * 2 + 1024)) ] || fail card-full
    status writing
    rm -rf "$TARGET.next"
    mkdir -p "$AUTORUN" || fail card-write
    cp -r "$NEW" "$TARGET.next" || fail card-write
    sync
    (cd "$TARGET.next" && sha256sum -c SHA256SUMS >/dev/null 2>&1) || fail card-write
    for name in autorun.json autorun.sh autorun.check; do
        [ ! -f "$AUTORUN/$name" ] || cp "$AUTORUN/$name" "$WORK/old-$name" || fail card-write
    done
    rm -rf "$TARGET.prev"
    CARD_CHANGED=1
    if [ -d "$TARGET" ]; then mv "$TARGET" "$TARGET.prev" || fail card-write; fi
    mv "$TARGET.next" "$TARGET" || fail card-write
    for name in autorun.json autorun.sh autorun.check; do
        { cp "$WORK/zip/autorun/$name" "$AUTORUN/$name.next" && mv -f "$AUTORUN/$name.next" "$AUTORUN/$name"; } || fail card-write
    done
    sync

    # Stop this app and start the new one from the card with its sd-worker.sh.
    status starting
    OLD=$(readlink "$ACTIVE" 2>/dev/null || true)
    STOPPED=1
    stop_quietly
    /bin/sh "$TARGET/sd-worker.sh" "$TARGET" >&2 || fail no-start
    ready || fail no-start

    trap - EXIT
    status done
    rm -f "$ZIP"
    drop_stale
    rm -rf "$WORK"
    sync
}

case "${1:-}" in
    check) shift; check "$@" ;;
    install) shift; install "$@" ;;
    *) echo "usage: sd-update.sh check ZIP PACKAGE KEY | install ZIP CARD PACKAGE APP KEY" >&2; exit 2 ;;
esac
