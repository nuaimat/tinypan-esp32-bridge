#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_bt_defs.h"
#include "tinypan.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── Forward declarations ─────────────────────────────────────── */

struct netif;

/* ── Event types ───────────────────────────────────────────────── */

/** Events the pan_manager reports to registered listeners */
typedef enum {
    PAN_MANAGER_EVENT_IP_ACQUIRED,    /**< DHCP completed, IP assigned */
    PAN_MANAGER_EVENT_DISCONNECTED,   /**< PAN link dropped */
    PAN_MANAGER_EVENT_CONNECTED,      /**< PAN transport established */
    PAN_MANAGER_EVENT_ERROR,          /**< An unrecoverable error occurred */
} pan_manager_event_t;

/** Callback type for pan_manager events */
typedef void (*pan_manager_event_cb_t)(pan_manager_event_t event);

/* ── API ───────────────────────────────────────────────────────── */

/**
 * @brief  Initialise the PAN manager and the TinyPAN library.
 *
 * Creates a default TinyPAN config (auto_init_lwip = false since
 * ESP-IDF manages lwIP), calls tinypan_config_init + tinypan_init,
 * registers an internal event callback, and initialises the
 * TinyPAN lwIP netif adapter.
 *
 * Call once after bt_manager_init() and before pan_manager_connect().
 *
 * @return ESP_OK on success.
 */
esp_err_t pan_manager_init(void);

/**
 * @brief  Connect to a phone acting as a Bluetooth NAP.
 *
 * Stores the peer BD address and starts the TinyPAN connection
 * sequence (L2CAP → BNEP setup → DHCP).
 *
 * @param  phone_bda  6-byte Bluetooth device address of the phone.
 * @return ESP_OK on success, or ESP_ERR_INVALID_STATE if not
 *         initialised or already connected.
 */
esp_err_t pan_manager_connect(esp_bd_addr_t phone_bda);

/**
 * @brief  Disconnect from the phone and stop TinyPAN.
 * @return ESP_OK on success.
 */
esp_err_t pan_manager_disconnect(void);

/**
 * @brief  Periodic processing — call from the main loop.
 *
 * Dispatches tinypan_process() and tinypan_netif_process() so the
 * state machine, timeouts, and lwIP housekeeping run.
 */
void pan_manager_process(void);

/**
 * @brief  Get the current PAN IP configuration.
 *
 * @param[out] info  Populated with IP address, netmask, gateway,
 *                   and DNS server (network byte order).
 * @return ESP_OK if an IP has been acquired, otherwise an error.
 */
esp_err_t pan_manager_get_ip_info(tinypan_ip_info_t *info);

/**
 * @brief  Get the lwIP netif used by TinyPAN (for NAT routing).
 *
 * @return Pointer to the netif, or NULL if not initialised.
 */
struct netif *pan_manager_get_netif(void);

/**
 * @brief  Check whether the PAN link is online (IP acquired).
 * @return true if online.
 */
bool pan_manager_is_online(void);

/**
 * @brief  Get the current TinyPAN state as a human-readable string.
 * @return String literal.
 */
const char *pan_manager_get_state_string(void);

/* ── Callback registration ───────────────────────────────────── */

/**
 * @brief  Register a callback for PAN manager events.
 *
 * Only one callback slot — later registrations overwrite earlier ones.
 * The callback fires in the context of the main loop (inside
 * pan_manager_process).
 */
void pan_manager_register_event_cb(pan_manager_event_cb_t cb);

#ifdef __cplusplus
}
#endif
