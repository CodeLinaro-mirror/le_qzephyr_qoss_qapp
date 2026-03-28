"""
Copyright (c) 2025 Qualcomm Technologies, Inc.
SPDX-License-Identifier: BSD-3-Clause
"""

import pytest
from custom_hardware_adapter import CustomHardwareAdapter
from twister_harness.twister_harness_config import DeviceConfig
from pathlib import Path


def pytest_addoption(parser):
    """Add local pytest options for uart_stress_pytest."""
    parser.addoption(
        "--skip-flash",
        action="store_true",
        default=False,
        help="Skip west flash in fixture setup and use pre-flashed image.",
    )


@pytest.fixture(scope="session")
def dut(request):
    """Override the default dut fixture to use custom adapter"""

    serial_port = request.config.getoption('--device-serial', default='COM9')
    baud_rate = request.config.getoption('--device-serial-baud', default=115200)
    build_dir = request.config.getoption('--build-dir', default='.')
    skip_flash = request.config.getoption('--skip-flash', default=False)

    if isinstance(baud_rate, str):
        baud_rate = int(baud_rate)

    device_config = DeviceConfig(
        type='hardware',
        serial=serial_port,
        baud=baud_rate,
        build_dir=Path(build_dir),
        base_timeout=60.0,
        flash_timeout=60.0
    )

    adapter = CustomHardwareAdapter(device_config)
    if skip_flash:
        adapter.launch_without_flash()
    else:
        adapter.launch()
    yield adapter
    adapter.close()
