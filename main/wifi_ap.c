#include <string.h>
#include "esp_log.h"
#include "esp_err.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_netif_ip_addr.h"
#include "esp_netif_net_stack.h"
#include "esp_mac.h"
#include "nvs_flash.h"
#include "lwip/ip4_addr.h"
#include "dhcpserver/dhcpserver.h"
#include "esp_coexist.h"

#include "wifi_ap.h"
#include "esp_event.h"
#include "esp_timer.h"

static const char *TAG = "wifi_ap";

/* ── NVS keys & namespace ──────────────────────────────────────── */
#define NVS_NAMESPACE  "wifi_ap"
#define NVS_KEY_SSID   "ssid"
#define NVS_KEY_PASS   "password"
#define NVS_KEY_CHAN   "channel"
#define NVS_KEY_MAXCON "max_conn"

/* ── DHCP pool (per spec) ──────────────────────────────────────── */
#define AP_IP_ADDR      IPADDR4_INIT_BYTES(BRIDGE_AP_IP_OCT1, BRIDGE_AP_IP_OCT2, BRIDGE_AP_IP_OCT3, BRIDGE_AP_IP_OCT4)
#define AP_GW_ADDR      IPADDR4_INIT_BYTES(BRIDGE_AP_IP_OCT1, BRIDGE_AP_IP_OCT2, BRIDGE_AP_IP_OCT3, BRIDGE_AP_IP_OCT4)
#define AP_NETMASK      IPADDR4_INIT_BYTES(255, 255, 255, 0)
#define DHCP_START      IPADDR4_INIT_BYTES(BRIDGE_AP_IP_OCT1, BRIDGE_AP_IP_OCT2, BRIDGE_AP_IP_OCT3, 2)
#define DHCP_END        IPADDR4_INIT_BYTES(BRIDGE_AP_IP_OCT1, BRIDGE_AP_IP_OCT2, BRIDGE_AP_IP_OCT3, 100)

/* ── Internal state ────────────────────────────────────────────── */
static esp_netif_t  *s_ap_netif = NULL;
static bool          s_initialised = false;
static bool          s_running = false;

/* BT keepalive pause: set when WiFi station handshake fails (reason 15),
 * cleared when station connects. BNEP transport checks this before sending
 * keepalive to give WiFi uncontested RF during EAPOL retry. */
static volatile int64_t s_bt_pause_until_ms = 0;

#define WIFI_AP_BT_PAUSE_MS  10000

static void wifi_ap_event_handler(void *arg, esp_event_base_t event_base,
                                   int32_t event_id, void *event_data)
{
    ESP_LOGI(TAG, "event: base=%s id=%d", event_base ? event_base : "NULL", (int)event_id);

    if (event_id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t *event = (wifi_event_ap_staconnected_t *)event_data;
        ESP_LOGI(TAG, "Station connected: %02x:%02x:%02x:%02x:%02x:%02x, AID=%d",
                 event->mac[0], event->mac[1], event->mac[2],
                 event->mac[3], event->mac[4], event->mac[5], event->aid);
        s_bt_pause_until_ms = 0;
    } else if (event_id == WIFI_EVENT_AP_STADISCONNECTED) {
        wifi_event_ap_stadisconnected_t *event = (wifi_event_ap_stadisconnected_t *)event_data;
        ESP_LOGW(TAG, "Station disconnected: %02x:%02x:%02x:%02x:%02x:%02x, AID=%d, reason=%d",
                 event->mac[0], event->mac[1], event->mac[2],
                 event->mac[3], event->mac[4], event->mac[5], event->aid, event->reason);
        if (event->reason == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT) {
            int64_t now = esp_timer_get_time() / 1000;
            s_bt_pause_until_ms = now + WIFI_AP_BT_PAUSE_MS;
            ESP_LOGI(TAG, "Handshake timeout — pausing BT keepalive for %dms",
                     WIFI_AP_BT_PAUSE_MS);
        }
    }
}

bool wifi_ap_bt_pause_active(void)
{
    if (s_bt_pause_until_ms == 0)
        return false;
    int64_t now = esp_timer_get_time() / 1000;
    if (now >= s_bt_pause_until_ms) {
        s_bt_pause_until_ms = 0;
        return false;
    }
    return true;
}

