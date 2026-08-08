#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief  Initialise and start the HTTP server on port 80.
 *
 * Mounts the SPIFFS "www" partition at /www, registers a wildcard
 * handler that serves static files from /www with automatic content-type
 * detection by file extension, and starts the HTTP server on port 80.
 *
 * Call once after wifi_ap_init() so the netif IP is available.
 *
 * @return ESP_OK on success.
 */
esp_err_t http_server_init(void);

#ifdef __cplusplus
}
#endif
