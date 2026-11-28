/* SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 Kerem Kilic (Ellic Studio)
 * Screen Mirror's kernel modules (GPL, built by tools/build_kernel_modules.sh)
 * inside the binary, loaded by src/drivers/screen_mirror.cpp: kernel/uvc's
 * webcam function and kernel/vepu's JPEG encoder switch. UVC_MODULE and
 * VEPU_MODULE are the builds for firmware 1.3.0.12's kernel, UVC_MODULE_6_18
 * and VEPU_MODULE_6_18 those for 1.4's; each is its path from where make
 * runs (base.mk), and a pair not defined is left out. */
    .section .rodata
#if defined(UVC_MODULE) && defined(VEPU_MODULE)
    .balign 16
    .global sample_base_uvc_module
    .global sample_base_uvc_module_end
sample_base_uvc_module:
    .incbin UVC_MODULE
sample_base_uvc_module_end:
    .balign 16
    .global sample_base_vepu_module
    .global sample_base_vepu_module_end
sample_base_vepu_module:
    .incbin VEPU_MODULE
sample_base_vepu_module_end:
#endif
#if defined(UVC_MODULE_6_18) && defined(VEPU_MODULE_6_18)
    .balign 16
    .global sample_base_uvc_6_18_module
    .global sample_base_uvc_6_18_module_end
sample_base_uvc_6_18_module:
    .incbin UVC_MODULE_6_18
sample_base_uvc_6_18_module_end:
    .balign 16
    .global sample_base_vepu_6_18_module
    .global sample_base_vepu_6_18_module_end
sample_base_vepu_6_18_module:
    .incbin VEPU_MODULE_6_18
sample_base_vepu_6_18_module_end:
#endif
    .section .note.GNU-stack,"",%progbits
