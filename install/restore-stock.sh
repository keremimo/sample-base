#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-only
set -eu
# Shutdown already owns service teardown; do not restart an application then.
state=$(/bin/systemctl is-system-running 2>/dev/null || true)
case "$state" in
    stopping|offline) exit 0 ;;
esac
# A card lent to a computer (src/drivers/card_share.h) comes back first: the
# medium out of the gadget even if the computer holds it, the mass-storage
# function unlinked and removed, the other functions linked again in the
# order the saved line names (the card share moved the network function
# after the drive), the card flushed and mounted by edisksd.
CARD_SAVED=/run/sample-base-card-share
DISK= ORDER=
if [ -r "$CARD_SAVED" ]; then read -r DISK ORDER < "$CARD_SAVED" || true; fi
case "$DISK" in *[!a-z0-9]*) DISK= ;; esac
for CARD_FUNCTION in /sys/kernel/config/usb_gadget/*/functions/mass_storage.sd; do
    [ -d "$CARD_FUNCTION" ] || continue
    GADGET=${CARD_FUNCTION%/functions/*}
    { echo > "$CARD_FUNCTION/lun.0/file" || echo 1 > "$CARD_FUNCTION/lun.0/forced_eject"; } 2>/dev/null || true
    UDC=$(cat "$GADGET/UDC" 2>/dev/null || true)
    { [ -z "$UDC" ] || echo > "$GADGET/UDC"; } 2>/dev/null || true
    rm -f "$GADGET"/configs/*/mass_storage.sd 2>/dev/null || true
    if [ -n "$ORDER" ]; then
        for CONFIG in "$GADGET"/configs/*/; do
            for NAME in $ORDER; do
                case "$NAME" in *[!a-zA-Z0-9._]*|mass_storage.sd) continue ;; esac
                [ -d "$GADGET/functions/$NAME" ] || continue
                rm -f "$CONFIG$NAME" 2>/dev/null || true
                ln -s "$GADGET/functions/$NAME" "$CONFIG$NAME" 2>/dev/null || true
            done
            break
        done
    fi
    rmdir "$CARD_FUNCTION" 2>/dev/null || true
    { [ -z "$UDC" ] || echo "$UDC" > "$GADGET/UDC"; } 2>/dev/null || true
done
if [ -n "$DISK" ] && [ -b "/dev/$DISK" ]; then
    sync
    for PART in /sys/block/"$DISK"/"$DISK"*; do
        [ -e "$PART" ] || continue
        busctl call io.github.inmusicdev.edisksd "/io/github/inmusicdev/edisksd/dev/${PART##*/}" \
            io.github.inmusicdev.edisksd.Filesystem Mount 'a{sv}' 0 >/dev/null 2>&1 || true
    done
fi
rm -f "$CARD_SAVED"
# Screen Mirror's webcam (src/drivers/screen_mirror.h) stays in the gadget
# while an app restarts: the stock app gets the gadget without it. Its links
# go first, then its folders deepest first (configfs keeps the ones it made
# itself until the function's own rmdir), then its module.
for MIRROR in /sys/kernel/config/usb_gadget/*/functions/uvc.screen; do
    [ -d "$MIRROR" ] || continue
    GADGET=${MIRROR%/functions/*}
    UDC=$(cat "$GADGET/UDC" 2>/dev/null || true)
    { [ -z "$UDC" ] || echo > "$GADGET/UDC"; } 2>/dev/null || true
    rm -f "$GADGET"/configs/*/uvc.screen 2>/dev/null || true
    find "$MIRROR" -type l -exec rm -f {} \; 2>/dev/null || true
    find "$MIRROR" -mindepth 1 -depth -type d -exec rmdir {} \; 2>/dev/null || true
    rmdir "$MIRROR" 2>/dev/null || true
    { [ -z "$UDC" ] || echo "$UDC" > "$GADGET/UDC"; } 2>/dev/null || true
