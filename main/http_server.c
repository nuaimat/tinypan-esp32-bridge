#include "http_server.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_log.h"
#include "esp_spiffs.h"
#include "esp_http_server.h"
#include "esp_timer.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "lwip/inet.h"
#include "lwip/netif.h"

#include "bt_manager.h"
#include "bt_state.h"
#include "pan_manager.h"
#include "wifi_ap.h"

/* ── Module state ──────────────────────────────────────────────────── */

static const char *TAG = "http_server";

static httpd_handle_t g_server      = NULL;
static int64_t        g_start_time  = 0;   /* boot-relative µs, set in init */

/* ── SPIFFS mount ──────────────────────────────────────────────────── */

static esp_err_t mount_spiffs(void)
{
    esp_vfs_spiffs_conf_t conf = {
        .base_path              = "/www",
        .partition_label        = "www",
        .max_files              = 5,
        .format_if_mount_failed = true,
    };

    esp_err_t ret = esp_vfs_spiffs_register(&conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPIFFS mount failed: %s (continuing without static files)",
                 esp_err_to_name(ret));
        return ret;
    }

    size_t total = 0, used = 0;
    esp_spiffs_info(conf.partition_label, &total, &used);
    ESP_LOGI(TAG, "SPIFFS \"%s\" mounted: %u / %u bytes used",
             conf.partition_label, used, total);
    return ESP_OK;
}

/* ── GET / ─────────────────────────────────────────────────────────── */

static const char ROOT_HTML[] =
    "<!DOCTYPE html>\n"
    "<html lang=\"en\">\n"
    "<head>\n"
    "  <meta charset=\"UTF-8\">\n"
    "  <meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">\n"
    "  <title>TinyPAN Bridge</title>\n"
    "  <style>\n"
    "    *{box-sizing:border-box;margin:0;padding:0}\n"
    "    body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',sans-serif;\n"
    "         background:#111;color:#eee;max-width:640px;margin:60px auto;padding:0 20px}\n"
    "    h1{font-size:1.7em;color:#4af;margin-bottom:4px}\n"
    "    .sub{color:#888;margin-bottom:24px}\n"
    "    .card{background:#1a1a2e;border-radius:10px;padding:18px;margin:12px 0}\n"
    "    .dot{display:inline-block;width:10px;height:10px;border-radius:50%;margin-right:8px}\n"
    "    .dot.on{background:#4f8;box-shadow:0 0 6px #4f8}\n"
    "    .dot.off{background:#f44;box-shadow:0 0 6px #f44}\n"
    "    .row{display:flex;justify-content:space-between;align-items:center;margin:6px 0}\n"
    "    .lbl{color:#888;font-size:.85em;text-transform:uppercase}\n"
    "    .val{font-size:1.05em}\n"
    "  </style>\n"
    "</head>\n"
    "<body>\n"
    "  <h1>🔷 TinyPAN Bridge</h1>\n"
    "  <p class=\"sub\">Bluetooth PAN → Wi-Fi bridge for retro computing</p>\n"
    "  <div class=\"card\" id=\"status-card\">\n"
    "    <div class=\"row\"><span class=\"lbl\">Bluetooth</span><span class=\"val\" id=\"bt\">…</span></div>\n"
    "    <div class=\"row\"><span class=\"lbl\">Wi-Fi clients</span><span class=\"val\" id=\"clients\">…</span></div>\n"
    "    <div class=\"row\"><span class=\"lbl\">AP IP</span><span class=\"val\" id=\"ip\">…</span></div>\n"
    "    <div class=\"row\"><span class=\"lbl\">Uptime</span><span class=\"val\" id=\"uptime\">…</span></div>\n"
    "  </div>\n"
    "  <script>\n"
    "    function refresh(){\n"
    "      fetch('/api/status').then(r=>r.json()).then(s=>{\n"
    "        document.getElementById('bt').innerHTML=\n"
    "          (s.bt_connected?'<span class=\"dot on\"></span>Connected'\n"
    "          :'<span class=\"dot off\"></span>Disconnected');\n"
    "        document.getElementById('clients').textContent=s.wifi_clients;\n"
    "        document.getElementById('ip').textContent=s.ip;\n"
    "        var u=s.uptime,m=Math.floor(u/60),h=Math.floor(m/60);\n"
    "        document.getElementById('uptime').textContent=\n"
    "          h+'h '+(m%60)+'m '+(u%60)+'s';\n"
    "      });\n"
    "    }\n"
    "    refresh();setInterval(refresh,5000);\n"
    "  </script>\n"
    "</body>\n"
    "</html>\n";

