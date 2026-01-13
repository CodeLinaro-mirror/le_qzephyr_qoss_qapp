# QCC730 Demo Shell Commands

This document describes the command-line interface for the QCC730 Host Demo application running on FreeRTOS.

## Overview

The QCC730 Demo provides a shell interface for interacting with the QCC730 device via QCSPI transport and Ring Service. The shell supports various commands for memory access, register operations, and data transfer testing.

## Prerequisites

- QCC730 device connected via SPI interface
- FreeRTOS environment with shell support enabled
- QCSPI transport initialized

## Command Categories

### 1. QCSPI Transport Commands

#### `qcspi_init`
Initialize QCSPI transport and display QCC730 slave ID.

**Usage:**
```bash
qcspi_init
```

**Example Output:**
```
QCSPI transport initialized successfully
QCC730 Slave ID: 12 34 56
```

---

#### `qcspi_read`
Read memory from QCC730 device.

**Usage:**
```bash
qcspi_read <address> <length>
```

**Parameters:**
- `address`: Memory address (hex format, e.g., 0x8fc00)
- `length`: Number of bytes to read (1-1500)

**Valid Range:** 0x0 - 0x9FFFF

**Example:**
```bash
qcspi_read 0x8fc00 16
```

**Output:**
```
Read 16 bytes from 0x0008FC00:
0008FC00: 01 02 03 04 05 06 07 08 09 0A 0B 0C 0D 0E 0F 10
```

---

#### `qcspi_write`
Write data to QCC730 memory.

**Usage:**
```bash
qcspi_write <address> <hex_data>
```

**Parameters:**
- `address`: Memory address (hex format)
- `hex_data`: Hex string to write (e.g., AABBCCDD or 0xAABBCCDD)

**Valid Range:** 0x10000 - 0x9FC00

**Example:**
```bash
qcspi_write 0x8fc00 AABBCCDD
qcspi_write 0x10000 0x12345678
```

**Output:**
```
Wrote 4 bytes to 0x0008FC00
```

---

#### `qcspi_reg_read`
Read QCC730 register value.

**Usage:**
```bash
qcspi_reg_read <reg_addr>
```

**Parameters:**
- `reg_addr`: Register address (0x00 - 0x60, must be 4-byte aligned)

**Example:**
```bash
qcspi_reg_read 0x0C
```

**Output:**
```
Register 0x0C = 0x12345678
```

---

#### `qcspi_reg_write`
Write value to QCC730 register.

**Usage:**
```bash
qcspi_reg_write <reg_addr> <value>
```

**Parameters:**
- `reg_addr`: Register address (0x00 - 0x60, must be 4-byte aligned)
- `value`: 32-bit value to write (hex format)

**Example:**
```bash
qcspi_reg_write 0x0C 0x12345678
```

**Output:**
```
Wrote 0x12345678 to register 0x0C
```

---

#### `qcspi_interrupt`
Trigger A2F (Application to Firmware) interrupt on QCC730.

**Usage:**
```bash
qcspi_interrupt
```

**Output:**
```
A2F interrupt triggered successfully
```

---

#### `qcspi_reset`
Reset QCSPI interface.

**Usage:**
```bash
qcspi_reset
```

**Output:**
```
QCSPI reset successfully
```

---

#### `qcspi_test_transfer`
Performance test for QCSPI write operations.

**Usage:**
```bash
qcspi_test_transfer <address> <size> <count>
```

**Parameters:**
- `address`: Target memory address (0x10000 - 0x9FC00)
- `size`: Bytes per write (1-2048)
- `count`: Number of write iterations (1-100000)

**Example:**
```bash
qcspi_test_transfer 0x8fc00 1452 1000
```

**Output:**
```
=== QCSPI Transport Write Performance Test ===
Target Address: 0x0008FC00
Write Size: 1452 bytes
Iterations: 1000
Starting test...
Progress: 1000/1000 writes completed

=== Test Results ===
Success: 1000 writes
Failed: 0 writes
Total Bytes Written: 1452000 bytes
Elapsed Time: 2500 ms

=== Performance Metrics ===
Throughput: 580800 bytes/sec (567 KB/sec)
Throughput: 4646400 bps (4537 Kbps, 4 Mbps)
Write Rate: 400 writes/sec
Average Time per Write: 2500 us
Test completed successfully
```

---

### 2. Ring Service Commands

#### `ring_status`
Display status of all configured ring buffers.

**Usage:**
```bash
ring_status
```

**Output:**
```
=== Ring Service Status ===

--- Ring 0 ---
TX Available: 8 descriptors
RX Available: 5 descriptors
TX Count: 1234
RX Count: 567
TX Errors: 0
RX Errors: 0

--- Ring 1 ---
TX Available: 8 descriptors
RX Available: 8 descriptors
TX Count: 89
RX Count: 45
TX Errors: 0
RX Errors: 0
```

---

#### `ring_send`
Send string data via ring service.

**Usage:**
```bash
ring_send <ring_id> <string>
```

**Parameters:**
- `ring_id`: Ring buffer ID (0-2)
- `string`: Text string to send (multiple words supported)

**Example:**
```bash
ring_send 0 Hello World
ring_send 2 Test message from host
```

---

#### `ring_send_hex`
Send hex data via ring service.

