# Zephyr Patches Directory

This directory contains patches that will be automatically applied to the Zephyr source code before building.

## Available Patches

### 0001_lrChangeForDebug_thread.patch
**Purpose:** Initialize LR (Link Register) to 0 in thread initialization to prevent GDB backtrace timeout

**File Modified:** zephyr/arch/arm/core/cortex_m/thread.c

**Description:** 
This patch adds initialization of the Link Register (LR) to 0 in the arch_new_thread() function.

## How to Use

### Apply Patches
python qapp/patch/apply_patches.py apply

### Check Status
python qapp/patch/apply_patches.py status

### Revert Patches
python qapp/patch/apply_patches.py revert
