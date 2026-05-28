/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 *
 * Linker fragments for rcp_app:
 *   .rcp_shmem  — QcSPI shared ring buffers in RCP_SHMEM region (NOLOAD).
 *   .wifi_ram   — RCP protocol buffers/mutexes (rcp_protocol/buffer_mgmt.c
 *                 uses __attribute__((section(".wifi_ram")))); must be in RAM.
 */

.rcp_shmem (NOLOAD) : ALIGN(4) {
    *(.rcp_shmem)
    *(.rcp_shmem.*)
} GROUP_LINK_IN(RCP_SHMEM)

.wifi_ram (NOLOAD) : ALIGN(4) {
    *(.wifi_ram)
    *(.wifi_ram.*)
} GROUP_LINK_IN(RAMABLE_REGION)
