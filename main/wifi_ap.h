#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Maximum length for SSID (excluding null terminator) */
#define WIFI_AP_SSID_MAX_LEN      32
/** Maximum length for password (excluding null terminator) */
#define WIFI_AP_PASSWORD_MAX_LEN  64

/** Default AP configuration values */
#define WIFI_AP_DEFAULT_SSID      "TinyPAN-Bridge"
#define WIFI_AP_DEFAULT_PASSWORD  "bridge1234"
#define WIFI_AP_DEFAULT_CHANNEL   1
#define WIFI_AP_DEFAULT_MAX_CONN  4

/** AP subnet (shared with captive DNS and other modules) */
#define BRIDGE_AP_IP_OCT1  192
#define BRIDGE_AP_IP_OCT2  168
#define BRIDGE_AP_IP_OCT3  99
#define BRIDGE_AP_IP_OCT4  1
#define BRIDGE_AP_NETMASK  "255.255.255.0"
/** Pre-formatted gateway string for log messages */
#define BRIDGE_AP_IP_STR   "192.168.99.1"

/** SoftAP configuration */
typedef struct {
    char     ssid[WIFI_AP_SSID_MAX_LEN + 1];
    char     password[WIFI_AP_PASSWORD_MAX_LEN + 1];
    uint8_t  channel;
    uint8_t  max_connections;
    bool     ssid_hidden;
} bridge_ap_config_t;

/**
 * @brief Initialise WiFi SoftAP mode.
 *
 * Creates the default AP netif with DHCP server, initialises the WiFi
 * subsystem, sets AP mode, and applies the supplied configuration.
 * Does NOT start the AP — call wifi_ap_start() afterwards.
 *
 * @param config  AP configuration (SSID, password, channel, etc.).
 *                If NULL, defaults are used.
 * @return ESP_OK on success.
 */
esp_err_t wifi_ap_init(const bridge_ap_config_t *config);

/**
 * @brief Start the WiFi SoftAP (must be called after wifi_ap_init).
 * @return ESP_OK on success.
 */
esp_err_t wifi_ap_start(void);

/**
 * @brief Stop the WiFi SoftAP.
 * @return ESP_OK on success.
 */
esp_err_t wifi_ap_stop(void);

/**
 * @brief Read persistent AP configuration from NVS.
 *
 * Loads the last-saved config.  If no record exists in NVS the
 * output struct is filled with compile-time defaults.
 *
 * @param[out] config  Destination buffer.
 * @return ESP_OK on success.
 */
esp_err_t wifi_ap_get_config(bridge_ap_config_t *config);

/**
 * @brief Persist AP configuration to NVS.
 *
 * The stored values are applied on the next wifi_ap_init() call.
 *
 * @param config  Configuration to save.
 * @return ESP_OK on success.
 */
esp_err_t wifi_ap_set_config(const bridge_ap_config_t *config);

/**
 * @brief Return the number of currently connected stations.
 * @return Station count, or -1 on error.
 */
int wifi_ap_get_connected_clients(void);

/**
 * @brief Get the lwIP netif used by the WiFi SoftAP (for NAT routing).
 *
 * @return Pointer to the netif, or NULL if not initialised.
 */
struct netif *wifi_ap_get_netif(void);

/**
 * @brief Set the DNS server handed out by the AP DHCP server.
 *
 * Called after the PAN link acquires an IP to propagate the phone's
 * DNS server to WiFi clients. Falls back to 8.8.8.8 if dns_addr is 0.
 *
 * @param dns_addr  DNS server IPv4 address in network byte order.
 * @return ESP_OK on success.
 */
esp_err_t wifi_ap_set_dns(uint32_t dns_addr);

/**
 * @brief Check if BT keepalive should be paused.
 *
 * Returns true when a WiFi station recently failed the 4-way handshake
 * (reason 15) and BT keepalive should be temporarily suspended to give
 * the next handshake attempt uncontested RF access.
 *
 * @return true if BT keepalive should be paused, false otherwise.
 */
bool wifi_ap_bt_pause_active(void);

/**
 * @brief Pause BT keepalive for the given duration.
 *
 * Used when a station join/leave is detected (via station-list polling —
 * the AP station events never reach the event handler on this build) to
 * give the EAPOL 4-way handshake uncontested RF.
 *
 * @param duration_ms  Pause window in milliseconds.
 */
void wifi_ap_bt_pause(uint32_t duration_ms);

#ifdef __cplusplus
}
#endif
