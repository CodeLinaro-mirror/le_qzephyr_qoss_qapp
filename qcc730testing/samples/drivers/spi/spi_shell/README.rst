.. zephyr:code-sample:: spi_shell
   :name: SPI Master Shell for STM32 MCUs
   :relevant-api: spi_interface

Overview
********

This sample is for STM32 devices which could be be used to communicate
with QCC730 as a SPI peripheral device. The sample opens a uart shell 
with three simple commands which are listed with the command `spi help`.

QCC730 slave SPI testing
********

In order to test slave SPI role of QCC730 one need to invoke following
commands in host shell:

.. code-block:: console
	uart:~$ spi config_freq 1000000

Setting SPI frequency to 1MHz.

.. code-block:: console
	uart:~$ spi send_hex 810C000000000000
81 - IRR (internal register read) command
0C - Offset of QCSPI_SLAVE_R_SPI_SLAVE_CONFIG register
0000 - two dummy bytes needed by the slave
00000000 - four dummy bytes needed to read data on MISO
Reading Slave config register in order to know its current value.
This value can be for example: 0x0A050800
Then, we should write to this register value that contains some
bit in HOST_INT field.

.. code-block:: console
	uart:~$ spi send_hex 820C0A050820
82 - IRW (internal register write) command
0C - Offset of QCSPI_SLAVE_R_SPI_SLAVE_CONFIG register
0A0508 - Previous value of most significant 3 bytes of QCSPI_SLAVE_R_SPI_SLAVE_CONFIG register
20 - Trigger interrupt 0

After sending last command one should be able to see information in QCC730 shell that SPI
interrupt has occurred.

Other option of checking SPI slave functionality would be to use WRITE host command:

.. code-block:: console
	uart:~$ spi send_hex 020001011CDEADBEEF
02 - WRITE command
0001011C - (example) address to write data to
DEADBEEF - (example) dummy data to write

One can verify on QCC730 if "DEADBEEF" data is prent at address 0001011C.
