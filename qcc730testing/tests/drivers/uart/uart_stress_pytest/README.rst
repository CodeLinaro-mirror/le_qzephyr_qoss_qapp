QCC730 UART Large Transfer Stress Test
======================================

Test Overview
------------

This test suite validates the robustness and reliability of the QCC730 UART
driver implementation for large data transfers. The tests verify that the UART
subsystem can handle sustained high-volume communication with data integrity
verification through CRC checksums.

Hardware Requirements
--------------------

Host Communication Setup
~~~~~~~~~~~~~~~~~~~~~~~

* QCC730 board with UART console enabled
* Host PC connected via USB-to-serial or direct UART connection
* Python library `crcmod`. Please install it via specified `requirements.txt`
  file (example command: `pip install -r requirements.txt`).

Test Components
--------------

RX Large Transfer Test
~~~~~~~~~~~~~~~~~~~~~~

Validates reliable reception of large data volumes from host:

* Receives configurable data size (default 10 MB) from host
* Real-time CRC16 calculation during reception
* Progress reporting at configurable intervals
* Host-side CRC verification

**Test Parameters**:

* Configurable RX size via CONFIG_RX_TEST_SIZE_KB
* Configurable progress intervals via CONFIG_PROGRESS_INTERVAL_KB

**Success Criteria**: All bytes received without loss, CRC match with
host-calculated value.

TX Large Transfer Test
~~~~~~~~~~~~~~~~~~~~~

Validates reliable transmission of large data volumes to host:

* Sends configurable data size (default 10 MB) to host
* Pattern-based data generation (sequential bytes)
* CRC16 calculation and transmission for host verification

**Test Parameters**:

* Configurable TX size via CONFIG_TX_TEST_SIZE_KB
* START signal synchronization with host
* Host-side CRC verification with pass/fail feedback

**Success Criteria**: All bytes transmitted successfully, host confirms CRC
match.

Robustness Validation Criteria
------------------------------

Data Integrity
~~~~~~~~~~~~~

Both tests use CRC16 checksums to verify data integrity. Any of the following
constitutes a test failure:

* CRC mismatch between sender and receiver
* Incomplete data transfer
* UART device not ready at test start
* Timeout during host synchronization

Performance Metrics
~~~~~~~~~~~~~~~~~~

The tests measure and report:

* Total bytes transferred (RX/TX)
* Transfer duration in milliseconds
* Throughput in bytes/sec
* Effective bit rate in bps

These metrics validate that the UART driver maintains adequate performance under
sustained transfer loads.

Host Integration
~~~~~~~~~~~~~~~

The Python test harness provides automated verification:

* Generates predictable test data patterns
* Synchronizes with device through control messages
* Performs independent CRC calculations
* Provides pass/fail verdict to device

Test Results Interpretation
--------------------------

Success Criteria
~~~~~~~~~~~~~~~

A passing test indicates:

* Zero data loss during large transfers
* Correct CRC verification for all data
* Successful bidirectional handshake protocol
* Performance metrics within acceptable ranges

Practical Implications
---------------------

This comprehensive stress testing demonstrates that the QCC730 UART driver can
reliably handle:

* High-volume data transfers
* Data integrity preservation over large payloads
* Integration with external host systems
