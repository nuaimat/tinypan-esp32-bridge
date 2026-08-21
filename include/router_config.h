/* Core router configuration for TinyPAN v2.
 * Adapted from martin-ger/esp32_nat_router.
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PARAM_NAMESPACE "tinypan"
#define DEFAULT_HOSTNAME "tinypan-bridge"
#define PROTO_TCP 6
#define PROTO_UDP 17

/* ── WiFi AP ──────────────────────────────────────────────────── */
#define AP_MAX_CONNECTIONS  8
#define AP_SSID_MAX_LEN     32
#define AP_PASS_MAX_LEN     64

/* ── Byte counters (STA/WAN side) ─────────────────────────────── */
uint64_t get_sta_bytes_sent(void);
uint64_t get_sta_bytes_received(void);

/* ── NAPT / forwarding ────────────────────────────────────────── */
extern bool ap_nat_enabled;
extern uint16_t ap_mss_clamp;   /* TCP MSS clamp for PAN upstream (0=disabled) */

#ifdef __cplusplus
}
#endif
