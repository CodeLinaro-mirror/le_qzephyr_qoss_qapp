QCC730 UART Stress Test Suite
============================

Test Overview
-------------

This test suite validates the robustness, stability, and performance of the
QCC730 UART driver implementation using Zephyr's ztress framework. The tests are
designed to ensure the UART driver remains stable under high-stress conditions,
including rapid configuration changes, high-throughput data transmission, and
long-duration endurance scenarios. The tests primarily focus on the
polling-based transmission (`uart_poll_out`) and configuration APIs.

Hardware Requirements
---------------------

* QCC730 board with UART0 enabled and configured as the Zephyr console.
* A host computer connected to the board's serial port to monitor test output.
* No external loopback connections are required, as the tests focus on the
  stability of the transmission (TX) path and QCC730 has anyway single UART
  only. 

Test Components
---------------

Configuration Stress Test
~~~~~~~~~~~~~~~~~~~~~~~~~

This test validates the stability of the UART driver when subjected to rapid and
concurrent configuration changes from multiple threads.

* **Execution**: Four concurrent threads continuously reconfigure the UART port,
  cycling through different settings for baud rate, parity, data bits, and stop
  bits.
* **Purpose**: To detect race conditions or instability in the driver's
  configuration logic and ensure that hardware registers are updated correctly
  under load.

**Test Parameters**:

* 1000 configuration changes per thread.
* 10-second timeout.
* < 0.1% error tolerance.

**Success Criteria**: Completion of all operations with a minimum throughput of
1500 operations/second and minimal errors.

Throughput Test
~~~~~~~~~~~~~~~

This test measures the maximum effective data transmission rate of the UART
driver in polling mode.

* **Execution**: A single thread transmits a large block of data (100 KB) as
  quickly as possible using `uart_poll_out`.
* **Purpose**: To verify that the driver's polling implementation is efficient
  and can achieve a data rate close to the theoretical maximum for the
  configured baud rate.

**Success Criteria**: The measured throughput must be at least 80% of the
theoretical maximum for the configured baud rate (e.g., > 9216 bytes/sec for
115200 baud 8N1).

Endurance Test
~~~~~~~~~~~~~~

This is a long-duration test designed to detect issues that may only appear over
extended periods, such as memory leaks, performance degradation, or driver
instability.

* **Execution**: Three threads run concurrently for an extended duration (60
  seconds):
    * One thread performs rapid configuration changes.
    * Two threads continuously transmit data using `uart_poll_out`.
* **Purpose**: To ensure the driver remains stable and performant during
  prolonged, high-intensity usage.

**Test Parameters**:

* 10,000 transmission operations per TX thread.
* 1,000 configuration operations.
* 60-second timeout.
* < 0.1% error tolerance.

**Success Criteria**: Sustained performance of at least 1500 operations/second
over the entire test duration without crashing or accumulating significant
errors.

Driver Features Validated
--------------------------

Thread Safety
~~~~~~~~~~~~~

* **Concurrent API Calls**: Ensures that simultaneous calls to `uart_configure`
  and `uart_poll_out` from multiple threads do not lead to corruption or
  deadlocks.
* **State Management**: Validates that the driver's internal state remains
  consistent during preemptive multitasking.

Configuration Management
~~~~~~~~~~~~~~~~~~~~~~~~

* **Rapid Reconfiguration**: Verifies the driver's ability to handle frequent
  changes to baud rate, parity, stop bits, and data bits without errors.
* **Stability**: Confirms that applying new configurations does not disrupt
  ongoing operations or cause the hardware to enter an invalid state.

Performance and Throughput
~~~~~~~~~~~~~~~~~~~~~~~~~~

* **Polling Efficiency**: Measures the performance of the `uart_poll_out`
  function to ensure low-overhead, high-speed data transmission.
* **Sustained Load**: Confirms that the driver can maintain high performance
  over extended periods without degradation.

Configuration Options
---------------------

The test's behavior can be modified by changing compile-time parameters defined in `main.c`:

.. code-block:: c

   #define ZTRESS_ITERATIONS                  1000U         // Ops for config/interrupt tests
   #define ZTRESS_ENDURANCE_POLL_ITERATIONS   10000U        // Polling ops for endurance test
   #define ZTRESS_MIN_PREEMPTIONS             50U           // Minimum preemptions for ztress threads
   #define ZTRESS_TEST_TIMEOUT_MS             10000U        // Standard test timeout (10s)
   #define ZTRESS_ENDURANCE_TEST_TIMEOUT_MS   60000U        // Endurance test timeout (60s)
   #define THROUGHPUT_TEST_SIZE               (100 * 1024)  // 100KB for throughput test
   #define ZTRESS_MINIMUM_OPERATIONS_PER_SEC  1500U         // Minimum performance threshold
   #define ZTRESS_ERRORS_PERCENTAGE_TOLEARNCE 0.1           // Error tolerance in percent

Known Limitations
-----------------

* **TX Path Focus**: As the tests use the console UART, they primarily validate
  the transmission (TX) path. Full validation of the receive (RX) path would
  require a physical loopback and a test harness that does not rely on the
  console for communication. As QCC730 supports only single UART and creating
  loopback causes console not working then full RX and TX will be validated in
  dedicated driver pytest tests.
* **Interrupt Storm Test Skipped**: The interrupt-based stress test is skipped
  because the QCC730 has a single UART instance, which is used for the console.
  Performing a loopback test would interfere with test logging. Interrupt
  functionality is validated in separate, dedicated driver pytest tests.