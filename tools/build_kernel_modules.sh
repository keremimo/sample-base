#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-only
# Builds the modules under kernel/<name>/ (each folder's Kbuild names its
# module) against the MPC Sample's kernel (6.12.60-imb, PREEMPT_RT) and
# leaves each .ko beside its source, where it is kept in Git
# (tools/check_private_files.py lets a module through beside the C it is
# built from) and the build embeds it (src/drivers/panel_module.S,
# src/drivers/kernel_modules.S). Run it after changing a module's source, and
# commit both. Needs inMusic's kernel source in .local/kbuild/kernel-source
# (its public release) and the MPC's /proc/config.gz as
# .local/mpc-kernel.config; the tree is prepared once. With names, only those
# folders: tools/build_kernel_modules.sh uvc vepu
set -eu
cd "$(dirname "$0")/.."
[ -d .local/kbuild/kernel-source ] && [ -f .local/mpc-kernel.config ] || {
    echo "Needs .local/kbuild/kernel-source and .local/mpc-kernel.config" >&2; exit 1; }
names=${*:-$(for kbuild in kernel/*/Kbuild; do basename "$(dirname "$kbuild")"; done)}
docker build -q --platform linux/arm64 -t sample-base-kbuild tools/kbuild >/dev/null
for name in $names; do
    [ -f "kernel/$name/Kbuild" ] || { echo "kernel/$name has no Kbuild" >&2; exit 1; }
    rm -rf "build/kernel-$name" && mkdir -p "build/kernel-$name"
    cp "kernel/$name"/Kbuild "kernel/$name"/*.c "build/kernel-$name/"
    for header in "kernel/$name"/*.h; do [ -f "$header" ] && cp "$header" "build/kernel-$name/"; done
    docker run --rm --platform linux/arm64 -v "$PWD/.local:/k" -v "$PWD/build/kernel-$name:/m" sample-base-kbuild sh -c '
        set -eu
        cd /k/kbuild/kernel-source
        [ -f include/config/kernel.release ] || { cp /k/mpc-kernel.config .config; make -s olddefconfig; make -s -j8 modules_prepare; }
        # The prepared tree has no Module.symvers: modpost cannot see the
        # kernel'"'"'s symbols, which resolve when the module loads.
        make -s -j8 M=/m KBUILD_MODPOST_WARN=1 modules 2>&1 | grep -v -e "undefined!" -e "Module.symvers" -e "modversions" \
            -e "unresolved symbol errors" -e "KBUILD_MODPOST_WARN" -e "own risk" -e "suppressed" || true
        for ko in /m/*.ko; do
            strip --strip-debug "$ko"
            modinfo "$ko" | grep -E "^(name|vermagic|parm):"
        done'
    for ko in "build/kernel-$name"/*.ko; do
        cp "$ko" "kernel/$name/"
        echo "kernel/$name/$(basename "$ko")"
    done
done
