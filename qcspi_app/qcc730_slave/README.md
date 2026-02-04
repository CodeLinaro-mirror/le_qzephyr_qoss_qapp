# QCC730 Slave Shell Commands

Command-line interface for QCC730 slave device Ring Service.

## Commands

### ring status
Display status of all ring buffers.

```bash
ring status
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
```

---

### ring send
Send string data to host.

```bash
ring send <ring_id> <string>
```

**Parameters:**
- `ring_id`: Ring ID (0-2)
- `string`: Text string (multiple words supported)

**Examples:**
```bash
ring send 0 Hello World
```

---

### ring send_hex
Send hex data to host.

```bash
ring send_hex <ring_id> <hex_string>
```

**Parameters:**
- `ring_id`: Ring ID (0-2)
- `hex_string`: Hex string (e.g., AABBCCDD or 0x1122334455)

**Examples:**
```bash
ring send_hex 0 AABBCCDD
ring send_hex 1 0x0102030405060708
```

---

### ring send_pattern
Send test pattern to host.

```bash
ring send_pattern <ring_id> <length>
```

**Parameters:**
- `ring_id`: Ring ID (0-2)
- `length`: Data length in bytes (1-1500)

**Examples:**
```bash
ring send_pattern 0 100
ring send_pattern 2 1500
```

**Note:** Sends incrementing byte pattern (0x00, 0x01, 0x02, ..., 0xFF, 0x00, ...)

---

### ring send_burst
Send burst packets to host (high priority thread).

**Mode 1: Count-based**
```bash
ring send_burst <ring_id> <count> <size>
```

**Mode 2: Time-based (stress test)**
```bash
ring send_burst <ring_id> 0 <duration_ms>
```

**Parameters:**
- `ring_id`: Ring ID (0-2)
- `count`: Packet count (1-100000), set to 0 for time-based mode
- `size`: Bytes per packet (1-1500)
- `duration_ms`: Duration in milliseconds (1-3600000, max 1 hour)

**Examples:**
```bash
# Send 100 packets of 1500 bytes
ring send_burst 0 100 1500

# Continuous send for 60 seconds (stress test)
ring send_burst 1 0 60000
```

**Output:**
```
=== Burst Test Results ===
Success: 1000 packets
Failed: 0 packets
Total Packets: 1000
Time: 2500 ms
Throughput: 4800000 bits/sec (4.80 Mbps)
Packet Rate: 400 packets/sec
```

---

## Ring IDs

- **Ring 0 (RING_CONFIG)**: Configuration and control messages
- **Ring 1 (RING_DATA)**: Data transfer
- **Ring 2 (RING_LOOPBACK)**: Loopback test (auto echo received data)

---

## Usage Examples

### Basic Testing
```bash
# Check status
ring status

# Send test message
ring send 2 Hello from QCC730

# Send hex data
ring send_hex 2 AABBCCDD

# Send test pattern
ring send_pattern 2 100
```

### Performance Testing
```bash
# Count-based test
ring send_burst 2 1000 1500

# Time-based stress test (60 seconds)
ring send_burst 2 0 60000
```

---

## Build Configuration

Required configuration options:
- `CONFIG_RING_SERVICE`: Enable Ring Service
- Zephyr RTOS environment
