/* SPDX-License-Identifier: AGPL-3.0-only
 * kernel/panel's driver module (GPL-2.0+, built by
 * tools/build_kernel_modules.sh) inside the binary, loaded by
 * src/drivers/panel_driver.cpp. PANEL_MODULE is its path from where make
 * runs (base.mk). */
    .section .rodata
    .balign 16
    .global sample_base_panel_module
    .global sample_base_panel_module_end
sample_base_panel_module:
    .incbin PANEL_MODULE
sample_base_panel_module_end:
    .section .note.GNU-stack,"",%progbits
