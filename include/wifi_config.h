/* WiFi AP configuration globals for TinyPAN v2.
 * Adapted from martin-ger/esp32_nat_router.
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── IP configuration ─────────────────────────────────────────── */
extern uint32_t my_ap_ip;    /* AP/LAN side IP (set at wifi_ap_init) */

#ifdef __cplusplus
}
#endif