static esp_err_t root_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, ROOT_HTML, sizeof(ROOT_HTML) - 1);
}

/* ── GET /api/status ───────────────────────────────────────────────── */

static esp_err_t status_get_handler(httpd_req_t *req)
{
    /* gather status */
    bool bt_connected = bt_manager_is_connected();

    int wifi_clients = wifi_ap_get_connected_clients();
    if (wifi_clients < 0) { wifi_clients = 0; }

    int64_t uptime_sec = (esp_timer_get_time() - g_start_time) / 1000000;

    char ip_str[16] = "0.0.0.0";
    struct netif *ap_netif = wifi_ap_get_netif();
    if (ap_netif != NULL) {
        const ip4_addr_t *addr = netif_ip4_addr(ap_netif);
        if (addr != NULL) {
            ip4addr_ntoa_r(addr, ip_str, sizeof(ip_str));
        }
    }

    /* build JSON */
    char json[256];
    int len = snprintf(json, sizeof(json),
        "{"
          "\"bt_connected\":%s,"
          "\"wifi_clients\":%d,"
          "\"uptime\":%lld,"
          "\"ip\":\"%s\""
        "}",
        bt_connected ? "true" : "false",
        wifi_clients,
        uptime_sec,
        ip_str);

    if (len < 0 || (size_t)len >= sizeof(json)) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, len);
}

/* ── Request body reader ─────────────────────────────────────────────── */

/**
 * @brief Read the entire request body into a NUL-terminated buffer.
 *
 * httpd_req_recv() may return short reads; a single call only guarantees
 * up to one TCP segment. Loop until Content-Length bytes are consumed.
 *
 * @return Number of bytes read (>= 0), or -1 on socket error or if the
 *         body does not fit in @p buf_size - 1.
 */
static int httpd_read_full_body(httpd_req_t *req, char *buf, size_t buf_size)
{
    size_t total = 0;
    size_t want = (size_t)req->content_len;

    while (total < want) {
        if (total >= buf_size - 1) {
            return -1;  /* body larger than buffer */
        }
        int r = httpd_req_recv(req, buf + total, buf_size - 1 - total);
        if (r <= 0) {
            return -1;  /* socket error or connection closed */
        }
        total += (size_t)r;
    }
    buf[total] = '\0';
    return (int)total;
}

/* ── Hand-rolled JSON parser helpers ─────────────────────────────────── */

/**
 * @brief  Extract a quoted string value for the given key from a JSON buffer.
 *
 * Scans for \"key\" then the next \"…\" value.  No recursion, no escape
 * handling — sufficient for the flat payloads this firmware receives.
 *
 * @return true if found and copied to out (truncated if needed), false otherwise.
 */
static bool json_get_string(const char *json, const char *key, char *out, size_t out_len)
{
    if (!json || !key || !out || out_len == 0) return false;

    /* build search pattern: \"key\" */
    char pattern[64];
    int plen = snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    if (plen < 0 || (size_t)plen >= sizeof(pattern)) return false;

    const char *pos = strstr(json, pattern);
    if (!pos) return false;

    /* skip past \"key\" and find the colon */
    pos += plen;
    while (*pos == ' ' || *pos == '\t') pos++;
    if (*pos != ':') return false;
    pos++;
    while (*pos == ' ' || *pos == '\t') pos++;
    if (*pos != '"') return false;
    pos++;  /* skip opening quote */

    /* copy until closing quote or end; handle backslash escapes so an
     * escaped quote (\") doesn't terminate the value early */
    size_t i = 0;
    while (*pos && *pos != '"' && i < out_len - 1) {
        if (*pos == '\\' && pos[1] != '\0') {
            pos++;  /* skip backslash, copy the escaped char literally */
        }
        out[i++] = *pos++;
    }
    out[i] = '\0';
    return true;
}

/**
 * @brief  Extract an integer value for the given key from a JSON buffer.
 * @return true if found and parsed, false otherwise.
 */
