#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_bt_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/** BNEP PSM for L2CAP connections */
#define BT_MGR_BNEP_PSM  0x000F

/** Maximum number of devices tracked during a discovery session */
#define BT_DISCOVERY_MAX_DEVICES  32

/** Maximum length of a remote device name (including null terminator) */
#define BT_DISCOVERY_NAME_MAX  32

/* ── Types ──────────────────────────────────────────────────────── */

/**
 * @brief  A discovered Bluetooth device.
 *
 * Populated during inquiry and deduplicated by BD address — when the
 * same device is seen multiple times the entry keeps the highest RSSI.
 */
typedef struct {
    char          name[BT_DISCOVERY_NAME_MAX];
    esp_bd_addr_t bda;
    uint32_t      cod;
    int8_t        rssi;
} bt_discovered_device_t;

/** Callback for discovered devices during inquiry */
typedef void (*bt_manager_discovery_cb_t)(
    const esp_bd_addr_t bda,
    const char          *name,
    int                  rssi,
    uint32_t             cod
);

/** Callback for connection state changes */
typedef void (*bt_manager_conn_cb_t)(
    const esp_bd_addr_t bda,
    bool                connected,
    int                 reason
);

/** Callback for authentication events */
typedef void (*bt_manager_auth_cb_t)(
    const esp_bd_addr_t bda,
    bool                success
);

/** Maximum length for a bonded device name (including null terminator) */
#define BT_BONDED_NAME_MAX  32

/** A bonded (paired) device stored in NVS */
typedef struct {
    esp_bd_addr_t bda;
    char          name[BT_BONDED_NAME_MAX];
} bt_bonded_device_t;

/* ── Lifecycle ──────────────────────────────────────────────────── */

/**
 * @brief  Initialise Bluetooth controller, Bluedroid host stack, GAP,
 *         and SSP security (IO_CAP_NONE for no-UI auto-confirm).
 *
 *         Sets device name to "TinyPAN Bridge" and CoD to LAN/NAP.
 *
 * @return ESP_OK on success.
 */
esp_err_t bt_manager_init(void);

/**
 * @brief  Check whether bt_manager_init() has been called successfully.
 * @return true if the BT controller, Bluedroid, and GAP are initialised.
 */
bool bt_manager_is_initialised(void);

/* ── Discovery ──────────────────────────────────────────────────── */

/**
 * @brief  Start device discovery (inquiry) to find nearby BR/EDR devices.
 *
 *         Results are stored in an internal, deduplicated list (strongest
 *         RSSI per device) and also delivered via the per-device callback
 *         registered with bt_manager_register_discovery_cb().
 *
 * @param  duration_sec   Inquiry duration in seconds (1–61).  Internally
 *                        converted to 1.28 s units; clamped to 1–48 slots.
 * @return ESP_OK on success.
 */
esp_err_t bt_manager_start_discovery(uint8_t duration_sec);

/**
 * @brief  Cancel an ongoing device discovery.
 * @return ESP_OK on success.
 */
esp_err_t bt_manager_stop_discovery(void);

/**
 * @brief  Check whether remote-name resolution is still in progress.
 *
 *         After inquiry completes, names of unnamed devices are fetched
 *         one by one via remote name requests. Poll this to know when
 *         the discovered-device list is final.
 *
 * @return true while name requests are outstanding.
 */
bool bt_manager_is_resolving_names(void);

/**
 * @brief  Retrieve the deduplicated list of devices discovered so far.
 *
 *         The returned pointer is valid until the next call to
 *         bt_manager_start_discovery().
 *
 * @param[out] devices  Receives pointer to the internal device array.
 * @param[out] count    Receives number of valid entries in the array.
 * @return ESP_OK on success.
 */
esp_err_t bt_manager_get_discovered_devices(
    const bt_discovered_device_t **devices,
    size_t                         *count
);

/* ── Connection ─────────────────────────────────────────────────── */

/**
 * @brief  Initiate an L2CAP connection to a remote device on the BNEP PSM.
 *
 *         The actual L2CAP open/close events are handled by the TinyPAN HAL.
 *         Connection state is tracked internally and reported via the
 *         connection callback.
 *
 * @param  bda  Remote Bluetooth device address.
 * @return ESP_OK on success (connection initiation started).
 */
esp_err_t bt_manager_connect(esp_bd_addr_t bda);

/**
 * @brief  Disconnect from the currently connected remote device.
 *
 *         The underlying L2CAP channel is closed through the TinyPAN HAL.
 *
 * @return ESP_OK on success.
 */
esp_err_t bt_manager_disconnect(void);

/**
 * @brief  Check whether a remote device is currently connected.
 * @return true if connected.
 */
bool bt_manager_is_connected(void);

/**
 * @brief  Get the BD address of the currently connected remote device.
 *
 * @param[out] bda  Buffer to receive the 6-byte BD address.
 * @return ESP_OK on success, ESP_ERR_INVALID_STATE if not connected.
 */
esp_err_t bt_manager_get_peer_bda(esp_bd_addr_t bda);

/* ── Pairing / Bonding persistence ──────────────────────────────── */

/**
 * @brief  Load the bonded device from NVS.
 *
 * Reads the BD address and name of the last successfully paired device
 * from NVS storage.  The stored bond is the application-level record;
 * the link key itself is managed by Bluedroid internally.
 *
 * @param[out] device  Populated with the saved BDA and name on success.
 * @return ESP_OK if a bonded device was found in NVS,
 *         ESP_ERR_NOT_FOUND if no bond has ever been saved.
 */
esp_err_t bt_manager_load_bonded_device(bt_bonded_device_t *device);

/* ── Callback registration ─────────────────────────────────────── */

void bt_manager_register_discovery_cb(bt_manager_discovery_cb_t cb);
void bt_manager_register_connection_cb(bt_manager_conn_cb_t cb);
void bt_manager_register_auth_cb(bt_manager_auth_cb_t cb);

#ifdef __cplusplus
}
#endif
