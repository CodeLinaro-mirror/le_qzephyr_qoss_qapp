QCC730 UART Power Management Test
==================================

Overview
--------

Tests UART driver power management with automatic pinctrl state transitions
during suspend/resume operations.

Test Cases
----------

**test_suspend_resume_success**
  - Suspends UART and verifies operations return ``-EBUSY``
  - Resumes UART and verifies operations work normally
  - Validates PM state transitions (ACTIVE ↔ SUSPENDED)

**test_multiple_suspend_resume_cycles**
  - Performs 5 suspend/resume cycles
  - Verifies driver stability across multiple PM transitions

Expected Behavior
-----------------

During **suspend**:
  - All UART operations return ``-EBUSY``
  - Pinctrl switches to sleep state (no bias, low drive)
  - Device state = ``PM_DEVICE_STATE_SUSPENDED``

During **resume**:
  - UART operations work normally
  - Pinctrl switches to default state (pull-up, high drive)
  - Device state = ``PM_DEVICE_STATE_ACTIVE``