static bool json_get_int(const char *json, const char *key, int *out)
{
    if (!json || !key || !out) return false;

    /* build search pattern: \"key\" */
    char pattern[64];
    int plen = snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    if (plen < 0 || (size_t)plen >= sizeof(pattern)) return false;

    const char *pos = strstr(json, pattern);
    if (!pos) return false;

    pos += plen;
    while (*pos == ' ' || *pos == '\t') pos++;
    if (*pos != ':') return false;
    pos++;
    while (*pos == ' ' || *pos == '\t') pos++;

    /* parse sign + digits */
    int sign = 1;
    if (*pos == '-') { sign = -1; pos++; }
    if (*pos < '0' || *pos > '9') return false;

    int val = 0;
    while (*pos >= '0' && *pos <= '9') {
        val = val * 10 + (*pos - '0');
        pos++;
    }
    *out = sign * val;
    return true;
}

/**
 * @brief  Extract a boolean value (true/false) for the given key.
 * @return true if found, false otherwise.  *out is set on success.
 */
static bool json_get_bool(const char *json, const char *key, bool *out)
{
    if (!json || !key || !out) return false;

    char pattern[64];
    int plen = snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    if (plen < 0 || (size_t)plen >= sizeof(pattern)) return false;

    const char *pos = strstr(json, pattern);
    if (!pos) return false;

    pos += plen;
    while (*pos == ' ' || *pos == '\t') pos++;
    if (*pos != ':') return false;
    pos++;
    while (*pos == ' ' || *pos == '\t') pos++;

    if (strncmp(pos, "true", 4) == 0) { *out = true;  return true; }
    if (strncmp(pos, "false", 5) == 0) { *out = false; return true; }
    return false;
}

/**
 * @brief  Parse a colon-separated BD address string ("AA:BB:CC:DD:EE:FF")
 *         into 6 raw bytes.
 * @return true on success.
 */
static bool parse_bda(const char *str, esp_bd_addr_t out)
{
    if (!str || !out) return false;

    unsigned int b[6];
    if (sscanf(str, "%02x:%02x:%02x:%02x:%02x:%02x",
               &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) != 6) {
        /* also try without colons */
        if (sscanf(str, "%02x%02x%02x%02x%02x%02x",
                   &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) != 6) {
            return false;
        }
    }

    for (int i = 0; i < 6; i++) {
        out[i] = (uint8_t)b[i];
    }
    return true;
}

/* ── GET /api/wifi ──────────────────────────────────────────────────── */

static esp_err_t wifi_get_handler(httpd_req_t *req)
{
    bridge_ap_config_t cfg;
    esp_err_t ret = wifi_ap_get_config(&cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "wifi_ap_get_config failed: %s", esp_err_to_name(ret));
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    int clients = wifi_ap_get_connected_clients();
    if (clients < 0) clients = 0;

    char json[512];
    int len = snprintf(json, sizeof(json),
        "{"
          "\"ssid\":\"%.*s\","
          "\"password\":\"%.*s\","
          "\"channel\":%u,"
          "\"max_connections\":%u,"
          "\"ssid_hidden\":%s,"
          "\"connected_clients\":%d"
        "}",
        WIFI_AP_SSID_MAX_LEN,     cfg.ssid,
        WIFI_AP_PASSWORD_MAX_LEN, cfg.password,
        cfg.channel,
        cfg.max_connections,
        cfg.ssid_hidden ? "true" : "false",
        clients);

    if (len < 0 || (size_t)len >= sizeof(json)) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, len);
}

/* ── POST /api/wifi ─────────────────────────────────────────────────── */