void wifi_ap_bt_pause(uint32_t duration_ms)
{
    int64_t now = esp_timer_get_time() / 1000;
    s_bt_pause_until_ms = now + duration_ms;
}

/* ── Helpers ───────────────────────────────────────────────────── */

/** Fill config with compile-time defaults. */
static void config_set_defaults(bridge_ap_config_t *cfg)
{
    strncpy(cfg->ssid,     WIFI_AP_DEFAULT_SSID,     sizeof(cfg->ssid) - 1);
    strncpy(cfg->password, WIFI_AP_DEFAULT_PASSWORD, sizeof(cfg->password) - 1);
    cfg->channel          = WIFI_AP_DEFAULT_CHANNEL;
    cfg->max_connections  = WIFI_AP_DEFAULT_MAX_CONN;
    cfg->ssid_hidden      = false;
}

/** Apply a bridge_ap_config_t to the ESP wifi_config_t used by esp_wifi_set_config. */
static void apply_ap_config(const bridge_ap_config_t *src, wifi_config_t *dst)
{
    memset(dst, 0, sizeof(*dst));
    dst->ap.channel         = src->channel;
    dst->ap.max_connection  = src->max_connections;
    dst->ap.ssid_hidden     = src->ssid_hidden ? 1 : 0;
    dst->ap.authmode        = (src->password[0] == '\0') ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;
    dst->ap.beacon_interval = 100;   /* explicit: default 100ms beacon interval */
    dst->ap.dtim_period     = 2;     /* explicit: DTIM every 2 beacons */

    strncpy((char *)dst->ap.ssid,      src->ssid,     sizeof(dst->ap.ssid) - 1);
    strncpy((char *)dst->ap.password,  src->password, sizeof(dst->ap.password) - 1);
}

/* ── Public API ────────────────────────────────────────────────── */

esp_err_t wifi_ap_init(const bridge_ap_config_t *config)
{
    if (s_initialised) {
        ESP_LOGW(TAG, "Already initialised");
        return ESP_OK;
    }

    bridge_ap_config_t cfg;
    if (config) {
        memcpy(&cfg, config, sizeof(cfg));
    } else {
        config_set_defaults(&cfg);
    }

    /* 1. Create default AP netif with DHCP server */
    s_ap_netif = esp_netif_create_default_wifi_ap();
    if (!s_ap_netif) {
        ESP_LOGE(TAG, "Failed to create AP netif");
        return ESP_FAIL;
    }

    /* 2. Configure static IP and DHCP pool */
    esp_netif_ip_info_t ip_info = {
        .ip      = { .addr = ipaddr_addr(BRIDGE_AP_IP_STR) },
        .gw      = { .addr = ipaddr_addr(BRIDGE_AP_IP_STR) },
        .netmask = { .addr = ipaddr_addr(BRIDGE_AP_NETMASK) },
    };
    esp_netif_dhcps_stop(s_ap_netif);
    esp_netif_set_ip_info(s_ap_netif, &ip_info);
    esp_netif_dhcps_start(s_ap_netif);

    /* 3. WiFi init */
    wifi_init_config_t wifi_cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t ret = esp_wifi_init(&wifi_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Register WiFi event handler for station connect/disconnect */
    ret = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                               &wifi_ap_event_handler, NULL, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "event handler register failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* 4. Set AP mode */
    ret = esp_wifi_set_mode(WIFI_MODE_AP);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_mode failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* 5. Apply AP configuration */
    wifi_config_t wifi_ap_cfg;
    apply_ap_config(&cfg, &wifi_ap_cfg);
    ret = esp_wifi_set_config(WIFI_IF_AP, &wifi_ap_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_config failed: %s", esp_err_to_name(ret));
        return ret;
    }

    s_initialised = true;
    ESP_LOGI(TAG, "SoftAP initialised – SSID: \"%s\", password: ****, channel: %d, max clients: %d",
             cfg.ssid, cfg.channel, cfg.max_connections);
    return ESP_OK;
}

esp_err_t wifi_ap_start(void)
{
    if (!s_initialised) {
        ESP_LOGE(TAG, "Not initialised – call wifi_ap_init() first");
        return ESP_ERR_INVALID_STATE;
    }
    if (s_running) {
        ESP_LOGW(TAG, "Already running");
        return ESP_OK;
    }

    esp_err_t ret = esp_wifi_start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_start failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Set WiFi to minimum power save (required when BT is active).
     * WIFI_PS_NONE is not allowed with BT enabled, but MIN_MODEM gives
     * WiFi the most airtime while satisfying coexistence requirements. */
    esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
    ESP_LOGI(TAG, "WiFi PS set to MIN_MODEM for BT coexistence");

    /* Force HT20 bandwidth — avoids HT40→HT20 channel switch disruption
     * when a station associates, which can cause EAPOL frame loss. */
    esp_wifi_set_bandwidth(WIFI_IF_AP, WIFI_BW_HT20);
    ESP_LOGI(TAG, "WiFi AP bandwidth set to HT20");

    s_running = true;
    ESP_LOGI(TAG, "SoftAP started");
    return ESP_OK;
}

esp_err_t wifi_ap_stop(void)
{
    if (!s_initialised) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_running) {
        return ESP_OK;
    }

    esp_err_t ret = esp_wifi_stop();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_stop failed: %s", esp_err_to_name(ret));
        return ret;
    }

    s_running = false;
    ESP_LOGI(TAG, "SoftAP stopped");
    return ESP_OK;
}

