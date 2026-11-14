/* SPDX-License-Identifier: AGPL-3.0-only
 * Screen Mirror's kernel modules (GPL, built by tools/build_kernel_modules.sh)
 * inside the binary, loaded by src/drivers/screen_mirror.cpp: kernel/uvc's
 * webcam function and kernel/vepu's JPEG encoder switch. UVC_MODULE and
 * VEPU_MODULE are their paths from where make runs (base.mk). */
    .section .rodata
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
    .section .note.GNU-stack,"",%progbits
