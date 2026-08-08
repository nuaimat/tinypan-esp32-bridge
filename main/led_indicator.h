#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** LED blink patterns for system status indication */
typedef enum {
    LED_PATTERN_BOOTING,       /**< Fast blink   — 200 ms on/off            */
    LED_PATTERN_AP_READY,      /**< Slow blink   — 1 s on/off               */
    LED_PATTERN_BT_CONNECTING, /**< Medium blink — 500 ms on/off            */
    LED_PATTERN_CONNECTED,     /**< Solid on     — BT + NAT operational     */
    LED_PATTERN_ERROR,         /**< 3 fast blinks → 1 s pause → repeat      */
    LED_PATTERN_OFF,           /**< LED turned off                          */
} led_pattern_t;

/**
 * @brief  Create the FreeRTOS LED indicator task on GPIO 2.
 *
 * The task starts with the BOOTING pattern and reacts to
 * led_indicator_set_pattern() calls made from anywhere in the system.
 *
 * Call once during early initialisation.
 *
 * @return ESP_OK on success.
 */
esp_err_t led_indicator_init(void);

/**
 * @brief  Switch the LED to a new pattern.
 *
 * Thread-safe — may be called from any task or ISR.
 * The change takes effect after the current blink phase completes
 * (at most 1 s of latency).
 *
 * @param pattern  Desired LED pattern.
 */
void led_indicator_set_pattern(led_pattern_t pattern);

#ifdef __cplusplus
}
#endif