esp_err_t wifi_ap_get_config(bridge_ap_config_t *config)
{
    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }

    config_set_defaults(config); /* fallback */

    nvs_handle_t handle;
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "NVS open(read) failed (%s) – using defaults", esp_err_to_name(ret));
        return ESP_OK;
    }

    size_t len;

    len = sizeof(config->ssid);
    nvs_get_str(handle, NVS_KEY_SSID, config->ssid, &len);

    len = sizeof(config->password);
    nvs_get_str(handle, NVS_KEY_PASS, config->password, &len);

    uint8_t val;
    if (nvs_get_u8(handle, NVS_KEY_CHAN, &val) == ESP_OK)   config->channel = val;
    if (nvs_get_u8(handle, NVS_KEY_MAXCON, &val) == ESP_OK) config->max_connections = val;

    nvs_close(handle);
    return ESP_OK;
}

esp_err_t wifi_ap_set_config(const bridge_ap_config_t *config)
{
    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "NVS open(write) failed: %s", esp_err_to_name(ret));
        return ret;
    }

    nvs_set_str(handle, NVS_KEY_SSID,   config->ssid);
    nvs_set_str(handle, NVS_KEY_PASS,   config->password);
    nvs_set_u8(handle,  NVS_KEY_CHAN,   config->channel);
    nvs_set_u8(handle,  NVS_KEY_MAXCON, config->max_connections);

    ret = nvs_commit(handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "NVS commit failed: %s", esp_err_to_name(ret));
    }
    nvs_close(handle);
    return ret;
}

int wifi_ap_get_connected_clients(void)
{
    if (!s_running) {
        return -1;
    }

    wifi_sta_list_t sta_list;
    esp_err_t ret = esp_wifi_ap_get_sta_list(&sta_list);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_ap_get_sta_list failed: %s", esp_err_to_name(ret));
        return -1;
    }

    return sta_list.num;
}

struct netif *wifi_ap_get_netif(void)
{
    return esp_netif_get_netif_impl(s_ap_netif);
}

esp_err_t wifi_ap_set_dns(uint32_t dns_addr)
{
    if (!s_initialised || !s_ap_netif) {
        return ESP_ERR_INVALID_STATE;
    }

    if (dns_addr == 0) {
        ESP_LOGW(TAG, "DNS address is 0.0.0.0 — using fallback 8.8.8.8");
        dns_addr = lwip_htonl(LWIP_MAKEU32(8, 8, 8, 8));
    }

    esp_netif_dns_info_t dns_info = {
        .ip.u_addr.ip4.addr = dns_addr,
        .ip.type = IPADDR_TYPE_V4,
    };

    esp_err_t ret = esp_netif_set_dns_info(s_ap_netif, ESP_NETIF_DNS_MAIN, &dns_info);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set AP DNS server: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "AP DHCP DNS server set to %d.%d.%d.%d",
             (int)(dns_addr & 0xFF), (int)((dns_addr >> 8) & 0xFF),
             (int)((dns_addr >> 16) & 0xFF), (int)((dns_addr >> 24) & 0xFF));
    return ESP_OK;
}
