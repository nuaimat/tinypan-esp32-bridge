/**
 * @file pan_manager.c
 * @brief PAN manager wrapping the TinyPAN library.
 *
 * Bridges the TinyPAN BNEP client library to the rest of the bridge
 * firmware.  Handles init, connect/disconnect, process dispatch, IP
 * queries, and event forwarding.
 *
 * Because TinyPAN copies the config at tinypan_init() time (with no
 * public API to update remote_addr later), pan_manager_connect()
 * deinits and reinits the library with the supplied phone BD address.
 */

#include "pan_manager.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <string.h>
#include "esp_log.h"
#include "tinypan_hal.h"
#include "tinypan_lwip_netif.h"

static const char *TAG = "pan_manager";

/* ── Internal state ──────────────────────────────────────────── */

static bool s_initialised = false;

/* ── Callback slot ───────────────────────────────────────────── */

static pan_manager_event_cb_t s_event_cb = NULL;

/* ── TinyPAN event callback ──────────────────────────────────── */

static void tp_event_cb(tinypan_event_t event, void *user_data)
{
    (void)user_data;

    switch (event) {
    case TINYPAN_EVENT_IP_ACQUIRED: {
        tinypan_ip_info_t ip;
        if (tinypan_get_ip_info(&ip) == TINYPAN_OK) {
            ESP_LOGI(TAG, "IP acquired: %08lx / mask %08lx / gw %08lx / dns %08lx",
                     (unsigned long)ip.ip_addr, (unsigned long)ip.netmask,
                     (unsigned long)ip.gateway, (unsigned long)ip.dns_server);
        }
        if (s_event_cb)
            s_event_cb(PAN_MANAGER_EVENT_IP_ACQUIRED);
        break;
    }

    case TINYPAN_EVENT_CONNECTED:
        ESP_LOGI(TAG, "PAN transport connected (BNEP established)");
        if (s_event_cb)
            s_event_cb(PAN_MANAGER_EVENT_CONNECTED);
        break;

    case TINYPAN_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "PAN link disconnected");
        if (s_event_cb)
            s_event_cb(PAN_MANAGER_EVENT_DISCONNECTED);
        break;

    case TINYPAN_EVENT_ERROR:
        ESP_LOGW(TAG, "TinyPAN error event — state=%s",
                 tinypan_state_to_string(tinypan_get_state()));
        if (s_event_cb)
            s_event_cb(PAN_MANAGER_EVENT_ERROR);
        break;

    case TINYPAN_EVENT_IP_LOST:
        /* Fires on spontaneous link loss (supervisor clears the IP on
         * L2CAP disconnect) as well as on explicit tinypan_stop().
         * TINYPAN_EVENT_DISCONNECTED only fires on explicit stops, so
         * without this, listeners never learn about link drops and
         * stale NAT state (NAPT, default route, static ARP) persists. */
        ESP_LOGI(TAG, "PAN IP lost");
        if (s_event_cb)
            s_event_cb(PAN_MANAGER_EVENT_DISCONNECTED);
        break;

    default:
        /* TINYPAN_EVENT_STATE_CHANGED is logged but not forwarded. */
        break;
    }
}

/* ==================================================================
 *  Public API
 * ================================================================== */

esp_err_t pan_manager_init(void)
{
    if (s_initialised) {
        ESP_LOGW(TAG, "Already initialised");
        return ESP_OK;
    }

    tinypan_config_t cfg;
    tinypan_config_init(&cfg);

    /* ESP-IDF already initialises lwIP — do not double-init */
    cfg.auto_init_lwip = false;

    /* Infinite reconnect attempts: this is an unattended IoT bridge that
     * must self-recover. Capping attempts caused permanent ERROR state
     * requiring a manual reboot. BT inquiry (web UI scan) is a separate
     * code path and is not blocked by L2CAP reconnect paging. */
    cfg.max_reconnect_attempts = 0;  /* 0 = infinite */

    tinypan_error_t ret = tinypan_init(&cfg);
    if (ret != TINYPAN_OK) {
        ESP_LOGE(TAG, "tinypan_init failed: %d", ret);
        return ESP_FAIL;
    }

    tinypan_set_event_callback(tp_event_cb, NULL);

    s_initialised = true;
    ESP_LOGI(TAG, "PAN manager initialised");
    return ESP_OK;
}

esp_err_t pan_manager_connect(esp_bd_addr_t phone_bda)
{
    if (!s_initialised) {
        ESP_LOGE(TAG, "Not initialised");
        return ESP_ERR_INVALID_STATE;
    }

    if (tinypan_get_state() != TINYPAN_STATE_IDLE) {
        ESP_LOGW(TAG, "Already connected or connecting — stopping first");
        tinypan_stop();
    }

    /* Set the remote BD address via TinyPAN API (no deinit/reinit).
     * When stopped, tinypan_set_remote_addr updates the config directly. */
    tinypan_error_t ret = tinypan_set_remote_addr(phone_bda);
    if (ret != TINYPAN_OK) {
        ESP_LOGE(TAG, "Failed to set remote addr: %d", ret);
        return ESP_FAIL;
    }

    ret = tinypan_start();
    if (ret != TINYPAN_OK) {
        ESP_LOGE(TAG, "tinypan_start failed: %d", ret);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Connecting to phone %02x:%02x:%02x:%02x:%02x:%02x",
             phone_bda[0], phone_bda[1], phone_bda[2],
             phone_bda[3], phone_bda[4], phone_bda[5]);
    return ESP_OK;
}

esp_err_t pan_manager_disconnect(void)
{
    if (!s_initialised) {
        return ESP_ERR_INVALID_STATE;
    }

    tinypan_stop();
    ESP_LOGI(TAG, "Disconnected");
    return ESP_OK;
}

void pan_manager_process(void)
{
    if (!s_initialised)
        return;

    /* tinypan_process() already calls tinypan_netif_process()
     * and hal_bt_poll() internally — no separate netif call needed. */
    tinypan_process();
}

esp_err_t pan_manager_get_ip_info(tinypan_ip_info_t *info)
{
    if (!s_initialised) {
        return ESP_ERR_INVALID_STATE;
    }

    tinypan_error_t ret = tinypan_get_ip_info(info);
    if (ret != TINYPAN_OK) {
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}

struct netif *pan_manager_get_netif(void)
{
    return tinypan_netif_get();
}

bool pan_manager_is_online(void)
{
    return s_initialised && tinypan_is_online();
}

const char *pan_manager_get_state_string(void)
{
    if (!s_initialised)
        return "NOT_INIT";
    return tinypan_state_to_string(tinypan_get_state());
}

/* ── Callback registration ───────────────────────────────────── */

void pan_manager_register_event_cb(pan_manager_event_cb_t cb)
{
    s_event_cb = cb;
}