static esp_err_t wifi_post_handler(httpd_req_t *req)
{
    char body[512];
    int total = httpd_read_full_body(req, body, sizeof(body));
    if (total <= 0) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, NULL, 0);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "POST /api/wifi body: %s", body);

    /* load existing config then overlay any fields present in the JSON */
    bridge_ap_config_t cfg;
    esp_err_t ret = wifi_ap_get_config(&cfg);
    if (ret != ESP_OK) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    char buf[WIFI_AP_PASSWORD_MAX_LEN + 1];
    int ival;
    bool bval;

    if (json_get_string(body, "ssid", buf, sizeof(buf))) {
        strncpy(cfg.ssid, buf, sizeof(cfg.ssid) - 1);
        cfg.ssid[sizeof(cfg.ssid) - 1] = '\0';
    }
    if (json_get_string(body, "password", buf, sizeof(buf))) {
        strncpy(cfg.password, buf, sizeof(cfg.password) - 1);
        cfg.password[sizeof(cfg.password) - 1] = '\0';
    }
    if (json_get_int(body, "channel", &ival)) {
        if (ival >= 1 && ival <= 13)
            cfg.channel = (uint8_t)ival;
    }
    if (json_get_int(body, "max_connections", &ival)) {
        if (ival >= 1 && ival <= 10)
            cfg.max_connections = (uint8_t)ival;
    }
    if (json_get_bool(body, "ssid_hidden", &bval)) {
        cfg.ssid_hidden = bval;
    }

    /* persist */
    ret = wifi_ap_set_config(&cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "wifi_ap_set_config failed: %s", esp_err_to_name(ret));
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "WiFi config saved – SSID: %s, chan: %u, max_clients: %u",
             cfg.ssid, cfg.channel, cfg.max_connections);

    /* respond with the updated config */
    int clients = wifi_ap_get_connected_clients();
    if (clients < 0) clients = 0;

    char json[512];
    int len = snprintf(json, sizeof(json),
        "{\"ok\":true,\"ssid\":\"%.*s\",\"password\":\"%.*s\",\"channel\":%u,\"max_connections\":%u,\"ssid_hidden\":%s,\"connected_clients\":%d}",
        WIFI_AP_SSID_MAX_LEN,     cfg.ssid,
        WIFI_AP_PASSWORD_MAX_LEN, cfg.password,
        cfg.channel,
        cfg.max_connections,
        cfg.ssid_hidden ? "true" : "false",
        clients);

    if (len < 0 || (size_t)len >= sizeof(json)) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, len);
}

/* ── JSON string escaping ────────────────────────────────────────────── */

/**
 * @brief  Write a JSON-escaped string into a buffer.
 *
 * Escapes " \ / \b \f \n \r \t and control characters (0x00–0x1F).
 *
 * @param out     Destination buffer.
 * @param max     Size of @p out.
 * @param src     Source string (may be NULL).
 * @param src_len Length of source string to write (use SIZE_MAX for all).
 * @return Number of characters written (excluding NUL), or -1 on truncation.
 */
static int json_escape_string(char *out, size_t max, const char *src, size_t src_len)
{
    if (!out || max == 0) return 0;
    if (!src) {
        if (max >= 1) { out[0] = '\0'; }
        return 0;
    }

    size_t n = 0;

    for (size_t i = 0; i < src_len && src[i] != '\0'; i++) {
        char c = src[i];
        const char *esc = NULL;

        switch (c) {
        case '"':  esc = "\\\"";  break;
        case '\\': esc = "\\\\";  break;
        case '\b': esc = "\\b";   break;
        case '\f': esc = "\\f";   break;
        case '\n': esc = "\\n";   break;
        case '\r': esc = "\\r";   break;
        case '\t': esc = "\\t";   break;
        default:
            if ((unsigned char)c < 0x20) {
                /* control char → \\u00XX */
                if (n + 7 > max) goto trunc;
                int w2 = snprintf(out + n, max - n, "\\u%04X", (unsigned char)c);
                if (w2 < 0 || (size_t)w2 >= max - n) goto trunc;
                n += w2;
                continue;
            }
            /* printable — copy as-is */
            if (n + 2 > max) goto trunc;
            out[n++] = c;
            continue;
        }

        /* write escape sequence */
        size_t esc_len = strlen(esc);
        if (n + esc_len + 1 > max) goto trunc;
        memcpy(out + n, esc, esc_len);
        n += esc_len;
    }

    out[n] = '\0';
    return (int)n;

trunc:
    out[n] = '\0';
    return -1;
}

/* ── GET /api/bt/scan ───────────────────────────────────────────────── */

/* Background task: start BT discovery on its own stack to avoid
 * overflowing the small HTTPD task stack. */
