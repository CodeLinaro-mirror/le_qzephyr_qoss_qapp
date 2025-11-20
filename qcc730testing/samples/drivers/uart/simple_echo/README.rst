.. zephyr:code-sample:: simple-echo
   :name: Simple Echo
   :relevant-api: uart_interface

   Use UART with interrupts to create a simple loopback on the console.

Overview
********

This sample demonstrates the usage of the UART driver in order to obtain
software loopback. When user writes to the serial data is being collected 
and after receiving new line character, code sends back the message
with "Loopback: " prefix.
