/* SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 Kerem Kilic (Ellic Studio)
 * kernel/panel's driver module (GPL-2.0+, built by
 * tools/build_kernel_modules.sh) inside the binary, loaded by
 * src/drivers/panel_driver.cpp: one build for firmware 1.3.0.12's kernel
 * (PANEL_MODULE) and one for 1.4's (PANEL_MODULE_6_18), each its path
 * from where make runs (base.mk), each left out when not defined. */
    .section .rodata
#ifdef PANEL_MODULE
    .balign 16
    .global sample_base_panel_module
    .global sample_base_panel_module_end
sample_base_panel_module:
    .incbin PANEL_MODULE
sample_base_panel_module_end:
#endif
#ifdef PANEL_MODULE_6_18
    .balign 16
    .global sample_base_panel_6_18_module
    .global sample_base_panel_6_18_module_end
sample_base_panel_6_18_module:
    .incbin PANEL_MODULE_6_18
sample_base_panel_6_18_module_end:
#endif
    .section .note.GNU-stack,"",%progbits