static void scan_task(void *arg)
{
    /* Stop any active/retrying PAN connection first — BT inquiry cannot
     * run reliably while the controller is paging a remote device. */
    if (!pan_manager_is_online()) {
        pan_manager_disconnect();
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    bt_manager_start_discovery(10);
    vTaskDelete(NULL);
}
/* Background task: initiate PAN connection via TinyPAN on its own stack */
static void connect_task(void *arg)
{
    esp_bd_addr_t *bda = (esp_bd_addr_t *)arg;
    pan_manager_connect(*bda);
    free(bda);
    vTaskDelete(NULL);
}


static esp_err_t bt_scan_get_handler(httpd_req_t *req)
{
    /* Check BT is initialised first */
    if (!bt_manager_is_initialised()) {
        const char *err = "{\"ok\":false,\"error\":\"Bluetooth not initialised\"}";
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_send(req, err, strlen(err));
        return ESP_FAIL;
    }

    bt_state_t state = bt_state_machine_get_state();

    /* Don't start discovery if we're connected — that's a different operation */
    if (state == BT_STATE_CONNECTED) {
        const char *err = "{\"ok\":false,\"error\":\"Already connected — disconnect first\"}";
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_send(req, err, strlen(err));
        return ESP_FAIL;
    }

    /* If already discovering, return accepted immediately */
    if (state == BT_STATE_DISCOVERING) {
        const char *ok = "{\"ok\":true,\"status\":\"already_scanning\"}";
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_status(req, "202 Accepted");
        httpd_resp_send(req, ok, strlen(ok));
        return ESP_OK;
    }

    /* Start discovery in a background task (avoids stack overflow
     * in the HTTPD task from deep BT GAP callbacks). */
    if (xTaskCreate(scan_task, "bt_scan", 3072, NULL,
                    tskIDLE_PRIORITY + 3, NULL) != pdPASS) {
        const char *err = "{\"ok\":false,\"error\":\"Task creation failed\"}";
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_send(req, err, strlen(err));
        return ESP_FAIL;
    }

    /* Discovery started successfully — return 202 Accepted */
    const char *ok = "{\"ok\":true,\"status\":\"scanning\"}";
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_status(req, "202 Accepted");
    httpd_resp_send(req, ok, strlen(ok));
    return ESP_OK;
}

/* ── GET /api/bt/scan/results ────────────────────────────────────────── */

static esp_err_t bt_scan_results_get_handler(httpd_req_t *req)
{
    if (!bt_manager_is_initialised()) {
        const char *err = "{\"ok\":false,\"error\":\"Bluetooth not initialised\"}";
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_send(req, err, strlen(err));
        return ESP_FAIL;
    }

    bt_state_t state = bt_state_machine_get_state();
    /* Scan is "active" while inquiry runs AND while remote names are
     * still being resolved — the list isn't final until both finish. */
    bool active = (state == BT_STATE_DISCOVERING) ||
                  bt_manager_is_resolving_names();

    const bt_discovered_device_t *devs = NULL;
    size_t count = 0;
    bt_manager_get_discovered_devices(&devs, &count);

    /* Sort indices by RSSI descending so the strongest (closest)
     * devices are listed first. */
    static size_t order[BT_DISCOVERY_MAX_DEVICES];
    if (count > BT_DISCOVERY_MAX_DEVICES) count = BT_DISCOVERY_MAX_DEVICES;
    for (size_t i = 0; i < count; i++) order[i] = i;
    for (size_t i = 1; i < count; i++) {
        size_t key = order[i];
        size_t j = i;
        while (j > 0 && devs[order[j - 1]].rssi < devs[key].rssi) {
            order[j] = order[j - 1];
            j--;
        }
        order[j] = key;
    }

    /* Build JSON on the heap — 32 devices × ~110 B each needs ~4 KB,
     * far too large for the HTTPD task stack. */
    const size_t json_cap = 4096;
    char *json = malloc(json_cap);
    if (json == NULL) {
        const char *err2 = "{\"ok\":false,\"error\":\"Out of memory\"}";
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, err2, strlen(err2));
        return ESP_FAIL;
    }

    int offset = 0;
    int remain = (int)json_cap;

    /* Opening */
    int w = snprintf(json + offset, remain,
                     "{\"ok\":true,\"devices\":[");
    if (w < 0 || w >= remain) goto results_overflow;
    offset += w; remain -= w;

    for (size_t n = 0; n < count && remain > 10; n++) {
        size_t i = order[n];

        /* BDA string */
        char bda_str[18];
        snprintf(bda_str, sizeof(bda_str), "%02X:%02X:%02X:%02X:%02X:%02X",
                 devs[i].bda[0], devs[i].bda[1], devs[i].bda[2],
                 devs[i].bda[3], devs[i].bda[4], devs[i].bda[5]);

        /* JSON-escape the device name */
        static char esc_name[BT_DISCOVERY_NAME_MAX * 2];
        json_escape_string(esc_name, sizeof(esc_name),
                           devs[i].name, BT_DISCOVERY_NAME_MAX);

        w = snprintf(json + offset, remain,
                      "%s{\"name\":\"%s\",\"bda\":\"%s\",\"rssi\":%d,\"cod\":%lu}",
                      (n > 0) ? "," : "",
                      esc_name,
                      bda_str, devs[i].rssi,
                      (unsigned long)devs[i].cod);
        if (w < 0 || w >= remain) goto results_overflow;
        offset += w; remain -= w;
    }

    /* Closing — include active flag so client knows if scan is still running */
    w = snprintf(json + offset, remain,
                 "],\"active\":%s,\"completed\":%s}",
                 active ? "true" : "false",
                 active ? "false" : "true");
    if (w < 0 || w >= remain) goto results_overflow;
    offset += w;

    httpd_resp_set_type(req, "application/json");
    esp_err_t send_ret = httpd_resp_send(req, json, offset);
    free(json);
    return send_ret;

results_overflow:
    /* fallback: return minimal error JSON instead of 500 */
    free(json);
    const char *err2 = "{\"ok\":false,\"error\":\"Response too large — too many devices\"}";
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, err2, strlen(err2));
    return ESP_FAIL;
}

