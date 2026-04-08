/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 *
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#ifndef QCC730_ATCMD_DEMO_H
#define QCC730_ATCMD_DEMO_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize AT command demo
 *
 * This function initializes the AT command demo by:
 * - Setting up response and event parsers
 * - Allocating test parameter structures
 * - Registering the AT command receive callback with ring service
 */
void atcmd_demo_init(void);

/**
 * @brief Register AT command shell commands
 *
 * This function registers all AT command test shell commands:
 * - help: Display AT command help
 * - parser: Enable/disable response parser
 * - qatperf: Performance test
 * - tx: TX test
 * - rx_mqtt: RX MQTT test
 * - tx_loop: TX stress test
 * - httptest: HTTP test
 */
void atcmd_demo_register_commands(void);

/**
 * @brief Get SPI state
 *
 * @return Current SPI state (0=NOT_READY, 1=READY)
 */
int qapi_atcmd_get_spi_state(void);

/**
 * @brief Set SPI state
 *
 * @param state New SPI state (0=NOT_READY, 1=READY)
 * @return 0 on success, negative error code on failure
 */
int qapi_atcmd_set_spi_state(int state);

#ifdef __cplusplus
}
#endif

#endif /* QCC730_ATCMD_DEMO_H */
