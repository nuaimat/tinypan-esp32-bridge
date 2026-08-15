/* Global variable definitions for TinyPAN v2 router config.
 * Adapted from martin-ger/esp32_nat_router.
 */

#include "router_config.h"
#include "wifi_config.h"

/* ── IP configuration ─────────────────────────────────────────── */
uint32_t my_ip      = 0;
uint32_t my_ap_ip   = 0;

/* ── WiFi AP ──────────────────────────────────────────────────── */
char *ap_ssid       = NULL;
char *ap_passwd     = NULL;
char *ap_dns        = NULL;
uint8_t ap_ssid_hidden = 0;

/* ── Connection state ─────────────────────────────────────────── */
uint16_t connect_count = 0;
bool ap_connect      = false;
bool ap_nat_enabled  = true;

/* ── Byte counters ────────────────────────────────────────────── */
uint64_t sta_bytes_sent     = 0;
uint64_t sta_bytes_received = 0;
uint16_t ap_mss_clamp        = 0;

/* ── DHCP reservations ────────────────────────────────────────── */
#include "dhcp_reservations.h"
struct dhcp_reservation_entry dhcp_reservations[MAX_DHCP_RESERVATIONS] = {0};