/* ── GET /api/bt/status ─────────────────────────────────────────────── */

static esp_err_t bt_status_get_handler(httpd_req_t *req)
{
    bt_state_t state = bt_state_machine_get_state();
    bool connected = bt_manager_is_connected();

    char peer_bda_str[18] = "";
    char peer_name[BT_DISCOVERY_NAME_MAX] = "";

    if (connected) {
        esp_bd_addr_t bda;
        if (bt_manager_get_peer_bda(bda) == ESP_OK) {
            snprintf(peer_bda_str, sizeof(peer_bda_str),
                     "%02X:%02X:%02X:%02X:%02X:%02X",
                     bda[0], bda[1], bda[2], bda[3], bda[4], bda[5]);
        }

        /* look up bonded name from NVS */
        bt_bonded_device_t bonded;
        if (bt_manager_load_bonded_device(&bonded) == ESP_OK) {
            strncpy(peer_name, bonded.name, sizeof(peer_name) - 1);
        }
    }

    bool online = pan_manager_is_online();

    char json[512];
    int len = snprintf(json, sizeof(json),
        "{"
          "\"state\":\"%s\","
          "\"connected\":%s,"
          "\"peer_bda\":\"%s\","
          "\"peer_name\":\"%.*s\","
          "\"pan_online\":%s,"
          "\"state_string\":\"%s\""
        "}",
        bt_state_to_string(state),
        connected ? "true" : "false",
        peer_bda_str,
        BT_DISCOVERY_NAME_MAX - 1, peer_name,
        online ? "true" : "false",
        pan_manager_get_state_string());

    if (len < 0 || (size_t)len >= sizeof(json)) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, len);
}

/* ── POST /api/bt/connect ───────────────────────────────────────────── */

