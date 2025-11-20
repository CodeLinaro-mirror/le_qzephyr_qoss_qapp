QCC730 DXE DMA Test Suite
=========================

Test Overview
-------------

This test suite validates the QCC730 DXE DMA controller integration in Zephyr.
It verifies correct functionality of memory-to-memory transfers across SRAM and
RRAM, multi-block transfers, multi-channel operation, priorities, and
error handling.

Hardware Requirements
---------------------

- QCC730 board with DXE (DMA) controller enabled

Test Cases
---------------

DXE memory to memory transfers
~~~~~~~~~~~~~~~~~~~~~~

Validates memory copy between memory buffers. The test runs 3 different transfers
for each of the following directions: SRAM2SRAM, SRAM2RRAM, RRAM2RRAM, RRAM2SRAM.
Transfer differs by block sizes, single and multi-block transfers, different channels
and priorities.

**Success Criteria**: Destination buffers match source buffers.

DMA stop during transfer
~~~~~~~~~~~~~~~~~~~~~~

Validates if a DXE transfer can be stopped.

**Success Criteria**: Destination buffer does not match source buffer.

Boundary transfer size
~~~~~~~~~~~~~~~~~~~~~~

Validates if a DXE transfer works for the maximum allowed transfer size of 0x3FFC bytes.

**Success Criteria**: Destination matches source buffer.

Error handling
~~~~~~~~~~~~~~~~~~~~

Validates that driver returns expected errors when invalid configurations are used:

- Zero-length block
- Oversized block (>0x3FFC bytes)
- Misaligned blocks' addresses
- Unsupported channel direction
- Null block pointer

**Success Criteria**: Each invalid configuration returns ``-EINVAL``.
