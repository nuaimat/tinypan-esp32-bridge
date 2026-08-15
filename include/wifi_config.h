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

#define DEFAULT_AP_IP "192.168.99.1"

/* ── IP configuration ─────────────────────────────────────────── */
extern uint32_t my_ip;       /* PAN/WAN side IP (0 until PAN DHCP) */
extern uint32_t my_ap_ip;    /* AP/LAN side IP (set at wifi_ap_init) */

/* ── WiFi AP settings ─────────────────────────────────────────── */
extern char *ap_ssid;
extern char *ap_passwd;
extern char *ap_dns;
extern uint8_t ap_ssid_hidden;

/* ── Connection state ──────────────────────────────────────────── */
extern uint16_t connect_count;
extern bool ap_connect;

#ifdef __cplusplus
}
#endif
