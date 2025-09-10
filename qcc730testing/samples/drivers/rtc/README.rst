.. zephyr:code-sample:: rtc
   :name: Real-Time Clock (RTC)
   :relevant-api: rtc_interface

   Set and read the date/time from a Real-Time Clock. Test rtc alarm.

Overview
********

This sample shows how to use the :ref:`rtc driver API <rtc_api>`
to set and read the date/time from RTC and display on the console
and can be built and executed on boards supporting RTC.
Additionally patch provided extends original zephyr sample with
testing alarm functions

Building and Running
********************

Build original zephyr sample with the command:
.. code-block:: console
	west build -p -b qcc730mi zephyr/samples/drivers/rtc/ -- \
		-DDTC_OVERLAY_FILE=../../../qcc730zephyrporting/samples/drivers/rtc/boards/qcc730mi.overlay \
		-DEXTRA_CONF_FILE=../../../../qcc730zephyrporting/samples/drivers/rtc/boards/qcc730mi.conf

If you want to test extended version. First apply the patch provided to your local zephyr repo:
.. code-block:: console
	cd <your workspace path>/zephyr
	git am ../qcc730zephyrporting/samples/drivers/rtc/0001-samples-Extend-drivers-rtc-sample-to-test-alarm-func.patch

And now proceed with the build.

Sample Output
=============

Expected output for extended version:
.. code-block:: console
	*** Booting Zephyr OS build v4.1.0-1-g5afa88ce2cf8 ***
	[00:00:00.000,000] <dbg> qtmr_qcc730: qtmr_qcc730_set_alarm_absolute: Setting alarm for timer frame 0, ticks 2309153131
	Correct alarm time obtained
	RTC date and time: 2024-11-17 04:19:00
	RTC date and time: 2024-11-17 04:19:02
	RTC date and time: 2024-11-17 04:19:04
	...
	RTC date and time: 2024-11-17 04:19:54
	RTC date and time: 2024-11-17 04:19:56
	RTC date and time: 2024-11-17 04:19:58
	[00:01:00.000,000] <dbg> qtmr_qcc730: qtmr_qcc730_isr: Qtimer frame 0 ISR triggered
	[00:01:00.000,000] <dbg> rtc_qcc730: rtc_qcc730_counter_alarm_callback: RTC received the Qtimer alarm callback.
	RTC callback triggered! 
	RTC date and time: 2024-11-17 04:20:00
	RTC date and time: 2024-11-17 04:20:02
