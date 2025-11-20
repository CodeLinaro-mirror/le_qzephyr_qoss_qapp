SPDX-License-Identifier: BSD-3-Clause
Copyright (c) 2025 Qualcomm Technologies, Inc.

Counter Stress Test Suite
================================

Overview
--------

This test validates the robustness, concurrency, and endurance performance of
the Qtimer counter peripheral driver on the QCC730 SoC. It uses Zephyr's
**ztress** framework to simulate high-frequency, multi-threaded operations on
the counter device to ensure API stability under load and concurrent usage.

Test Purpose
------------

The purpose of this test is to stress-test the Counter (Qtimer) driver with
multiple simultaneous API calls and verify:

- No deadlocks or race conditions occur under concurrent access.
- The counter maintains correct operation during continuous start/stop, alarm,
  and read cycles.
- The driver correctly handles EBUSY conditions when alarms overlap.
- Performance metrics meet the expected operations per second threshold.
- Endurance behavior remains stable during long-duration stress workloads.

Test Design Overview
--------------------

The test uses several ztress threads running in parallel to perform various
counter operations:

- **Start/Stop operations:** Verify that the counter can be started and stopped
  repeatedly with value reading without error or race conditions.
- **Alarm setting and cancellation:** Alternating between short and long alarm
  ticks to validate the alarm scheduling and cancellation paths.
- **Endurance test:** Long-running scenario combining all above operations to
  ensure driver reliability over time.

Each test case collects statistics on total operations, number of errors, and
achieved operations per second.

Test Cases
----------

1. **Concurrent Start/Stop Test**
   - Runs multiple threads performing start, stop, and value-read operations concurrently.
   - Verifies that the driver remains functional and no API call returns unexpected errors.

2. **Relative Alarm Test**
   - Alternates between setting and cancelling alarms using relative ticks.
   - Checks correct handling of EBUSY when alarms overlap and proper callback triggering.

3. **Endurance Test**
   - Executes a long run (default 60 seconds) combining start, stop, set alarm, and cancel alarm
     operations to measure stability, performance, and error-free execution over time.

Performance Metrics
-------------------

- **Minimum operations per second:** 1500 ops/sec (defined by ``MIN_OPS_PER_SECOND``)
- **Maximum error tolerance:** 1% of total operations
- **Timeouts:** 10s for standard runs, 60s for endurance tests

Expected Results
----------------

- All ztress threads complete successfully within configured timeouts.
- Reported total operations exceed the expected minimum threshold.
- Error count is zero or within the 1% tolerance limit.
- No assertion failures or unexpected kernel faults occur.
- Alarm callbacks are triggered as expected.
