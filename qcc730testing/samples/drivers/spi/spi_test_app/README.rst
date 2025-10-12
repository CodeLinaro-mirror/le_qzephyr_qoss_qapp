.. zephyr:code-sample:: spi-test-app
   :name: SPI test app
   :relevant-api: spi_interface

   Use SPI in slave role together with UART console for verification.

Overview
********

This app goal is to verify that SPI slave function is working properly.
We need to have UART shell for verification if commands sent from the
SPI master was executed properly.