done
rmmod sample_base_uvc 2>/dev/null || true
# sample-base widened the USB-C audio gadget for stems and set its rate to the
# audio device's (src/drivers/usb_gadget.h): the stock app gets its original
# channel mask and rate back (44.1 kHz when an older line names none). Any
# failure here still starts the stock app.
SAVED=/run/sample-base-uac2-p_chmask
RATE=
if [ -r "$SAVED" ] && read -r FUNCTION MASK RATE < "$SAVED"; then
    case "$RATE" in ''|*[!0-9,]*) RATE=44100 ;; esac
    GADGET=${FUNCTION%/functions/*}
    case "$FUNCTION" in /sys/kernel/config/usb_gadget/*/functions/uac2.*) ;; *) FUNCTION= ;; esac
    if [ -n "$FUNCTION" ] && [ -f "$FUNCTION/p_chmask" ] && { [ "$(cat "$FUNCTION/p_chmask")" != "$MASK" ] ||
            [ "$(cat "$FUNCTION/p_srate" 2>/dev/null)" != "$RATE" ] || [ "$(cat "$FUNCTION/c_srate" 2>/dev/null)" != "$RATE" ]; }; then
        UDC=$(cat "$GADGET/UDC" 2>/dev/null || true)
        { [ -z "$UDC" ] || echo > "$GADGET/UDC"; } 2>/dev/null || true
        # f_uac2 takes a mask only while no configuration links it. configfs
        # lists links newest first; reversed, they are in the order they go back.
        # It resolves a relative target from the working directory, not the
        # link's, so each goes back to the function's absolute directory.
        LINKS=
        for LINK in $(find "$GADGET"/configs/*/ -maxdepth 1 -type l 2>/dev/null | sed '1!G;h;$!d'); do
            TARGET=$(readlink "$LINK")
            LINKS="$LINKS $LINK=$GADGET/functions/${TARGET##*/}"
            rm -f "$LINK" 2>/dev/null || true
        done
        echo "$MASK" > "$FUNCTION/p_chmask" 2>/dev/null || true
        echo "$RATE" > "$FUNCTION/p_srate" 2>/dev/null || true
        echo "$RATE" > "$FUNCTION/c_srate" 2>/dev/null || true
        for ENTRY in $LINKS; do ln -s "${ENTRY#*=}" "${ENTRY%%=*}" 2>/dev/null || true; done
        { [ -z "$UDC" ] || echo "$UDC" > "$GADGET/UDC"; } 2>/dev/null || true
    fi
    rm -f "$SAVED"
fi
# sample-base sets the codec link's pmdown_time to 0 only while its stream
# ends (src/drivers/codec_power.h); one killed right then leaves it there.
# The kernel's own 5000 ms goes back for the stock app.
for PMDOWN in /sys/class/sound/card*/device/*/pmdown_time; do
    if [ -w "$PMDOWN" ] && [ "$(cat "$PMDOWN" 2>/dev/null)" = 0 ]; then echo 5000 > "$PMDOWN" 2>/dev/null || true; fi
done
# sample-base drives the panel with its own module (kernel/panel,
# src/drivers/panel_driver.h); one killed before it gave the panel back leaves
# it there. The stock app needs st7796s.
SPI=/sys/bus/spi/devices/spi3.0
if [ -e "$SPI/driver_override" ] && [ "$(cat "$SPI/driver_override" 2>/dev/null)" = sample-base-panel ]; then
    { echo spi3.0 > /sys/bus/spi/drivers/sample-base-panel/unbind; } 2>/dev/null || true
    { echo > "$SPI/driver_override"; } 2>/dev/null || true
    { [ -e "$SPI/driver" ] || echo spi3.0 > /sys/bus/spi/drivers/st7796s/bind; } 2>/dev/null || true
fi
rmmod sample_base_panel 2>/dev/null || true
# An app may stop the stock web server to serve its own page on port 80
# (Melodee's, src/melodee/web/); it comes back with the stock app.
/bin/systemctl start az0x-webserver.service 2>/dev/null || true
exec /bin/systemctl start ac50.service
