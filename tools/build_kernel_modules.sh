#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-only
# Builds kernel/panel/sample_base_panel.ko against the MPC Sample's kernel
# (6.12.60-imb, PREEMPT_RT) and leaves it beside its source, where it is kept
# in Git (tools/check_private_files.py lets a module by its source through)
# and the build embeds it (src/drivers/panel_module.S). Run it after changing
# the source, and commit both. Needs inMusic's kernel source in
# .local/kbuild/kernel-source (its public release) and the MPC's
# /proc/config.gz as .local/mpc-kernel.config; the tree is prepared once.
set -eu
cd "$(dirname "$0")/.."
[ -d .local/kbuild/kernel-source ] && [ -f .local/mpc-kernel.config ] || {
    echo "Needs .local/kbuild/kernel-source and .local/mpc-kernel.config" >&2; exit 1; }
docker build -q --platform linux/arm64 -t sample-base-kbuild tools/kbuild >/dev/null
rm -rf build/kernel-panel && mkdir -p build/kernel-panel
cp kernel/panel/sample_base_panel.c kernel/panel/Kbuild build/kernel-panel/
docker run --rm --platform linux/arm64 -v "$PWD/.local:/k" -v "$PWD/build/kernel-panel:/m" sample-base-kbuild sh -c '
    set -eu
    cd /k/kbuild/kernel-source
    [ -f include/config/kernel.release ] || { cp /k/mpc-kernel.config .config; make -s olddefconfig; make -s -j8 modules_prepare; }
    make -s -j8 M=/m KBUILD_MODPOST_WARN=1 modules
    strip --strip-debug /m/sample_base_panel.ko
    modinfo /m/sample_base_panel.ko | grep -E "^(vermagic|depends|parm):"'
cp build/kernel-panel/sample_base_panel.ko kernel/panel/sample_base_panel.ko
echo "kernel/panel/sample_base_panel.ko"
