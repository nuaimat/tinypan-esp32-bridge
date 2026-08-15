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
extern uint64_t sta_bytes_sent;
extern uint64_t sta_bytes_received;
uint64_t get_sta_bytes_sent(void);
uint64_t get_sta_bytes_received(void);
void reset_sta_byte_counts(void);

/* ── NAPT / forwarding ────────────────────────────────────────── */
extern bool ap_nat_enabled;
extern uint16_t ap_mss_clamp;   /* TCP MSS clamp for PAN upstream (0=disabled) */

/* ── Uptime ───────────────────────────────────────────────────── */
uint32_t get_uptime_seconds(void);
void format_uptime(uint32_t seconds, char *buf, size_t buf_len);
void format_boot_time(char *buf, size_t buf_len);

/* ── Client tracking ──────────────────────────────────────────── */
void resync_connect_count(void);

#ifdef __cplusplus
}
#endif
