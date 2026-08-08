/**
 * @file console_repl.c
 * @brief UART console REPL for debugging TinyPAN bridge
 *
 * Provides a simple command-line interface over UART0 (the same port used
 * for flashing/monitoring).  Commands allow querying status, triggering
 * reconnects, and testing network paths without needing a working BT link.
 *
 * Commands:
 *   help            — list commands
 *   status          — show BT, PAN, NAT, WiFi AP state
 *   ping <ip>       — send 4 ICMP pings from ESP32 (uses raw lwIP, not NAPT)
 *   curl <url>      — HTTP GET via ESP32 (tests DNS + TCP through PAN)
 *   reconnect       — force PAN disconnect + reconnect cycle
 *   dns             — show current DNS forward server
 *   wifi            — show WiFi AP info + connected stations
 *   bt              — show BT state + bonded devices
 *   keepalive <ms>  — set BNEP keepalive interval (runtime, not persisted)
 *   qos <hex>       — set BT QoS poll interval (runtime, not persisted)
 */

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_system.h"
#include "lwip/ip4_addr.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "lwip/icmp.h"
#include "lwip/inet_chksum.h"
#include "lwip/raw.h"
#include "lwip/netif.h"
#include "lwip/dns.h"
#include "lwip/tcpip.h"

#include "console_repl.h"
#include "esp_gap_bt_api.h"
#include "pan_manager.h"
#include "wifi_ap.h"
#include "bt_manager.h"
#include "bt_state.h"
#include "nat_router.h"
#include "captive_dns.h"
#include "tinypan.h"
#include "tinypan_lwip_netif.h"

static const char *TAG = "console";

#define UART_NUM        UART_NUM_0
#define BUF_SIZE        256
#define CMD_QUEUE_LEN   4

typedef struct {
    char cmd[64];
    char arg[64];
} repl_cmd_t;

static bool s_running = false;

/* Runtime-tunable parameters (not persisted) */
static uint32_t s_runtime_keepalive_ms = 0;  /* 0 = use compile-time default */
static uint8_t  s_runtime_qos_poll = 0;      /* 0 = use compile-time default */

/* ── Helpers ─────────────────────────────────────────────────── */

static void send_str(const char *s)
{
    uart_write_bytes(UART_NUM, s, strlen(s));
}

static void send_fmt(const char *fmt, ...)
{
    char buf[256];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    send_str(buf);
}

/* ── Command Handlers ────────────────────────────────────────── */

static void cmd_help(void)
{
    send_str("\r\nCommands:\r\n");
    send_str("  help            — this list\r\n");
    send_str("  status          — BT/PAN/NAT/WiFi state\r\n");
    send_str("  ping <ip>       — 4 ICMP pings from ESP32\r\n");
    send_str("  curl <url>      — HTTP GET through PAN\r\n");
    send_str("  reconnect       — force PAN reconnect\r\n");
    send_str("  dns             — show DNS forward server\r\n");
    send_str("  wifi            — WiFi AP info + stations\r\n");
    send_str("  bt              — BT state + bonded devices\r\n");
    send_str("  keepalive <ms>  — set BNEP keepalive interval\r\n");
    send_str("  qos <hex>       — set BT QoS poll interval\r\n");
    send_str("\r\n");
}

static void cmd_status(void)
{
    send_fmt("\r\nBT state: %s\r\n", bt_state_to_string(bt_state_machine_get_state()));
    send_fmt("PAN state: %s\r\n", pan_manager_get_state_string());
    send_fmt("PAN online: %s\r\n", pan_manager_is_online() ? "yes" : "no");
    send_fmt("NAT active: %s\r\n", nat_router_is_active() ? "yes" : "no");

    tinypan_ip_info_t ip;
    if (pan_manager_get_ip_info(&ip) == ESP_OK) {
        send_fmt("PAN IP: %s\r\n", ip4addr_ntoa((const ip4_addr_t*)&ip.ip_addr));
        send_fmt("PAN GW: %s\r\n", ip4addr_ntoa((const ip4_addr_t*)&ip.gateway));
        send_fmt("PAN DNS: %s\r\n", ip4addr_ntoa((const ip4_addr_t*)&ip.dns_server));
    } else {
        send_str("PAN IP: not acquired\r\n");
    }

    int clients = wifi_ap_get_connected_clients();
    send_fmt("WiFi clients: %d\r\n", clients);
    send_str("\r\n");
}

