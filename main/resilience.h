#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C"
{
#endif

/* ── Configuration ─────────────────────────────────────────────── */

/** Interval between upstream connectivity probes while ONLINE (ms). */
#ifndef RESILIENCE_PROBE_INTERVAL_MS
#define RESILIENCE_PROBE_INTERVAL_MS  20000
#endif

/** Timeout for each DNS probe stage (ms).
 *  Stage 1 (8.8.8.8 direct): 5s — should respond in <1s on cellular.
 *  Stage 2 (phone DNS forwarder): 15s — Android tethering DNS proxy
 *  can take 10+ seconds due to DNS-over-HTTPS or IPv6-first fallback.
 *  Total worst case: 20s, within the 20s probe interval. */
#ifndef RESILIENCE_PROBE_TIMEOUT_DIRECT_MS
#define RESILIENCE_PROBE_TIMEOUT_DIRECT_MS    5000
#endif
#ifndef RESILIENCE_PROBE_TIMEOUT_FALLBACK_MS
#define RESILIENCE_PROBE_TIMEOUT_FALLBACK_MS  15000
#endif

/** Number of consecutive probe failures before forcing a PAN reconnect
 *  when the BT link appears DEAD (no recent BNEP RX). In this case the
 *  link itself is gone, so we reconnect aggressively. */
#ifndef RESILIENCE_PROBE_FAIL_THRESHOLD_DEAD
#define RESILIENCE_PROBE_FAIL_THRESHOLD_DEAD 3
#endif

/** Number of consecutive probe failures before forcing a PAN reconnect
 *  when the BT link is still ALIVE (recent BNEP keepalive RX). In this
 *  case the phone likely lost cellular data (5G dead spot, VPN change)
 *  and will probably auto-recover, so we wait much longer before
 *  tearing down a healthy BT link. At 20s probe interval, 15 failures
 *  = 5 minutes of patience. */
#ifndef RESILIENCE_PROBE_FAIL_THRESHOLD_ALIVE
#define RESILIENCE_PROBE_FAIL_THRESHOLD_ALIVE 15
#endif

/** If the system has been continuously offline (no IP / not ONLINE) for
 *  this long, reboot the ESP32 as a last-resort recovery (ms). */
#ifndef RESILIENCE_OFFLINE_REBOOT_MS
#define RESILIENCE_OFFLINE_REBOOT_MS  120000  /* 2 minutes */
#endif

/** Fallback DNS server used for upstream probes when the phone's DHCP-
 *  provided DNS is unavailable or 0.0.0.0 (Google Public DNS).
 *  In network byte order — consistent with tinypan_ip_info_t.dns_server. */
#ifndef RESILIENCE_PROBE_DNS_FALLBACK
#define RESILIENCE_PROBE_DNS_FALLBACK  0x08080808U  /* 8.8.8.8 (network byte order) */
#endif

/** Interval for refreshing the DNS forward server from the PAN DHCP
 *  lease while online (ms). Handles VPN connect/disconnect on the
 *  phone that changes the system DNS server. */
#ifndef RESILIENCE_DNS_REFRESH_MS
#define RESILIENCE_DNS_REFRESH_MS     60000  /* 1 minute */
#endif

/** Domain queried during probes. */
#ifndef RESILIENCE_PROBE_DOMAIN
#define RESILIENCE_PROBE_DOMAIN        "a.root-servers.net"
#endif

/** Max age of last BNEP RX (ms) for the BT link to be considered "alive".
 *  BNEP keepalive runs every 2s, so anything within 15s means the link
 *  is healthy. Used to distinguish "BT dead" from "cellular dead". */
#ifndef RESILIENCE_BT_LINK_ALIVE_MS
#define RESILIENCE_BT_LINK_ALIVE_MS   15000
#endif

/* ── API ───────────────────────────────────────────────────────── */

/**
 * @brief Initialise the resilience watchdog.
 *
 * Call once at startup, after pan_manager and nat_router are initialised.
 */
esp_err_t resilience_init(void);

/**
 * @brief Periodic processing — call from the main loop.
 *
 * Performs:
 *  - Upstream connectivity probes (DNS query through PAN) when ONLINE.
 *  - Forces a PAN reconnect after repeated probe failures.
 *  - Tracks continuous-offline duration and reboots if exceeded.
 *  - Verifies NAT/route health and re-enables if lwIP dropped it.
 */
void resilience_process(void);

/**
 * @brief Notify the watchdog that the PAN link came online.
 *
 * Resets probe failure counters. Called automatically from resilience_process
 * by polling pan_manager_is_online(), but can also be called explicitly.
 */
void resilience_notify_online(void);

/**
 * @brief Notify the watchdog that the PAN link went offline.
 *
 * Starts the offline-duration timer for the reboot fallback.
 */
void resilience_notify_offline(void);

/**
 * @brief Get the number of consecutive probe failures (for diagnostics).
 */
int resilience_get_probe_failures(void);

/**
 * @brief Set the reconnect-in-progress flag.
 *
 * Called by the WiFi handshake logic when it intentionally disconnects
 * BT to give WiFi uncontested RF access. When true, the resilience
 * watchdog suspends probes and reconnect attempts (it would otherwise
 * interpret the intentional disconnect as a link failure).
 *
 * @param in_progress  true when BT is intentionally disconnected for WiFi
 */
void resilience_set_reconnect_in_progress(bool in_progress);

#ifdef __cplusplus
}
#endif