static esp_err_t bt_connect_post_handler(httpd_req_t *req)
{
    char body[256];
    int total = httpd_read_full_body(req, body, sizeof(body));
    if (total <= 0) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, NULL, 0);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "POST /api/bt/connect body: %s", body);

    /* extract BDA */
    char bda_str[24];
    if (!json_get_string(body, "bda", bda_str, sizeof(bda_str))) {
        const char *err = "{\"ok\":false,\"error\":\"missing 'bda' field\"}";
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, err, strlen(err));
        return ESP_FAIL;
    }

    esp_bd_addr_t bda;
    if (!parse_bda(bda_str, bda)) {
        const char *err = "{\"ok\":false,\"error\":\"invalid BD address format\"}";
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, err, strlen(err));
        return ESP_FAIL;
    }

    /* Initiate connect in background task (avoids HTTPD stack overflow) */
    esp_bd_addr_t *bda_copy = malloc(sizeof(esp_bd_addr_t));
    if (!bda_copy) {
        const char *err = "{\"ok\":false,\"error\":\"Out of memory\"}";
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, err, strlen(err));
        return ESP_FAIL;
    }
    memcpy(bda_copy, bda, sizeof(esp_bd_addr_t));

    if (xTaskCreate(connect_task, "bt_conn", 3072, bda_copy,
                    tskIDLE_PRIORITY + 3, NULL) != pdPASS) {
        free(bda_copy);
        const char *err = "{\"ok\":false,\"error\":\"Task creation failed\"}";
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_send(req, err, strlen(err));
        return ESP_FAIL;
    }

    const char *resp = "{\"ok\":true}";
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp, strlen(resp));
    return ESP_OK;
}

/* ── POST /api/bt/disconnect ────────────────────────────────────────── */

static esp_err_t bt_disconnect_post_handler(httpd_req_t *req)
{
    esp_err_t ret = bt_manager_disconnect();
    if (ret != ESP_OK) {
        char err[128];
        int elen = snprintf(err, sizeof(err),
                            "{\"ok\":false,\"error\":\"%s\"}", esp_err_to_name(ret));
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_send(req, err, elen);
        return ESP_FAIL;
    }

    const char *resp = "{\"ok\":true}";
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp, strlen(resp));
    return ESP_OK;
}

/* ── SPIFFS static file handler (GET /<path>) ────────────────────────── */

static esp_err_t spiffs_get_handler(httpd_req_t *req)
{
    /*
     * Build the real path under /www.  req->uri includes the leading /,
     * so e.g. "/images/logo.png" → "/www/images/logo.png".
     */
    char filepath[256];
    const char *uri = req->uri;

    /* Reject path traversal: raw ".." and percent-encoded variants */
    if (strstr(uri, "..") != NULL ||
        strstr(uri, "%2e") != NULL ||
        strstr(uri, "%2E") != NULL) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, NULL, 0);
        return ESP_FAIL;
    }

    /* Serve index.html for directory paths */
    char uri_buf[CONFIG_HTTPD_MAX_URI_LEN + 16];
    size_t uri_len = strlen(uri);
    if (uri_len > 0 && uri[uri_len - 1] == '/') {
        snprintf(uri_buf, sizeof(uri_buf), "%sindex.html", uri);
        uri = uri_buf;
    }

    int n = snprintf(filepath, sizeof(filepath), "/www%s", uri);
    if (n < 0 || (size_t)n >= sizeof(filepath)) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    /* stat the file */
    struct stat st;
    if (stat(filepath, &st) != 0 || S_ISDIR(st.st_mode)) {
        httpd_resp_send_404(req);
        return ESP_FAIL;
    }

    /* MIME type from extension (use modified uri, not req->uri) */
    const char *type = "text/plain";
    const char *dot = strrchr(uri, '.');
    if (dot) {
        if      (strcasecmp(dot, ".html") == 0 || strcasecmp(dot, ".htm") == 0) type = "text/html";
        else if (strcasecmp(dot, ".css")  == 0) type = "text/css";
        else if (strcasecmp(dot, ".js")   == 0) type = "application/javascript";
        else if (strcasecmp(dot, ".json") == 0) type = "application/json";
        else if (strcasecmp(dot, ".png")  == 0) type = "image/png";
        else if (strcasecmp(dot, ".jpg")  == 0 || strcasecmp(dot, ".jpeg") == 0) type = "image/jpeg";
        else if (strcasecmp(dot, ".svg")  == 0) type = "image/svg+xml";
        else if (strcasecmp(dot, ".ico")  == 0) type = "image/x-icon";
    }

    FILE *f = fopen(filepath, "rb");
    if (f == NULL) {
        httpd_resp_send_404(req);
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, type);

    /* stream in 512-byte chunks */
    char buf[512];
    size_t r;
    while ((r = fread(buf, 1, sizeof(buf), f)) > 0) {
        if (httpd_resp_send_chunk(req, buf, r) != ESP_OK) {
            fclose(f);
            return ESP_FAIL;
        }
    }
    httpd_resp_send_chunk(req, NULL, 0);   /* end-of-chunks marker */
    fclose(f);
    return ESP_OK;
}