static void cmd_ping(const char *arg)
{
    if (!arg || !*arg) {
        send_str("Usage: ping <ip>\r\n");
        return;
    }

    ip4_addr_t target;
    if (!ip4addr_aton(arg, &target)) {
        send_fmt("Invalid IP: %s\r\n", arg);
        return;
    }

    send_fmt("PING %s from ESP32 (4 packets)...\r\n", arg);

    /* Use raw ICMP via lwIP — bind to PAN netif so ICMP routes correctly
     * when multiple interfaces exist (WiFi AP + PAN). */
    struct raw_pcb *pcb = raw_new(IP_PROTO_ICMP);
    if (!pcb) {
        send_str("Failed to create raw PCB\r\n");
        return;
    }

    /* Bind to PAN netif IP to avoid routing through WiFi AP */
    struct netif *pan = tinypan_netif_get();
    if (pan && !ip4_addr_isany(netif_ip4_addr(pan))) {
        raw_bind(pcb, (const ip_addr_t *)netif_ip4_addr(pan));
    }

    struct icmp_echo_hdr hdr;
    hdr.type = ICMP_ECHO;
    hdr.code = 0;
    hdr.chksum = 0;
    hdr.id = 0x1234;
    hdr.seqno = 0;

    for (int i = 0; i < 4; i++) {
        hdr.seqno = i;
        hdr.chksum = 0;
        hdr.chksum = inet_chksum(&hdr, sizeof(hdr));

        struct pbuf *p = pbuf_alloc(PBUF_IP, sizeof(hdr), PBUF_RAM);
        if (!p) break;
        memcpy(p->payload, &hdr, sizeof(hdr));

        err_t err = raw_sendto(pcb, p, (const ip_addr_t*)&target);
        pbuf_free(p);
        if (err != ERR_OK) {
            send_fmt("  seq %d: send failed %d\r\n", i, err);
            continue;
        }

        /* Wait a bit for reply (simplified — real impl would use recv callback) */
        vTaskDelay(pdMS_TO_TICKS(500));
        /* Note: proper ICMP ping needs a recv callback or raw_recv.
         * This is a placeholder that at least exercises the TX path. */
        send_fmt("  seq %d: sent (no reply tracking yet)\r\n", i);
    }

    raw_remove(pcb);
    send_fmt("Ping complete (TX only — RX tracking TODO)\r\n\r\n");
}

static void cmd_curl(const char *arg)
{
    if (!arg || !*arg) {
        send_str("Usage: curl <url>\r\n");
        return;
    }

    send_fmt("HTTP GET %s...\r\n", arg);

    /* Strip http:// or https:// prefix for gethostbyname() */
    const char *host = arg;
    const char *path = "/";
    if (strncmp(arg, "http://", 7) == 0) {
        host = arg + 7;
    } else if (strncmp(arg, "https://", 8) == 0) {
        host = arg + 8;
    }
    /* Extract path if present */
    const char *slash = strchr(host, '/');
    char hostname[64];
    if (slash) {
        size_t hlen = slash - host;
        if (hlen >= sizeof(hostname)) hlen = sizeof(hostname) - 1;
        memcpy(hostname, host, hlen);
        hostname[hlen] = '\0';
        path = slash;
    } else {
        strncpy(hostname, host, sizeof(hostname) - 1);
        hostname[sizeof(hostname) - 1] = '\0';
    }

    /* Simple HTTP client using BSD sockets */
    struct hostent *he = gethostbyname(hostname);
    if (!he) {
        send_fmt("DNS failed for %s\r\n", hostname);
        return;
    }

    send_fmt("Resolved to %s\r\n", inet_ntoa(*(struct in_addr*)he->h_addr));

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        send_str("socket() failed\r\n");
        return;
    }

    /* Set timeouts so connect/read don't block indefinitely */
    struct timeval tv = { .tv_sec = 10, .tv_usec = 0 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    struct sockaddr_in sa = {0};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(80);
    memcpy(&sa.sin_addr, he->h_addr, 4);

    if (connect(sock, (struct sockaddr*)&sa, sizeof(sa)) < 0) {
        send_str("connect() failed\r\n");
        close(sock);
        return;
    }

    char req[256];
    snprintf(req, sizeof(req), "GET %s HTTP/1.0\r\nHost: %s\r\n\r\n", path, hostname);
    write(sock, req, strlen(req));

    char buf[512];
    int n = read(sock, buf, sizeof(buf)-1);
    if (n > 0) {
        buf[n] = '\0';
        send_fmt("Response (%d bytes):\r\n%s\r\n", n, buf);
    } else {
        send_str("No response\r\n");
    }
    close(sock);
    send_str("\r\n");
}

