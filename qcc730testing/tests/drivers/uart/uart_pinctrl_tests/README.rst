.. _uart_pinctrl_tests:

UART Pinctrl Tests
##################

Overview
********

This test validates that the QCC730 UART pinctrl integration correctly applies
different UART pin option groups and that the SoC PMU register reflects the
configured option.

The QCC730 SoC supports four UART pin options (0–3). The test applies each
option's pins directly via the pinctrl runtime API and verifies that
``PMU_BOOT_STRAP_CONFIGURATION_STATUS.CFG_UART_OPTION`` updates to 0/1/2/3
accordingly, then restores option 1 to keep the console on uart0.

Test Details
************

Configuration
=============

``prj.conf`` enables dynamic pinctrl.

- ``CONFIG_PINCTRL=y``
- ``CONFIG_PINCTRL_DYNAMIC=y``

How it works
============

1. Decode and assert DTS encodings for each option using the SoC pinmux
    helper macros from ``qcom-qcc730-pinctrl.h``.
2. Apply each option's pin array with ``pinctrl_configure_pins``. The QCC730
    pinctrl driver updates ``CFG_UART_OPTION`` based on the encoded
    ``uart_opt`` in the pin entries.
3. Restore option 1 before printing, so the console (on uart0 option 1) stays
    alive.

Expected Output
==========================

- Logs of DTS pin parsing for each option, e.g.:

   ``DT pins for uart0_option0: count=2``

   ``pin[0]: pin=11 func=2 mode=2 uart_opt=0 pu=1 pd=0 ds=1``

- Summary of observed UART option values after restore:

   ``CFG_UART_OPTION observed: opt0=0 opt1=1 opt2=2 opt3=3``

All assertions must pass.

Notes
=====

- The board's ``&uart0`` only defines the standard states: ``default`` and
  ``sleep``. ``pinctrl_apply_state()`` can only switch among states present
  in the device's pinctrl configuration. 
- Hence using ``pinctrl_configure_pins()`` to apply the option pin
  groups directly without altering the device's state list, keeping the
  console stable and the Devicetree simple, without changing bindings.