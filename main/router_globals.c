/* Global variable definitions for TinyPAN v2 router config.
 * Adapted from martin-ger/esp32_nat_router.
 */

#include "router_config.h"
#include "wifi_config.h"

/* ── IP configuration ─────────────────────────────────────────── */
uint32_t my_ap_ip   = 0;

/* ── Connection state ─────────────────────────────────────────── */
bool ap_nat_enabled  = false;

/* ── Byte counters ────────────────────────────────────────────── */
uint16_t ap_mss_clamp        = 0;

/* ── DHCP reservations ────────────────────────────────────────── */
#include "dhcp_reservations.h"
struct dhcp_reservation_entry dhcp_reservations[MAX_DHCP_RESERVATIONS] = {0};