static void cmd_reconnect(void)
{
    send_str("Forcing PAN reconnect...\r\n");
    pan_manager_disconnect();
    vTaskDelay(pdMS_TO_TICKS(1000));
    /* Reconnect needs the phone BDA — we can't get it here without bt_manager.
     * For now just report that manual reconnect is needed. */
    send_str("Disconnect sent. Use web UI or power-cycle to reconnect.\r\n\r\n");
}

static void cmd_dns(void)
{
    /* TODO: expose captive_dns forward server getter */
    send_str("DNS forward server: query via 'status' (not directly exposed)\r\n\r\n");
}

static void cmd_wifi(void)
{
    send_str("\r\nWiFi AP:\r\n");
    send_fmt("  SSID: %s\r\n", WIFI_AP_DEFAULT_SSID);
    send_fmt("  Channel: %d\r\n", WIFI_AP_DEFAULT_CHANNEL);
    send_fmt("  Clients: %d\r\n", wifi_ap_get_connected_clients());

    struct netif *ap = wifi_ap_get_netif();
    if (ap) {
        send_fmt("  IP: %s\r\n", ip4addr_ntoa(netif_ip4_addr(ap)));
        send_fmt("  Up: %d Link: %d\r\n", netif_is_up(ap), netif_is_link_up(ap));
    }
    send_str("\r\n");
}

static void cmd_bt(void)
{
    send_str("\r\nBluetooth:\r\n");
    send_fmt("  State: %s\r\n", bt_state_to_string(bt_state_machine_get_state()));

    int bond_num = esp_bt_gap_get_bond_device_num();
    send_fmt("  Bonded devices: %d\r\n", bond_num);
    if (bond_num > 0) {
        esp_bd_addr_t list[8];
        int n = bond_num < 8 ? bond_num : 8;
        esp_bt_gap_get_bond_device_list(&n, list);
        for (int i = 0; i < n; i++) {
            send_fmt("    %02x:%02x:%02x:%02x:%02x:%02x\r\n",
                     list[i][0], list[i][1], list[i][2],
                     list[i][3], list[i][4], list[i][5]);
        }
    }
    send_str("\r\n");
}

static void cmd_keepalive(const char *arg)
{
    if (!arg || !*arg) {
        send_fmt("Current runtime keepalive: %lu ms (0 = default)\r\n\r\n",
                 (unsigned long)s_runtime_keepalive_ms);
        return;
    }
    s_runtime_keepalive_ms = atoi(arg);
    send_fmt("Runtime keepalive set to %lu ms (takes effect on next BNEP connection)\r\n\r\n",
             (unsigned long)s_runtime_keepalive_ms);
}

static void cmd_qos(const char *arg)
{
    if (!arg || !*arg) {
        send_fmt("Current runtime QoS: 0x%02x (0 = default)\r\n\r\n", s_runtime_qos_poll);
        return;
    }
    s_runtime_qos_poll = (uint8_t)strtol(arg, NULL, 16);
    send_fmt("Runtime QoS set to 0x%02x (takes effect on next L2CAP connect)\r\n\r\n",
             s_runtime_qos_poll);
}