**Usage:**
```bash
ring_send_hex <ring_id> <hex_string>
```

**Parameters:**
- `ring_id`: Ring buffer ID (0-3)
- `hex_string`: Hex string to send (e.g., AABBCCDD)

**Example:**
```bash
ring_send_hex 0 AABBCCDD
ring_send_hex 2 0x0102030405060708
```

---

#### `ring_pattern`
Send test pattern via ring service.

**Usage:**
```bash
ring_pattern <ring_id> <length>
```

**Parameters:**
- `ring_id`: Ring buffer ID (0-3)
- `length`: Pattern length in bytes (1-1500)

**Example:**
```bash
ring_pattern 0 100
ring_pattern 2 1500
```

**Note:** Sends incrementing byte pattern (0x00, 0x01, 0x02, ..., 0xFF, 0x00, ...)

---

#### `ring_burst`
Send burst packets via ring service (runs in background thread).

**Usage:**

**Mode 1: Send by packet count**
```bash
ring_burst count <ring_id> <count> <size>
```

**Mode 2: Send by duration**
```bash
ring_burst time <ring_id> <seconds> <size>
```

**Mode 3: Send by data size**
```bash
ring_burst data <ring_id> <MB> <size>
```

**Stop burst test:**
```bash
ring_burst stop
```

**Parameters:**
- `ring_id`: Ring buffer ID (0-3)
- `count`: Number of packets (1-1000000)
- `seconds`: Duration in seconds (1-360000)
- `MB`: Total data in megabytes (1-10240)
- `size`: Packet size in bytes (1-1500)

**Examples:**
```bash
# Send 1000 packets of 1500 bytes each
ring_burst count 0 1000 1500

# Send packets for 60 seconds
ring_burst time 0 60 1500

# Send 100 MB of data
ring_burst data 0 100 1500

# Stop running burst test
ring_burst stop
```

**Output:**
```
Burst test started in background thread
[BURST_THREAD] Starting burst test on ring 0
  Packet size: 1500 bytes
  Mode: Packet count (1000 packets)
[BURST_THREAD] Progress: 100/1000 packets
[BURST_THREAD] Progress: 200/1000 packets
...
[BURST_THREAD] Progress: 1000/1000 packets

=== Burst Test Results ===
Total Packets: 1000 (Success: 1000, Failed: 0)
Elapsed Time: 2500 ms
Total Data: 1464 KB (1 MB)
Throughput: 4687 Kbps (4 Mbps)
Packet Rate: 400 pps
Average Latency: 2500 us/packet
[BURST_THREAD] Burst test completed
```

---

## Ring Buffer IDs

The system supports multiple ring buffers for different purposes:

- **Ring 0 (RING_CONFIG)**: Configuration and control messages
- **Ring 1 (RING_DATA)**: Data transfer
- **Ring 2 (RING_LOOPBACK)**: Loopback testing

---

## Error Codes

Common error codes returned by commands:

- `-EINVAL` (-22): Invalid parameter
- `-ENODEV` (-19): Device not ready
- `-E2BIG` (-7): Data too large
- `-EBUSY` (-16): Resource busy
- `-EIO` (-5): I/O error

---

## Usage Examples

### Basic Initialization and Testing

```bash
# 1. Initialize QCSPI transport
qcspi_init

# 2. Check ring service status
ring_status

# 3. Send test message
ring_send 0 "Hello QCC730"

# 4. Send hex data
ring_send_hex 0 0xAABBCCDD

# 5. Read memory
qcspi_read 0x8fc00 4
```

### Performance Testing

```bash
# Test QCSPI write performance
qcspi_test_transfer 0x8fc00 1500 1000

# Test ring service throughput (count mode)
ring_burst count 0 10000 1500

# Test ring service throughput (time mode)
ring_burst time 0 60 1500

# Test ring service throughput (data mode)
ring_burst data 0 100 1500
```

### Register Operations

```bash
# Read register
qcspi_reg_read 0x0C

# Write register
qcspi_reg_write 0x0C 0x0A050000

# Trigger interrupt
qcspi_interrupt
```

### Loopback Testing

```bash
# Send data on loopback ring (Ring 2)
ring_send 2 Test loopback message
ring_send_hex 2 0102030405060708
ring_pattern 2 100

# Data will be echoed back and displayed automatically
```

---

## Troubleshooting

### Device Not Ready
```
ERROR: QCSPI transport device not ready
```
**Solution:** Run `qcspi_init` first to initialize the transport.

### Invalid Address Range
```
ERROR: Address 0xXXXXXXXX out of range
```
**Solution:** Ensure address is within valid range (0x10000 - 0x9FC00 for memory, 0x00 - 0x60 for registers).

### Ring Send Failed
```
ERROR: Ring send failed: -16
```
**Solution:** Check ring status with `ring_status` to verify available descriptors. The ring may be full.

### Burst Test Already Running
```
ERROR: Burst test already running
```
**Solution:** Stop the current burst test with `ring_burst stop` before starting a new one.

---

## Build Configuration

This demo requires the following build flags:

- `QC_OS_FREERTOS`: Enable FreeRTOS support
- `SPI_DEMO_ENABLE`: Enable SPI demo features
- `SHELL_FEATURE`: Enable shell command interface
- `CONFIG_RING_SERVICE`: Enable ring service commands

