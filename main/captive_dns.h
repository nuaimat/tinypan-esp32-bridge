#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief  Start a minimal DNS server for IoT clients.
 *
 * Creates a UDP listener on port 53 (lwIP raw API).
 *
 * When PAN is offline: responds to all queries with DNS RCODE=2
 * (SERVFAIL) and 0 answer records. This tells IoT clients the DNS
 * server is temporarily unavailable so they retry quickly, instead
 * of caching a wrong captive-portal A record that they can never
 * "log in" to resolve.
 *
 * When PAN is online: forwards queries to the phone's DNS server and
 * relays replies back to the client.
 *
 * Must be called after wifi_ap_start() so the SoftAP netif is up.
 *
 * @return ESP_OK on success.
 */
esp_err_t captive_dns_start(void);

/**
 * @brief  Stop the captive-portal DNS server and release resources.
 */
void captive_dns_stop(void);

/**
 * @brief Toggle DNS passthrough mode.
 *
 * When PAN is online, set true to bypass DNS hijacking so that queries
 * flow through the phone's network. When false (default), all queries
 * return the AP gateway IP for captive-portal detection.
 */
void captive_dns_set_passthrough(bool enabled);

/**
 * @brief Set the upstream DNS server for forwarding mode.
 *
 * When passthrough is enabled, queries are forwarded to this server
 * and replies relayed back to the client.  Call this before or together
 * with captive_dns_set_passthrough(true).
 *
 * @param dns_addr  DNS server IPv4 address in network byte order.
 */
void captive_dns_set_forward_server(uint32_t dns_addr);

#ifdef __cplusplus
}
#endif
