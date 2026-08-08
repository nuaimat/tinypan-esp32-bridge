#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Start the UART console REPL task.
 *
 * Initializes UART0 for RX/TX and spawns a FreeRTOS task that reads
 * commands from the serial port and dispatches them to the bridge's
 * subsystems for debugging.
 *
 * @return ESP_OK on success.
 */
esp_err_t console_repl_start(void);

/**
 * @brief Get the runtime BNEP keepalive interval override (ms).
 * @return 0 if not overridden (use compile-time default).
 */
uint32_t console_get_runtime_keepalive_ms(void);

/**
 * @brief Get the runtime BT QoS poll interval override.
 * @return 0 if not overridden (use compile-time default).
 */
uint8_t console_get_runtime_qos_poll(void);

#ifdef __cplusplus
}
#endif
