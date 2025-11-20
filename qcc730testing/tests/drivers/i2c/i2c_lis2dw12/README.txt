I2C bus controller to LIS2DW12 sensor test
##########################################

This test verifies I2C target driver API methods with external sensors - ST
LIS2DW12.

Configuration Options
---------------------

Add "hal_st" to integrate the ST HAL drivers into the build. This is needed for
the LIS2DW12 sensor support.

in ${workdir}/qcc730/west.yml --> projects --> name-allowlist --> add "hal_st" to the list of allowed project names:
  projects:
    - name: zephyr
      remote: zephyrproject-rtos
      revision: kernel.qzephyr.1.0
      import:
        # By using name-allowlist we can clone only the modules that are
        # strictly needed by the application.
        name-allowlist:
          - zcbor      # for c++ programming
          - hal_st
