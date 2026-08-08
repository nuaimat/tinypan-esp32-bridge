/**
 * @file led_indicator.c
 * @brief FreeRTOS task that drives a status LED on GPIO 2.
 *
 * The task loops forever, reading the current pattern (set by
 * led_indicator_set_pattern()) and toggling the GPIO accordingly.
 * Pattern changes are picked up between blink phases — worst-case
 * latency is one full on/off cycle (1 s maximum).
 */

#include "led_indicator.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "led";

/* ── GPIO pin ─────────────────────────────────────────────────── */

#define LED_GPIO  GPIO_NUM_2

/* ── Shared state ──────────────────────────────────────────────── */

static volatile led_pattern_t s_pattern = LED_PATTERN_BOOTING;

/* ── Task ──────────────────────────────────────────────────────── */

static void led_task(void *arg)
{
    (void)arg;

    gpio_reset_pin(LED_GPIO);
    gpio_set_direction(LED_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(LED_GPIO, 0);

    while (1) {

        switch (s_pattern) {

        /* ── Booting: 200 ms on / 200 ms off ──────────────── */
        case LED_PATTERN_BOOTING:
            gpio_set_level(LED_GPIO, 1);
            vTaskDelay(pdMS_TO_TICKS(200));
            if (s_pattern != LED_PATTERN_BOOTING) break;
            gpio_set_level(LED_GPIO, 0);
            vTaskDelay(pdMS_TO_TICKS(200));
            break;

        /* ── AP ready: 1 s on / 1 s off ───────────────────── */
        case LED_PATTERN_AP_READY:
            gpio_set_level(LED_GPIO, 1);
            vTaskDelay(pdMS_TO_TICKS(1000));
            if (s_pattern != LED_PATTERN_AP_READY) break;
            gpio_set_level(LED_GPIO, 0);
            vTaskDelay(pdMS_TO_TICKS(1000));
            break;

        /* ── BT connecting: 500 ms on / 500 ms off ────────── */
        case LED_PATTERN_BT_CONNECTING:
            gpio_set_level(LED_GPIO, 1);
            vTaskDelay(pdMS_TO_TICKS(500));
            if (s_pattern != LED_PATTERN_BT_CONNECTING) break;
            gpio_set_level(LED_GPIO, 0);
            vTaskDelay(pdMS_TO_TICKS(500));
            break;

        /* ── Connected: solid on ──────────────────────────── */
        case LED_PATTERN_CONNECTED:
            gpio_set_level(LED_GPIO, 1);
            /* Sleep 1 s just to yield; check for pattern change */
            vTaskDelay(pdMS_TO_TICKS(1000));
            break;

        /* ── Error: 3 × 200 ms blinks, 1 s pause ──────────── */
        case LED_PATTERN_ERROR:
            for (int i = 0; i < 3; i++) {
                if (s_pattern != LED_PATTERN_ERROR) break;
                gpio_set_level(LED_GPIO, 1);
                vTaskDelay(pdMS_TO_TICKS(200));
                if (s_pattern != LED_PATTERN_ERROR) break;
                gpio_set_level(LED_GPIO, 0);
                vTaskDelay(pdMS_TO_TICKS(200));
            }
            if (s_pattern != LED_PATTERN_ERROR) break;
            /* Pause between bursts */
            vTaskDelay(pdMS_TO_TICKS(1000));
            break;

        /* ── Off / default ────────────────────────────────── */
        case LED_PATTERN_OFF:
        default:
            gpio_set_level(LED_GPIO, 0);
            vTaskDelay(pdMS_TO_TICKS(1000));
            break;
        }
    }
}

/* ==================================================================
 *  Public API
 * ================================================================== */

esp_err_t led_indicator_init(void)
{
    BaseType_t ret = xTaskCreate(led_task,
                                 "led_indicator",
                                 2048,
                                 NULL,
                                 tskIDLE_PRIORITY + 2,
                                 NULL);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create LED task");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "LED indicator task started on GPIO %d", LED_GPIO);
    return ESP_OK;
}

void led_indicator_set_pattern(led_pattern_t pattern)
{
    s_pattern = pattern;
}
