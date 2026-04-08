SPDX-License-Identifier: BSD-3-Clause-Clear
Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.

QCC730 RTC Stress Test Suite
=============================

Test Overview
-------------

This test suite validates the robustness, stability, and performance of the
QCC730 RTC driver implementation using Zephyr's ztress framework. The tests are
designed to ensure the RTC subsystem remains stable under high-stress
conditions, concurrent operations, and extended runtime scenarios.

Hardware Requirements
---------------------

* QCC730 board with RTC enabled
* RTC device accessible via device tree alias ``rtc``
* Qtimer operating at 38.4 kHz clock frequency
* CONFIG_RTC_ALARM must be enabled for full test coverage

No external hardware connections are required as the tests validate software
APIs and internal timing mechanisms.

Test Components
---------------

Concurrent Operations Stress Test
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Uses the ztress framework to execute multiple RTC operations simultaneously
across different execution contexts. This test validates:

**Thread Safety**: Four concurrent threads perform different operations:

* Rapid time set operations with incrementing timestamps
* Continuous time read operations with field validation
* Alarm management operations (get/set/cancel/pending checks)
* Callback registration/deregistration with supported fields validation

**Operation Volume**: Each thread executes a minimum of 3000 iterations,
generating 12,000+ RTC API transactions to expose timing-related issues or
resource conflicts.

**Test Parameters**:

* 3,000 operations per thread (12,000 total)
* 10-second timeout
* Minimum 1500 operations/second throughput

Endurance Test
~~~~~~~~~~~~~~

Executes mixed RTC operations continuously for an extended duration across three
threads simultaneously. This is a long-duration test designed to detect issues
that may only appear over extended periods, such as memory leaks, performance
degradation, or driver instability.

**Execution**: Three concurrent threads run for up to 60 seconds:

* One thread performs rapid time set operations
* One thread continuously reads and validates time values
* One thread performs alarm operations (get/cancel/pending checks)

**Test Parameters**:

* 10,000 operations per thread (30,000 total)
* 60-second timeout
* Minimum 1500 operations/second sustained throughput


Driver Features Validated
-------------------------

Thread Safety
~~~~~~~~~~~~~

* **Concurrent API Calls**: Ensures that simultaneous calls to
  ``rtc_set_time()``, ``rtc_get_time()``, alarm management, and callback
  registration from multiple threads do not lead to corruption or deadlocks.
* **State Management**: Validates that the driver's internal state (spinlocks,
  alarm settings, callback pointers) remains consistent during preemptive
  multitasking.
* **Alarm Synchronization**: Verifies proper synchronization between alarm
  operations, time updates, and callback execution.

Time Management
~~~~~~~~~~~~~~~

* **Rapid Time Updates**: Verifies the driver's ability to handle frequent time
  set operations with varying timestamps without errors.
* **Time Validation**: Confirms that ``rtc_get_time()`` always returns valid
  time values (proper ranges for hours, minutes, seconds, days, months).
* **Nanosecond Precision**: Tests nanosecond field handling during concurrent
  operations.

Alarm Functionality
~~~~~~~~~~~~~~~~~~~

* **Alarm Lifecycle**: Tests complete alarm lifecycle including set, get,
  cancel, and pending status checks.
* **Supported Fields**: Validates ``rtc_alarm_get_supported_fields()`` returns
  correct mask under stress.
* **Callback Management**: Tests callback registration, deregistration, and NULL
  callback handling.
* **Pending Status**: Verifies ``rtc_alarm_is_pending()`` clear-on-read behavior
  works correctly under concurrent access.

Performance and Throughput
~~~~~~~~~~~~~~~~~~~~~~~~~~

* **API Efficiency**: Measures RTC API performance to ensure low-overhead,
  high-speed operation.
* **Sustained Load**: Confirms that the driver maintains high performance over
  extended periods without degradation.
* **Concurrent Throughput**: Validates minimum 1500 operations/second with four
  concurrent threads.

Known Limitations
-----------------

* **Single Alarm Support**: The QCC730 RTC driver supports only one alarm (ID
  0). Tests are designed for this configuration.
* **Preemption Achievement**: The highest priority thread (set_time) may not
  achieve the full 100 preemptions within the 10-second timeout due to priority
  scheduling. This is expected behavior and does not indicate a driver issue.
* **Qtimer Dependency**: The RTC driver depends on qtimer being properly
  initialized and running at 38.4 kHz.
