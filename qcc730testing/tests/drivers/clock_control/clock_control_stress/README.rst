SPDX-License-Identifier: BSD-3-Clause
Copyright (c) 2025 Qualcomm Technologies, Inc.

QCC730 Clock Control Stress Test Suite
======================================

Test Overview
-------------

This test suite validates the robustness and reliability of the QCC730 Clock
Control driver implementation using Zephyr's ztress framework. The tests are
designed to verify that the clock control subsystem remains stable under
high-stress conditions, concurrent operations, and extended runtime scenarios
typical of production IoT deployments.

Hardware Requirements
---------------------

Clock Subsystems Under Test
~~~~~~~~~~~~~~~~~~~~~~~~~~~~

The test exercises three clock subsystems that actively use the clock_control
API:

- **I2C Clock** (QCC730_CLOCK_I2C)
- **Qtimer Clock** (QCC730_CLOCK_QTIMER)
- **Watchdog Clock** (QCC730_CLOCK_WDOG)

UART and GPIO clocks are intentionally excluded from testing to maintain console
communication.

Test Components
---------------

Concurrent Operations Stress Test
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Uses the ztress framework to execute multiple clock control operations
simultaneously across different execution contexts. This test validates:

**Thread Safety**: Three concurrent threads perform different operations:

* Clock enable operations (ON) on I2C subsystem
* Clock disable operations (OFF) on Qtimer subsystem with preemption
* Status query operations (GET_STATUS) on Watchdog subsystem

**Operation Volume**: Each thread executes 3000 iterations, generating thousands
of clock control transactions to expose timing-related issues or resource
conflicts.

**-EALREADY Handling**: The test validates that the driver correctly handles
attempts to enable already-enabled clocks, returning -EALREADY as expected.

Mixed Operations Test
~~~~~~~~~~~~~~~~~~~~~

Each thread performs a randomized mix of ON, OFF, and STATUS operations across
all three subsystems. This validates:

* API consistency during unpredictable workload patterns
* Correct state transitions regardless of operation sequence
* Proper handling of mixed operation types
* No state corruption from operation interleaving

The test cycles through operations in a 3-way pattern (ON -> OFF -> STATUS) for
3000 iterations per thread across three concurrent contexts.

Endurance Test
~~~~~~~~~~~~~~

Executes continuous clock toggle operations across three subsystems
simultaneously. This long-running test validates:

* Extended stability under sustained load (100000 iterations per thread)
* No memory leaks or resource exhaustion
* Consistent performance throughout test duration
* Hardware reliability over thousands of clock domain transitions

The test monitors performance metrics throughout execution and verifies that
throughput remains as expected with zero errors.


Performance Metrics
-------------------

Each test validates performance against these thresholds:

**Minimum Throughput**
  1500 operations per second across all threads

**Error Tolerance**
  Maximum 1% error rate (typically expecting 0 errors)

**Operation Counts**
  * Concurrent operations: 3000 iterations per thread
  * High-frequency toggle: 10000 iterations per thread
  * Mixed operations: 3000 iterations per thread
  * Endurance test: 100000 iterations per thread

Performance validation occurs after each test completion. The test framework
automatically checks:

1. Total operations completed meet minimum expected count
2. Operations per second exceed minimum threshold (1500 ops/sec)
3. Error count is within tolerance (<= 1% of total operations)
4. Test completes before timeout expires


Implementation Notes
--------------------

**Clock Subsystem Selection**

The test only exercises clock subsystems that actively use the clock_control API
in their drivers (I2C, Qtimer, Watchdog). UART and GPIO are excluded to maintain
test infrastructure stability.

**Error Handling**

The driver correctly returns -EALREADY when attempting to enable an
already-enabled clock. The test treats this as a valid response and does not
count it as an error.

**State Management**

All tests include cleanup phases that restore clocks to a known enabled state,
ensuring test isolation and repeatability.

**Performance Validation**

The test validates both throughput (operations per second) and error rate to
ensure the driver meets quality requirements under stress conditions.
