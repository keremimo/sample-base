/* SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 Kerem Kilic (Ellic Studio)
 * File Sharing's module (kernel/ksmbd: the kernel's SMB server, GPL-2.0+,
 * built by tools/build_kernel_modules.sh) inside the binary, loaded by
 * src/drivers/file_share.cpp: one build for firmware 1.3.0.12's kernel
 * (SMB_MODULE) and one for 1.4's (SMB_MODULE_6_18), each its path from where
 * make runs (base.mk), each left out when not defined. */
    .section .rodata
#ifdef SMB_MODULE
    .balign 16
    .global sample_base_smb_module
    .global sample_base_smb_module_end
sample_base_smb_module:
    .incbin SMB_MODULE
sample_base_smb_module_end:
#endif
#ifdef SMB_MODULE_6_18
    .balign 16
    .global sample_base_smb_6_18_module
    .global sample_base_smb_6_18_module_end
sample_base_smb_6_18_module:
    .incbin SMB_MODULE_6_18
sample_base_smb_6_18_module_end:
#endif
    .section .note.GNU-stack,"",%progbits