/* ── Command Parser ──────────────────────────────────────────── */

static void process_cmd(const char *line)
{
    char cmd[32] = {0};
    char arg[64] = {0};

    /* Skip leading whitespace */
    while (*line && isspace((unsigned char)*line)) line++;
    if (!*line) return;

    /* Split command and argument */
    const char *sp = strchr(line, ' ');
    if (sp) {
        size_t clen = sp - line;
        if (clen >= sizeof(cmd)) clen = sizeof(cmd)-1;
        memcpy(cmd, line, clen);
        cmd[clen] = '\0';
        while (*sp && isspace((unsigned char)*sp)) sp++;
        strncpy(arg, sp, sizeof(arg)-1);
    } else {
        strncpy(cmd, line, sizeof(cmd)-1);
    }

    /* Dispatch */
    if      (strcasecmp(cmd, "help") == 0)      cmd_help();
    else if (strcasecmp(cmd, "status") == 0)    cmd_status();
    else if (strcasecmp(cmd, "ping") == 0)      cmd_ping(arg);
    else if (strcasecmp(cmd, "curl") == 0)      cmd_curl(arg);
    else if (strcasecmp(cmd, "reconnect") == 0) cmd_reconnect();
    else if (strcasecmp(cmd, "dns") == 0)       cmd_dns();
    else if (strcasecmp(cmd, "wifi") == 0)      cmd_wifi();
    else if (strcasecmp(cmd, "bt") == 0)        cmd_bt();
    else if (strcasecmp(cmd, "keepalive") == 0) cmd_keepalive(arg);
    else if (strcasecmp(cmd, "qos") == 0)       cmd_qos(arg);
    else send_fmt("Unknown command: %s (try 'help')\r\n\r\n", cmd);
}

/* ── UART Task ───────────────────────────────────────────────── */

static void console_task(void *arg)
{
    uint8_t data[BUF_SIZE];
    char line[128];
    size_t line_len = 0;

    send_str("\r\nTinyPAN Console REPL — type 'help' for commands\r\n");
    send_str("> ");

    while (s_running) {
        int len = uart_read_bytes(UART_NUM, data, BUF_SIZE-1, pdMS_TO_TICKS(100));
        if (len <= 0) continue;

        for (int i = 0; i < len; i++) {
            char c = (char)data[i];

            /* Echo */
            uart_write_bytes(UART_NUM, &c, 1);

            if (c == '\r' || c == '\n') {
                if (line_len > 0) {
                    line[line_len] = '\0';
                    send_str("\r\n");
                    process_cmd(line);
                    line_len = 0;
                }
                send_str("> ");
            } else if (c == '\b' || c == 0x7F) {
                if (line_len > 0) {
                    line_len--;
                    send_str(" \b");
                }
            } else if (line_len < sizeof(line)-1) {
                line[line_len++] = c;
            }
        }
    }

    vTaskDelete(NULL);
}

/* ==================================================================
 *  Public API
 * ================================================================== */

esp_err_t console_repl_start(void)
{
    if (s_running) {
        ESP_LOGW(TAG, "Console already running");
        return ESP_OK;
    }

    /* UART0 is already initialized by the bootloader for logging.
     * We just need to enable RX. */
    uart_config_t cfg = {
        .baud_rate = 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    esp_err_t ret = uart_param_config(UART_NUM, &cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "uart_param_config failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Install UART driver with RX buffer */
    ret = uart_driver_install(UART_NUM, 1024, 0, 0, NULL, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "uart_driver_install failed: %s", esp_err_to_name(ret));
        return ret;
    }

    s_running = true;
    BaseType_t xret = xTaskCreate(console_task, "console", 4096, NULL, 5, NULL);
    if (xret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create console task");
        s_running = false;
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Console REPL started on UART0");
    return ESP_OK;
}

/* Runtime accessors for BNEP transport / HAL to query */
uint32_t console_get_runtime_keepalive_ms(void) { return s_runtime_keepalive_ms; }
uint8_t  console_get_runtime_qos_poll(void)     { return s_runtime_qos_poll; }
