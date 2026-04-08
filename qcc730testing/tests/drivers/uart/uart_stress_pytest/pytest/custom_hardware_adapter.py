"""
Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
SPDX-License-Identifier: BSD-3-Clause-Clear
"""

import time
import serial
from twister_harness.device.hardware_adapter import HardwareAdapter
import logging
import threading

logger = logging.getLogger(__name__)


class CustomHardwareAdapter(HardwareAdapter):
    """Custom hardware adapter that handles garbage data for QCC730 better"""

    def launch_without_flash(self) -> None:
        """Launch adapter session without invoking west flash."""
        self.close()
        self._clear_internal_resources()

        if not self.command:
            self.generate_command()
            if self.device_config.extra_test_args:
                self.command.extend(self.device_config.extra_test_args.split())

        self._device_run.set()
        self._start_reader_thread()
        self.connect(retry_s=10)

    def enter_raw_mode(self) -> None:
        """Stop background output handler to allow direct serial access"""
        logger.info("Entering raw mode - stopping output handler")
        self._device_run.clear()
        time.sleep(0.3)

        while not self._device_read_queue.empty():
            try:
                self._device_read_queue.get_nowait()
            except BaseException:
                break

    def exit_raw_mode(self) -> None:
        """Resume background output handler"""
        logger.info("Exiting raw mode - restarting output handler")
        self._device_run.set()
        self._connection_thread = threading.Thread(
            target=self._handle_device_output,
            daemon=True
        )
        self._connection_thread.start()

    def _read_device_output(self) -> bytes:
        """Override to handle reading more robustly"""
        if not self._serial_connection or not self._serial_connection.is_open:
            return b''

        try:
            if self._serial_connection.in_waiting > 0:
                data = self._serial_connection.read(self._serial_connection.in_waiting)
                return data
            else:
                self._serial_connection.timeout = 0.01
                data = self._serial_connection.read(1)
                if data:
                    if self._serial_connection.in_waiting > 0:
                        data += self._serial_connection.read(self._serial_connection.in_waiting)
                return data
        except (OSError, serial.SerialException) as e:
            return b''
        except Exception as e:
            logger.error(f"Error reading serial: {e}")
            return b''

    def _handle_device_output(self) -> None:
        """Override to better handle garbage and incomplete lines for QCC730"""
        buffer = b''

        with open(self.handler_log_path, 'a+', encoding='utf-8', errors='replace') as log_file:
            while self.is_device_running():
                if self.is_device_connected():
                    try:
                        raw_data = self._read_device_output()
                        if raw_data:
                            buffer += raw_data

                            while b'\n' in buffer or b'\r' in buffer:
                                idx_n = buffer.find(b'\n')
                                idx_r = buffer.find(b'\r')

                                if idx_n == -1:
                                    idx = idx_r
                                elif idx_r == -1:
                                    idx = idx_n
                                else:
                                    idx = min(idx_n, idx_r)

                                if idx >= 0:
                                    line = buffer[:idx]
                                    buffer = buffer[idx + 1:]
                                    if buffer.startswith(b'\n') or buffer.startswith(b'\r'):
                                        buffer = buffer[1:]

                                    try:
                                        decoded = line.decode('utf-8', errors='replace')
                                        decoded = decoded.replace('\x66', '').strip('\x00\xff')
                                        decoded = decoded.strip()

                                        if decoded and len(decoded) < 1000:
                                            self._device_read_queue.put(decoded)
                                            log_file.write(f'{decoded}\n')
                                            log_file.flush()
                                    except Exception as e:
                                        logger.error(f"Error decoding line: {e}")

                            if len(buffer) > 1024:
                                try:
                                    decoded = buffer[:1024].decode('utf-8', errors='replace')
                                    decoded = decoded.strip('\x00\x66\xff')
                                    if decoded:
                                        self._device_read_queue.put(decoded)
                                        log_file.write(f'{decoded}\n')
                                        log_file.flush()
                                except BaseException:
                                    pass
                                buffer = buffer[1024:]
                    except (OSError, serial.SerialException):
                        # Port was closed, exit gracefully
                        break
                    except Exception as e:
                        logger.error(f"Error in output handler: {e}")
                        time.sleep(0.1)
                else:
                    self._flush_device_output()
                    time.sleep(0.1)
