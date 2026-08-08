#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief  Initialise the NAT router between BT PAN (WAN) and WiFi AP (LAN).
 *
 * Registers for the PAN IP_ACQUIRED event.  Once the PAN link comes online
 * the router enables NAPT on the PAN netif and sets it as the lwIP default
 * route so WiFi clients can reach the internet through the phone's connection.
 *
 * Call once after pan_manager_init() and wifi_ap_init().
 *
 * @return ESP_OK on success.
 */
esp_err_t nat_router_init(void);

/**
 * @brief  Check whether NAT routing is active.
 * @return true if the PAN link is up and NAPT is enabled.
 */
bool nat_router_is_active(void);

#ifdef __cplusplus
}
#endif
