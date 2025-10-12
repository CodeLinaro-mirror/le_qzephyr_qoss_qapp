QCC730 I2C Stress Test Suite
============================

Test Overview
-------------

This test suite validates the robustness and reliability of the QCC730 I2C
driver implementation using Zephyr's ztress framework. The tests verify that the
I2C subsystem remains stable under high-stress conditions, concurrent
operations, speed transitions, and extended runtime scenarios.

Hardware Requirements
---------------------

Basic Configuration
~~~~~~~~~~~~~~~~~~~

* QCC730 board with I2C0 enabled.

Extended Configuration
~~~~~~~~~~~~~~~~~~~~~~

* LIS2DW12 accelerometer connected to I2C0 (address 0x19)

Test Components
---------------

Concurrent Operations Test
~~~~~~~~~~~~~~~~~~~~~~~~~~

Validates thread safety and synchronization by executing four simultaneous
operation types:

* **Configuration Thread**: Rapid speed transitions between Standard (100kHz)
  and Fast (400kHz) modes
* **Multi-Device Thread**: Address switching between LIS2DW12 (0x19) and
  secondary device (0x50)
* **Sensor Write Thread**: Direct WHO_AM_I register reads from LIS2DW12
* **Zero-Length Thread**: Zero-message transfers to test minimal overhead
  operations

**Test Parameters**:

* 3000 operations per thread (except multi-device: 1000)
* 10-second timeout
* 1% error tolerance

**Success Criteria**: Minimum 1500 operations/second with <= 1% communication errors.

High-Frequency Configuration Test
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Stresses the I2C configuration subsystem:

* Two concurrent threads performing 10,000 operations each
* One thread alternates speed configurations
* Second thread performs zero-length transfers
* Tests configuration change stability under load

**Success Criteria**: 20,000 total operations completed within timeout, minimum 1500 operations/second.

Sensor Communication Test
~~~~~~~~~~~~~~~~~~~~~~~~~

Real-world stress testing using LIS2DW12 accelerometer (skipped if sensor
unavailable):

* Four concurrent threads accessing sensor
* Three threads use Zephyr sensor API for acceleration data
* One thread performs direct register reads
* Mixed single-axis and multi-axis operations

**Test Parameters**:

* 3000 operations per thread
* Validates data integrity through sensor API

**Success Criteria**: Minimum 1500 operations/second with < 1% error tolerance.

Endurance Test
~~~~~~~~~~~~~~

Long-duration stability test:

* Four threads performing 50,000 operations each
* Sustained sensor communication over extended period
* Memory leak and resource exhaustion detection
* Performance degradation monitoring

**Test Parameters**:

* 200,000 total operations target
* 150 seconds timeout
* 1% error tolerance

**Success Criteria**: Minimum 1500 operations/second sustained over entire duration.

Driver Features Validated
--------------------------

Thread Safety
~~~~~~~~~~~~~

* **Semaphore Locking** (``lock_sem``): Ensures exclusive access during transfers
* **Concurrent Operations**: Multiple threads safely accessing I2C bus
* **Address Switching**: Rapid target address changes without corruption

Configuration Management
~~~~~~~~~~~~~~~~~~~~~~~~

* **Speed Transitions**: Standard (100kHz) to Fast (400kHz) mode switching
* **Lazy Configuration**: Skips redundant configurations for efficiency
* **State Tracking**: Maintains I2C_INIT and I2C_SETUP state flags

Error Handling
~~~~~~~~~~~~~~

* **Timeout Detection**: ``CONFIG_I2C_QCC730_TIMEOUT_MS`` (100ms default)
* **FIFO Management**: TX/RX FIFO overflow/underflow handling
* **Address NACK Handling**: Graceful handling of non-existent devices
* **10-bit Address Rejection**: Returns -ENOTSUP as expected

Performance Metrics
~~~~~~~~~~~~~~~~~~~

* **Throughput**: Validates minimum 1500 operations/second
* **Latency**: WHO_AM_I operations complete within 10ms
* **CPU Load**: Reported by ztress framework (typically 30-80%)

Configuration Options
---------------------

Compile-Time Parameters
~~~~~~~~~~~~~~~~~~~~~~~~

Modify test behavior via definitions in ``main.c``:

.. code-block:: c

   #define ZTRESS_ITERATIONS                  3000    // Operations per thread
   #define ZTRESS_SENSOR_SCAN_ITERATIONS      1000    // Multi-device operations
   #define ZTRESS_HIGH_ITERATIONS             10000   // High-frequency test ops
   #define ZTRESS_ENDURANCE_ITERATIONS        50000   // Endurance test ops
   #define ZTRESS_TEST_TIMEOUT_MS             10000U  // Standard test timeout
   #define ZTRESS_ENDURANCE_TIMEOUT_MS        150000U // Endurance test timeout (150 sec)
   #define ZTRESS_MINIMUM_OPERATIONS_PER_SEC  1500U   // Performance threshold
   #define ZTRESS_ERRORS_PERCENTAGE_TOLERANCE 1       // Error tolerance (%)

Known Limitations
-----------------

* Initial 5 timeouts may occur during sensor initialization
* Zero-length message scanning not fully supported by driver
* 10-bit addressing not implemented (returns -ENOTSUP)