<!-- Copyright (c) 2025 Qualcomm Technologies, Inc. and/or its subsidiaries -->
<!-- SPDX-License-Identifier: BSD-3-Clause-Clear -->

# Zephyr patching script (patch_zephyr.py)
Zephyr tests and samples need some patching before they can be build and run on QCC730 boards. This script will copy overlays and patch source code accordingly in the Zephyr working copy.

It is assumed here that you're in the root directory of our repository and you have activated the virtual environment. However it doesn't matter which directory you are in, when you run the script

## Get usage help for the script
```python scripts/tests/patch_zephyr/patch_zephyr.py```

Just run the script without parameters to get usage help.

## Remove current changes to Zephyr working copy
```python scripts/tests/patch_zephyr/patch_zephyr.py rm```

***This will hard reset the zephyr working copy and remove all untracked files, so be careful if you have been doing stuff in the zephyr working copy, you will lose changes not saved elsewhere.***

## Install overlays and patches to Zephyr working copy
```python scripts/tests/patch_zephyr/patch_zephyr.py inst```

This will copy tests and samples overlays to the Zephyr working copy. It will also patch the source code for tests and samples.

## Update tests and samples in Zephyr working copy
```python scripts/tests/patch_zephyr/patch_zephyr.py rm inst```

This will first clean up the Zephyr working copy and then install overlays and patches.

## Troubleshooting
If anything goes wrong it's most likely because there already are changes Zephyr working copy. Just use the 'rm' feature before installing again.
