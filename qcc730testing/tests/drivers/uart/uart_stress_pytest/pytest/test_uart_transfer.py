#!/usr/bin/env python3

"""
Copyright (c) 2025 Qualcomm Technologies, Inc.
SPDX-License-Identifier: BSD-3-Clause

UART RX/TX stress tests using pytest and twister_harness
Tests both directions without requiring loopback
"""

import pytest
import logging
import time
import crcmod
import os
from twister_harness import DeviceAdapter
from custom_hardware_adapter import CustomHardwareAdapter

POLYNOMINAL = 0x11021
crc16_ccitt = crcmod.mkCrcFun(POLYNOMINAL, initCrc=0, xorOut=0, rev=False)
logger = logging.getLogger(__name__)


@pytest.fixture(scope="session")
def zephyr_config(dut: DeviceAdapter) -> dict:
    """
    Load test configuration from .config using direct parsing
    """
    build_dir = dut.device_config.build_dir
    config_file = os.path.join(build_dir, 'zephyr', '.config')

    if not os.path.exists(config_file):
        raise FileNotFoundError(f"Config file not found at {config_file}")

    config_dict = {}
    with open(config_file, 'r') as f:
        for line in f:
            line = line.strip()
            if line.startswith('CONFIG_') and '=' in line:
                key, val = line.split('=', 1)
                config_dict[key] = val.strip('"')

    config = {
        'RX_TEST_SIZE': int(config_dict.get('CONFIG_RX_TEST_SIZE_KB', '1')) * 1024,
        'TX_TEST_SIZE': int(config_dict.get('CONFIG_TX_TEST_SIZE_KB', '1')) * 1024,
        'CHUNK_SIZE_KB': int(config_dict.get('CONFIG_CHUNK_SIZE_KB', '1')) * 1024,
        'PROGRESS_INTERVAL_KB': int(config_dict.get('CONFIG_PROGRESS_INTERVAL_KB', '1')) * 1024
    }

    logger.info("--- Test Configuration Loaded ---")
    logger.info(f"RX_TEST_SIZE: {config['RX_TEST_SIZE']} bytes")
    logger.info(f"TX_TEST_SIZE: {config['TX_TEST_SIZE']} bytes")
    logger.info("---------------------------------")

    return config