/* ── Public API ─────────────────────────────────────────────────────── */

esp_err_t http_server_init(void)
{
    /*
     * Mount SPIFFS first.  Failure is non-fatal — the static file
     * handler will just 404 on every request.
     */
    mount_spiffs();

    /* ── Start server ──────────────────────────────────────────── */
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port       = 80;
    config.max_uri_handlers  = 16;   /* default 8 — we register 9+ */
    config.uri_match_fn      = httpd_uri_match_wildcard;

    esp_err_t ret = httpd_start(&g_server, &config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* ── Register URI handlers ─────────────────────────────────── */
    /* IMPORTANT: register exact-path API handlers FIRST so they
     * match before the catch-all SPIFFS wildcard below.  With
     * httpd_uri_match_wildcard the first matching handler wins,
     * and the "*" wildcard matches everything — registering it
     * last ensures API endpoints are tried before falling through
     * to SPIFFS. */

    const httpd_uri_t status_uri = {
        .uri      = "/api/status",
        .method   = HTTP_GET,
        .handler  = status_get_handler,
        .user_ctx = NULL,
    };
    httpd_register_uri_handler(g_server, &status_uri);

    /* ── WiFi API ─────────────────────────────────────────────── */
    const httpd_uri_t wifi_get_uri = {
        .uri      = "/api/wifi",
        .method   = HTTP_GET,
        .handler  = wifi_get_handler,
        .user_ctx = NULL,
    };
    httpd_register_uri_handler(g_server, &wifi_get_uri);

    const httpd_uri_t wifi_post_uri = {
        .uri      = "/api/wifi",
        .method   = HTTP_POST,
        .handler  = wifi_post_handler,
        .user_ctx = NULL,
    };
    httpd_register_uri_handler(g_server, &wifi_post_uri);

    /* ── BT API ───────────────────────────────────────────────── */
    const httpd_uri_t bt_scan_uri = {
        .uri      = "/api/bt/scan",
        .method   = HTTP_GET,
        .handler  = bt_scan_get_handler,
        .user_ctx = NULL,
    };
    httpd_register_uri_handler(g_server, &bt_scan_uri);

    const httpd_uri_t bt_scan_results_uri = {
        .uri      = "/api/bt/scan/results",
        .method   = HTTP_GET,
        .handler  = bt_scan_results_get_handler,
        .user_ctx = NULL,
    };
    httpd_register_uri_handler(g_server, &bt_scan_results_uri);

    const httpd_uri_t bt_status_uri = {
        .uri      = "/api/bt/status",
        .method   = HTTP_GET,
        .handler  = bt_status_get_handler,
        .user_ctx = NULL,
    };
    httpd_register_uri_handler(g_server, &bt_status_uri);

    const httpd_uri_t bt_connect_uri = {
        .uri      = "/api/bt/connect",
        .method   = HTTP_POST,
        .handler  = bt_connect_post_handler,
        .user_ctx = NULL,
    };
    httpd_register_uri_handler(g_server, &bt_connect_uri);

    const httpd_uri_t bt_disconnect_uri = {
        .uri      = "/api/bt/disconnect",
        .method   = HTTP_POST,
        .handler  = bt_disconnect_post_handler,
        .user_ctx = NULL,
    };
    httpd_register_uri_handler(g_server, &bt_disconnect_uri);

    /* ── SPIFFS catch-all (after API, before fallback root) ───── */
    const httpd_uri_t spiffs_uri = {
        .uri      = "/*",
        .method   = HTTP_GET,
        .handler  = spiffs_get_handler,
        .user_ctx = NULL,
    };
    httpd_register_uri_handler(g_server, &spiffs_uri);

    /* Fallback root handler — only hit if SPIFFS mount failed
     * (SPIFFS wildcard above serves index.html for /) */
    const httpd_uri_t root_uri = {
        .uri      = "/",
        .method   = HTTP_GET,
        .handler  = root_get_handler,
        .user_ctx = NULL,
    };
    httpd_register_uri_handler(g_server, &root_uri);

    /* ── Record start time ─────────────────────────────────────── */
    g_start_time = esp_timer_get_time();

    ESP_LOGI(TAG, "HTTP server started on port 80");
    return ESP_OK;
}
