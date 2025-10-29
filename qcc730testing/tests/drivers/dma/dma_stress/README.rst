SPDX-License-Identifier: BSD-3-Clause
Copyright (c) 2025 Qualcomm Technologies, Inc.

DMA Stress Test for QCC730
==========================

Overview
--------

This test validates the stability, concurrency, and data integrity of the DMA
driver implementation for the QCC730 platform. It uses the Zephyr **ztress**
framework to generate high-frequency, concurrent DMA transfers under varying
configurations, priorities, and memory regions to ensure the DMA subsystem
behaves correctly under extreme workloads.

Test Purpose
------------

The DMA stress test aims to:

- Verify **multi-channel concurrency** - multiple DMA channels can operate
  independently without data corruption or contention
- Confirm **transfer correctness** for single-block and multi-block transfers of
  various sizes
- Measure performance and responsiveness under heavy workloads
- Validate **DMA configuration robustness**, including reconfiguration and
  priority handling
- Check **end-to-end data integrity** across SRAM and RRAM regions
- Ensure proper error handling, synchronization, and completion signaling via
  DMA callbacks

Test Design Overview
--------------------

The test suite defines several ztress-based scenarios using independent worker
threads that run concurrently on multiple DMA channels. Each thread repeatedly
executes a DMA transfer pattern, monitors completion via per-channel semaphores,
and verifies copied memory content.

Test buffers are allocated in SRAM and RRAM memory regions and are 4-byte
aligned. Each DMA operation runs in **MEMORY_TO_MEMORY** mode using the standard
Zephyr DMA API.

Key parameters::

   ZTRESS_ITERATIONS       = 3000           (iterations per thread)
   ZTRESS_TIMEOUT          = K_SECONDS(12)  (per-test timeout)
   MIN_OPS_PER_SECOND      = 100            (minimum performance threshold)
   ERROR_TOLERANCE_PERCENT = 1              (acceptable error tolerance)

Test Cases
----------

1. **Concurrent Channel Operations**
   - Runs multiple DMA channels in parallel with different priorities and data
     patterns
   - Validates simultaneous configuration and data transfers across all active
     channels

2. **Rapid Reconfiguration**
   - Continuously reconfigures DMA channels between transfers using small block
     sizes
   - Checks for configuration consistency and that no stale state persists
     across transfers

3. **Memory Regions Test**
   - Performs transfers between **SRAM to RRAM** regions to verify cross-memory
     addressing and correct data propagation across address domains

4. **Block Size Variability**
   - Exercises transfers of small, medium, large, and multiblock configurations
   - Confirms correct descriptor chaining and buffer boundary handling

5. **Priority Stress**
   - Runs simultaneous high- and low-priority channels to verify correct
     scheduling and arbitration

Performance Metrics
-------------------

Each test measures total operations, elapsed time, and achieved throughput:

- **Minimum performance:** 100 operations per second
- **Allowed error rate:** ≤ 1% of total operations
- **Maximum duration:** 12 seconds per case

Failures are reported via Zephyr assertions if:

- Operation count falls below the expected minimum
- Data integrity validation fails
- DMA callbacks report nonzero status codes
- The system exceeds the timeout threshold

Understanding Performance Numbers
----------------------------------

The expected/reported operations per second (ops/sec) described in previous
chapter represents **complete DMA operation cycles**, not raw hardware
bandwidth. Each "operation" includes:

- Buffer preparation (``memset`` for source and destination)
- DMA channel configuration (``dma_config``)
- DMA transfer initiation (``dma_start``)
- Semaphore-based synchronization wait (``k_sem_take``)
- Interrupt handling and callback execution
- Context switching overhead
- Data verification (on final iteration)

**Typical observed performance:**

- Block sizes: 1200-1400 ops/sec
- Concurrent channels: 1300-1600 ops/sec
- Memory regions: 700-800 ops/sec
- Priority stress: 1300-1400 ops/sec
- Rapid reconfiguration: 1400-1500 ops/sec

**Why RRAM is slower:**

RRAM has lower throughput due to its different electrical characteristics and
longer access latency compared to SRAM. This is expected hardware behavior.

**Why 100 ops/sec minimum:**

The minimum threshold of 100 ops/sec is intentionally conservative to:

- Account for system variability and different hardware configurations
- Match slower performance in data transfer to RRAM
- Detect catastrophic failures (deadlocks, severe contention, driver hangs)
- Allow headroom for debug builds and logging overhead
- Provide margin for different CPU frequencies and system loads

The test validates **driver correctness and stability under stress**, not peak
hardware throughput.

Expected Results
----------------

- All DMA transfers complete successfully without timeout or data corruption
- Measured operations per second meet or exceed defined minimum thresholds
- Error counter remains within the error tolerance limit
- Multi-block and region transfers maintain consistent memory contents
- No driver crashes, kernel faults, or DMA lockups are observed

Notes
-----

- Test uses DXE node ``dxe0``
- DMA direction is fixed to ``MEMORY_TO_MEMORY``
- Each channel uses its own semaphore for synchronization
- The test is intensive and may increase memory and CPU load temporarily
- For extended endurance validation, increase iteration count and/ timeout
  values in source