def generate_test_data(size: int) -> bytes:
    """Generate predictable test data for transfer."""
    pattern = bytes(range(256)) * (size // 256 + 1)
    return pattern[:size]


def test_1_uart_rx_stress(dut: CustomHardwareAdapter, zephyr_config: dict):
    logger.info("=== UART RX Stress Test ===")

    RX_TEST_SIZE = zephyr_config['RX_TEST_SIZE']
    CHUNK_SIZE_KB = zephyr_config['CHUNK_SIZE_KB']
    PROGRESS_INTERVAL_KB = zephyr_config['PROGRESS_INTERVAL_KB']

    # Wait for test result
    logger.info("Waiting for DUT RX_TEST_READY...")
    lines = dut.readlines_until(
        regex=r".*RX_TEST_READY.*",
        timeout=15.0,
        print_output=True
    )

    found_ready = any("RX_TEST_READY" in line for line in lines)
    assert found_ready, f"RX_TEST_READY not found. Got: {lines}"

    # Generate test data
    logger.info(f"Generating {RX_TEST_SIZE} test data...")
    test_data = generate_test_data(RX_TEST_SIZE)
    crc16 = crc16_ccitt(test_data)
    logger.info(f"Test data CRC16: 0x{crc16:02X}")

    # Send data to device
    logger.info(f"Sending {RX_TEST_SIZE} to device...")
    start_time = time.time()
    bytes_sent = 0

    while bytes_sent < RX_TEST_SIZE:
        # Send next chunk
        chunk_end = min(bytes_sent + CHUNK_SIZE_KB, RX_TEST_SIZE)
        chunk = test_data[bytes_sent:chunk_end]
        dut.write(chunk)
        bytes_sent = chunk_end

        time.sleep(0.01)

        # Progress reporting
        if bytes_sent % PROGRESS_INTERVAL_KB == 0:
            percent = (bytes_sent * 100) // RX_TEST_SIZE
            logger.info(f"TX Progress: {percent}% ({bytes_sent} bytes)")

    time.sleep(0.5)

    logger.info("Waiting for DUT RX_CRC_READY...")
    lines = dut.readlines_until(
        regex=r".*RX_CRC_READY.*",
        timeout=60.0
    )

    logger.info(f"Sending CRC16 to device: 0x{crc16:02X}")
    crc_bytes = crc16.to_bytes(2, byteorder='big')
    dut.write(crc_bytes)

    logger.info("Waiting for RX test result...")
    lines = dut.readlines_until(
        regex=r".*RX_TEST_.*",
        timeout=15.0
    )

    success = False
    for line in lines:
        if "RX_TEST_PASS" in line:
            success = True
            logger.info("RX test PASSED - device received all data correctly")
            break
        elif "RX_TEST_FAIL" in line:
            logger.error("RX test FAILED - CRC mismatch")
            break

    # Calculate statistics
    duration = time.time() - start_time
    throughput = RX_TEST_SIZE / duration
    logger.info(f"RX statistics: {RX_TEST_SIZE} bytes in {duration:.2f}s = "
                f"{throughput:.0f} bytes/sec ({throughput * 8 / 1000000:.2f} Mbps)")

    assert success, "UART RX test failed - device reported CRC mismatch"


def test_2_uart_tx_stress(dut, zephyr_config: dict):
    logger.info("=== UART TX Stress Test ===")

    TX_TEST_SIZE = zephyr_config['TX_TEST_SIZE']
    PROGRESS_INTERVAL_KB = zephyr_config['PROGRESS_INTERVAL_KB']

    lines = dut.readlines_until(
        regex=r".*TX_TEST_READY.*",
        timeout=15.0,
        print_output=True
    )

    found_ready = any("TX_TEST_READY" in line for line in lines)
    assert found_ready, f"TX_TEST_READY not found. Got: {lines}"

    logger.info("Entering raw mode before sending START...")
    dut.enter_raw_mode()
    time.sleep(0.1)

    logger.info("Sending START signal...")
    serial_conn = dut._serial_connection
    serial_conn.write(b"START")
    serial_conn.flush()
    time.sleep(0.1)

    logger.info(f"Receiving {TX_TEST_SIZE} bytes from device...")
    received_data = bytearray()
    start_time = time.time()
    # Derive a realistic timeout from payload size and UART baud.
    # UART transfers 1 byte with ~10 bits on the wire (8N1 framing).
    baud = getattr(serial_conn, "baudrate", 115200) or 115200
    expected_seconds = TX_TEST_SIZE / (float(baud) / 10.0)
    rx_timeout_s = max(30.0, expected_seconds * 3.0 + 5.0)
    rx_deadline = start_time + rx_timeout_s
    logger.info(f"TX receive deadline: {rx_timeout_s:.1f}s (expected ~{expected_seconds:.1f}s)")
    while len(received_data) < TX_TEST_SIZE:
        if serial_conn.in_waiting > 0:
            chunk = serial_conn.read(min(4096, TX_TEST_SIZE - len(received_data)))
            if chunk:
                received_data.extend(chunk)

                if len(received_data) % PROGRESS_INTERVAL_KB == 0:
                    percent = (len(received_data) * 100) // TX_TEST_SIZE
                    logger.info(f"RX Progress: {percent}% ({len(received_data)} bytes)")
        else:
            time.sleep(0.001)
            if time.time() > rx_deadline:
                logger.error(f"Timeout waiting for data. Received {len(received_data)}/{TX_TEST_SIZE} bytes")
                break

    # Receive CRC16
    crc_bytes = bytearray()
    timeout_count = 0
    while len(crc_bytes) < 2:
        if serial_conn.in_waiting > 0:
            crc_bytes.extend(serial_conn.read(2 - len(crc_bytes)))
            timeout_count = 0
        else:
            time.sleep(0.001)
            timeout_count += 1
            if timeout_count > 1000:
                logger.error("Timeout waiting for CRC bytes")
                break

    if len(crc_bytes) < 2:
        logger.error("Failed to receive complete CRC")
        dut.exit_raw_mode()
        assert False, "Failed to receive CRC from device"

    crc16_received = int.from_bytes(crc_bytes, byteorder='big')
    crc16_calculated = crc16_ccitt(bytes(received_data))
    logger.info(f"Received CRC16: 0x{crc16_received:02X}")
    logger.info(f"Calculated CRC16: 0x{crc16_calculated:02X}")

    # Send verification result
    if crc16_calculated == crc16_received:
        serial_conn.write(b"PASS")
        serial_conn.flush()
        logger.info("TX test PASSED - CRC verification successful")
        success = True
    else:
        serial_conn.write(b"FAIL")
        serial_conn.flush()
        logger.error(f"TX test FAILED - CRC mismatch")
        success = False

    # Exit raw mode before waiting for device response
    dut.exit_raw_mode()
    lines = dut.readlines_until(
        regex=r".*TX_TEST_.*",
        timeout=15.0
    )

    duration = time.time() - start_time
    throughput = TX_TEST_SIZE / duration if duration > 0 else 0
    logger.info(f"TX statistics: {TX_TEST_SIZE} bytes in {duration:.2f}s = "
                f"{throughput:.0f} bytes/sec ({throughput * 8 / 1000000:.2f} Mbps)")

    assert success, "UART TX test failed - CRC mismatch"
