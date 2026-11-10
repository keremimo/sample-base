#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-only
set -eu
# Shutdown already owns service teardown; do not restart an application then.
state=$(/bin/systemctl is-system-running 2>/dev/null || true)
case "$state" in
    stopping|offline) exit 0 ;;
esac
# sample-base widened the USB-C audio gadget for stems (src/drivers/usb_gadget.h):
# the stock app gets its original channel mask back. Any failure here still
# starts the stock app.
SAVED=/run/sample-base-uac2-p_chmask
if [ -r "$SAVED" ] && read -r FUNCTION MASK < "$SAVED"; then
    GADGET=${FUNCTION%/functions/*}
    case "$FUNCTION" in /sys/kernel/config/usb_gadget/*/functions/uac2.*) ;; *) FUNCTION= ;; esac
    if [ -n "$FUNCTION" ] && [ -f "$FUNCTION/p_chmask" ] && [ "$(cat "$FUNCTION/p_chmask")" != "$MASK" ]; then
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
        for ENTRY in $LINKS; do ln -s "${ENTRY#*=}" "${ENTRY%%=*}" 2>/dev/null || true; done
        { [ -z "$UDC" ] || echo "$UDC" > "$GADGET/UDC"; } 2>/dev/null || true
    fi
    rm -f "$SAVED"
fi
exec /bin/systemctl start ac50.service
