/**
 * @file resilience.c
 * @brief Resilience watchdog for the TinyPAN bridge.
 *
 * Addresses the "connection drops and won't recover" failure mode by:
 *
 * 1. **Upstream connectivity probing**: While the PAN link is ONLINE,
 *    periodically issues a real DNS query through the PAN interface to
 *    verify that the phone actually has internet (cellular data up).
 *    The probe uses the DNS server learned from the phone's DHCP
 *    response (not a hardcoded server) so it correctly handles VPN
 *    scenarios where the phone's system DNS changes.
 *
 *    Two failure modes are distinguished:
 *    - **BT link DEAD** (no recent BNEP RX): Reconnect aggressively
 *      after 3 failures (60s). The link is already gone.
 *    - **BT link ALIVE** (recent BNEP keepalive RX): The phone lost
 *      cellular data (5G dead spot, VPN transition) but the BT link
 *      is healthy. Wait 5 minutes (15 failures) before forcing a
 *      reconnect, since the phone will likely auto-recover cellular
 *      and a BT reconnect takes 20-30s of zero connectivity.
 *
 * 2. **DNS forward server refresh**: Periodically re-reads the PAN
 *    DHCP DNS lease and updates the captive DNS forward target. This
 *    handles VPN connect/disconnect on the phone that changes the
 *    system DNS server (e.g., Tailscale, WireGuard).
 *
 * 3. **System reboot fallback**: If the system has been continuously
 *    offline (no PAN IP) for RESILIENCE_OFFLINE_REBOOT_MS, reboots the
 *    ESP32. This is the ultimate recovery for stuck BT stacks, leaked
 *    resources, or other unrecoverable states.
 *
 * 4. **NAT/route health verification**: Periodically checks that the
 *    lwIP default route still points to the PAN netif and that NAPT is
 *    still enabled on the AP netif. lwIP can internally clear these
 *    under certain error conditions; if missing, re-enables them.
 *
 * All probing uses a separate UDP socket bound to the PAN netif's IP
 * so the query actually routes through Bluetooth, not the AP interface.
 */

#include "resilience.h"

#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"

#include "pan_manager.h"
#include "nat_router.h"
#include "wifi_ap.h"
#include "captive_dns.h"
#include "tinypan.h"
#include "tinypan_hal.h"       /* hal_bt_is_connected, hal_bt_get_last_rx_ms */
#include "tinypan_internal.h"  /* tinypan_internal_get_config() */

/* lwIP headers for NAT/route health check */
#include "lwip/netif.h"
#include "lwip/lwip_napt.h"
#include "lwip/ip4_addr.h"

static const char *TAG = "resilience";

/* ── Internal state ────────────────────────────────────────────── */

static bool     s_initialized      = false;
static bool     s_was_online       = false;
static uint32_t s_offline_since_ms = 0;     /* 0 = not tracking */
static uint32_t s_last_probe_ms    = 0;
static uint32_t s_last_dns_refresh_ms = 0;  /* DNS forward server refresh */
static int      s_probe_failures   = 0;
static bool     s_reconnect_in_progress = false;

/* ── Helpers ───────────────────────────────────────────────────── */

static uint32_t now_ms(void)
{
    return xTaskGetTickCount() * portTICK_PERIOD_MS;
}

/* ── Upstream DNS probe ────────────────────────────────────────── */

/**
 * Build a minimal DNS query for the given domain name.
 * Returns the number of bytes written into buf, or 0 on error.
 */
static uint16_t build_dns_query(uint8_t *buf, uint16_t bufsize,
                                const char *domain, uint16_t txid)
{
    if (bufsize < 512 || !domain || !buf) {
        return 0;
    }

    uint16_t offset = 0;

    /* Header (12 bytes) */
    buf[offset++] = (uint8_t)(txid >> 8);
    buf[offset++] = (uint8_t)(txid & 0xFF);
    buf[offset++] = 0x01;  /* RD=1 (recursion desired) */
    buf[offset++] = 0x00;
    buf[offset++] = 0x00;  /* QDCOUNT = 1 (high) */
    buf[offset++] = 0x01;  /* QDCOUNT = 1 (low) */
    buf[offset++] = 0x00;  /* ANCOUNT = 0 */
    buf[offset++] = 0x00;
    buf[offset++] = 0x00;  /* NSCOUNT = 0 */
    buf[offset++] = 0x00;
    buf[offset++] = 0x00;  /* ARCOUNT = 0 */
    buf[offset++] = 0x00;

    /* Encode domain name as DNS labels */
    const char *p = domain;
    while (*p) {
        const char *dot = strchr(p, '.');
        uint8_t label_len;
        if (dot) {
            label_len = (uint8_t)(dot - p);
        } else {
            label_len = (uint8_t)strlen(p);
        }
        if (label_len == 0 || label_len > 63 || offset + 1 + label_len >= bufsize) {
            return 0;
        }
        buf[offset++] = label_len;
        memcpy(buf + offset, p, label_len);
        offset += label_len;
        if (!dot) {
            break;
        }
        p = dot + 1;
    }
    buf[offset++] = 0x00;  /* null terminator */

    /* QTYPE = A (1), QCLASS = IN (1) */
    if (offset + 4 > bufsize) {
        return 0;
    }
    buf[offset++] = 0x00;
    buf[offset++] = 0x01;  /* QTYPE = A */
    buf[offset++] = 0x00;
    buf[offset++] = 0x01;  /* QCLASS = IN */

    return offset;
}

/**
 * Send a DNS query through the PAN interface and wait for a reply.
 *
 * Uses the DNS server learned from the phone's DHCP response
 * (ip_info.dns_server) so the probe tests the actual path that
 * clients use. This correctly handles VPN scenarios where the phone's
 * system DNS changes (e.g., Tailscale sets 100.100.100.100) — probing
 * 8.8.8.8 through a full-tunnel VPN would fail even though connectivity
 * is fine. Falls back to 8.8.8.8 if the DHCP DNS is 0.0.0.0.
 *
 * Returns true if a valid DNS response was received.
 */
static bool probe_upstream_dns(void)
{
    tinypan_ip_info_t ip_info;
    if (pan_manager_get_ip_info(&ip_info) != ESP_OK) {
        ESP_LOGD(TAG, "probe: no PAN IP info");
        return false;
    }

    /* Use the phone's DHCP-provided DNS server for probing. This is the
     * same server that captive_dns forwards client queries to, so a
     * successful probe guarantees clients can resolve too. */
    uint32_t dns_server_h = ip_info.dns_server;
    if (dns_server_h == 0) {
        ESP_LOGD(TAG, "probe: DHCP DNS is 0.0.0.0 — using fallback 8.8.8.8");
        dns_server_h = RESILIENCE_PROBE_DNS_FALLBACK;
    }

    /* Bind the socket to the PAN interface IP so the query routes
     * through Bluetooth, not the AP. This ensures we're actually
     * testing the upstream path, not a local loopback. */
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGW(TAG, "probe: socket() failed errno=%d", errno);
        return false;
    }

    /* Bind to PAN IP */
    struct sockaddr_in bind_addr;
    memset(&bind_addr, 0, sizeof(bind_addr));
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_addr.s_addr = ip_info.ip_addr;  /* already network order */
    bind_addr.sin_port = 0;  /* ephemeral source port */
    if (bind(sock, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0) {
        ESP_LOGW(TAG, "probe: bind() failed errno=%d", errno);
        close(sock);
        return false;
    }

    /* Set receive timeout */
    struct timeval tv = {
        .tv_sec  = RESILIENCE_PROBE_TIMEOUT_MS / 1000,
        .tv_usec = (RESILIENCE_PROBE_TIMEOUT_MS % 1000) * 1000,
    };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    /* Build and send DNS query */
    uint8_t query[512];
    uint16_t txid = (uint16_t)(now_ms() & 0xFFFF);
    if (txid == 0) txid = 1;
    uint16_t qlen = build_dns_query(query, sizeof(query),
                                    RESILIENCE_PROBE_DOMAIN, txid);
    if (qlen == 0) {
        close(sock);
        return false;
    }

    struct sockaddr_in dst;
    memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET;
    dst.sin_port = htons(53);
    dst.sin_addr.s_addr = dns_server_h;  /* already network order */

    int sent = sendto(sock, query, qlen, 0,
                      (struct sockaddr *)&dst, sizeof(dst));
    if (sent <= 0) {
        ESP_LOGD(TAG, "probe: sendto failed errno=%d", errno);
        close(sock);
        return false;
    }

    /* Wait for response */
    uint8_t reply[512];
    struct sockaddr_in from;
    socklen_t from_len = sizeof(from);
    int n = recvfrom(sock, reply, sizeof(reply), 0,
                     (struct sockaddr *)&from, &from_len);
    close(sock);

    if (n < 12) {
        ESP_LOGD(TAG, "probe: no reply (n=%d errno=%d)", n, errno);
        return false;
    }

    /* Verify transaction ID matches */
    uint16_t reply_txid = ((uint16_t)reply[0] << 8) | reply[1];
    if (reply_txid != txid) {
        ESP_LOGD(TAG, "probe: txid mismatch 0x%04X != 0x%04X", reply_txid, txid);
        return false;
    }

    /* Check DNS RCODE (bytes 3, bits 0-3). RCODE 0 = NOERROR */
    uint8_t rcode = reply[3] & 0x0F;
    if (rcode != 0) {
        ESP_LOGD(TAG, "probe: DNS RCODE=%d (not NOERROR)", rcode);
        return false;
    }

    /* Check that we got at least one answer */
    uint16_t ancount = ((uint16_t)reply[6] << 8) | reply[7];
    if (ancount == 0) {
        ESP_LOGD(TAG, "probe: 0 answers in response");
        return false;
    }

    ESP_LOGD(TAG, "probe: upstream OK (txid=0x%04X, answers=%d)", txid, ancount);
    return true;
}

/* ── BT link health ────────────────────────────────────────────── */

/**
 * Check if the BT/BNEP link is alive by looking at recent RX activity.
 * The BNEP keepalive runs every 2s and the NAP responds, so recent RX
 * means the BT link itself is healthy — even if cellular data is down.
 *
 * Returns true if the BT link is alive (recent RX within threshold).
 */
static bool bt_link_is_alive(void)
{
    if (!hal_bt_is_connected()) {
        return false;
    }
    uint32_t last_rx = hal_bt_get_last_rx_ms();
    if (last_rx == 0) {
        return false;
    }
    uint32_t now = now_ms();
    return (now - last_rx) <= RESILIENCE_BT_LINK_ALIVE_MS;
}

/* ── DNS forward server refresh ─────────────────────────────────── */

/**
 * Periodically refresh the captive DNS forward server from the PAN
 * DHCP lease. Handles VPN connect/disconnect on the phone that changes
 * the system DNS server (e.g., Tailscale sets 100.100.100.100,
 * WireGuard may use a different resolver). Without this, the ESP32
 * keeps forwarding to the stale DNS server captured at PAN connect time.
 */
static void refresh_dns_forward_server(void)
{
    tinypan_ip_info_t ip_info;
    if (pan_manager_get_ip_info(&ip_info) != ESP_OK) {
        return;
    }

    /* captive_dns_set_forward_server() is idempotent — it just stores
     * the address. We call it every refresh interval so changes on the
     * phone (VPN DNS) propagate to client DNS forwarding. */
    captive_dns_set_forward_server(ip_info.dns_server);
}

/* ── NAT/route health check ────────────────────────────────────── */

static void verify_nat_health(void)
{
    struct netif *pan_netif = pan_manager_get_netif();
    struct netif *ap_netif = wifi_ap_get_netif();
    if (!pan_netif || !ap_netif) {
        return;
    }

    /* Check default route */
    if (netif_default != pan_netif) {
        ESP_LOGW(TAG, "NAT health: default route lost (was %c%c%d, now %s) — restoring",
                 pan_netif->name[0], pan_netif->name[1], pan_netif->num,
                 netif_default ? "other" : "NULL");
        netif_set_default(pan_netif);
    }

    /* Check NAPT on AP netif */
    if (!ap_netif->napt) {
        ESP_LOGW(TAG, "NAT health: NAPT disabled on AP netif — re-enabling");
        if (ip_napt_enable_netif(ap_netif, 1)) {
            ESP_LOGI(TAG, "NAT health: NAPT re-enabled");
        } else {
            ESP_LOGE(TAG, "NAT health: failed to re-enable NAPT");
        }
    }
}

/* ── Public API ────────────────────────────────────────────────── */

esp_err_t resilience_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }
    s_was_online = false;
    s_offline_since_ms = 0;
    s_last_probe_ms = 0;
    s_last_dns_refresh_ms = 0;
    s_probe_failures = 0;
    s_reconnect_in_progress = false;
    s_initialized = true;
    ESP_LOGI(TAG, "Resilience watchdog initialised "
             "(probe=%dms, fail_dead=%d, fail_alive=%d, "
             "dns_refresh=%dms, offline_reboot=%dms)",
             RESILIENCE_PROBE_INTERVAL_MS,
             RESILIENCE_PROBE_FAIL_THRESHOLD_DEAD,
             RESILIENCE_PROBE_FAIL_THRESHOLD_ALIVE,
             RESILIENCE_DNS_REFRESH_MS,
             RESILIENCE_OFFLINE_REBOOT_MS);
    return ESP_OK;
}

void resilience_notify_online(void)
{
    if (s_probe_failures > 0 || s_reconnect_in_progress) {
        ESP_LOGI(TAG, "PAN online — clearing %d probe failures",
                 s_probe_failures);
    }
    s_probe_failures = 0;
    s_reconnect_in_progress = false;
    s_offline_since_ms = 0;
}

void resilience_notify_offline(void)
{
    if (s_offline_since_ms == 0) {
        s_offline_since_ms = now_ms();
        ESP_LOGI(TAG, "PAN offline — starting offline timer for reboot fallback");
    }
}

void resilience_process(void)
{
    if (!s_initialized) {
        return;
    }

    uint32_t now = now_ms();
    bool pan_online = pan_manager_is_online();

    /* ── Track online/offline transitions ────────────────────── */
    if (pan_online != s_was_online) {
        if (pan_online) {
            resilience_notify_online();
        } else {
            resilience_notify_offline();
        }
        s_was_online = pan_online;
    }

    /* ── Offline reboot fallback ─────────────────────────────── */
    if (!pan_online && s_offline_since_ms > 0) {
        uint32_t offline_dur = now - s_offline_since_ms;
        if (offline_dur >= RESILIENCE_OFFLINE_REBOOT_MS) {
            ESP_LOGE(TAG, "Continuously offline for %lu ms — REBOOTING",
                     (unsigned long)offline_dur);
            vTaskDelay(pdMS_TO_TICKS(500));  /* let log flush */
            esp_restart();
        }
    }

    if (!pan_online) {
        return;  /* Only probe when ONLINE */
    }

    /* ── NAT/route health verification ───────────────────────── */
    verify_nat_health();

    /* ── DNS forward server refresh ──────────────────────────── */
    /* Handles VPN connect/disconnect on the phone that changes the
     * system DNS server. Re-reads the PAN DHCP DNS lease periodically
     * and updates the captive DNS forward target. */
    if (now - s_last_dns_refresh_ms >= RESILIENCE_DNS_REFRESH_MS) {
        s_last_dns_refresh_ms = now;
        refresh_dns_forward_server();
    }

    /* ── Upstream connectivity probe ─────────────────────────── */
    if (s_reconnect_in_progress) {
        if (pan_online) {
            /* Reconnect completed (PAN came back between calls) — resume probing */
            ESP_LOGI(TAG, "Reconnect completed, resuming probes");
            s_reconnect_in_progress = false;
            /* fall through to probing below */
        } else {
            /* Still waiting for reconnect to complete; skip probing */
            return;
        }
    }

    if (now - s_last_probe_ms < RESILIENCE_PROBE_INTERVAL_MS) {
        return;
    }
    s_last_probe_ms = now;

    bool ok = probe_upstream_dns();
    if (ok) {
        if (s_probe_failures > 0) {
            ESP_LOGI(TAG, "Upstream probe OK — failures reset (%d -> 0)",
                     s_probe_failures);
        }
        s_probe_failures = 0;
    } else {
        s_probe_failures++;

        /* Distinguish two failure modes:
         *
         * 1. BT link DEAD (no recent BNEP RX): The Bluetooth link
         *    itself is gone. Reconnect aggressively — there's nothing
         *    to lose since the link is already dead.
         *
         * 2. BT link ALIVE (recent BNEP keepalive RX): The Bluetooth
         *    link is healthy but the phone has no upstream internet
         *    (5G dead spot, VPN transition, tower handoff glitch).
         *    The phone will likely auto-recover cellular on its own.
         *    Tearing down a healthy BT link to reconnect is
         *    counterproductive — it takes 20-30s to re-establish
         *    (L2CAP + BNEP + DHCP) during which clients have ZERO
         *    connectivity even if 5G just came back. Wait much longer
         *    (5 minutes at 20s probe interval = 15 failures) before
         *    forcing a reconnect. */
        bool link_alive = bt_link_is_alive();
        int threshold = link_alive ? RESILIENCE_PROBE_FAIL_THRESHOLD_ALIVE
                                   : RESILIENCE_PROBE_FAIL_THRESHOLD_DEAD;

        ESP_LOGW(TAG, "Upstream probe FAILED (%d/%d) — BT link %s",
                 s_probe_failures, threshold,
                 link_alive ? "ALIVE (cellular dead — patient)" :
                              "DEAD (reconnecting aggressively)");

        if (s_probe_failures >= threshold) {
            if (link_alive) {
                ESP_LOGW(TAG, "Upstream unreachable for %lu ms but BT link "
                         "alive — forcing PAN reconnect to refresh DHCP/routing",
                         (unsigned long)(s_probe_failures *
                                         RESILIENCE_PROBE_INTERVAL_MS));
            } else {
                ESP_LOGW(TAG, "Upstream unreachable and BT link dead — "
                         "forcing PAN reconnect");
            }

            /* Force a full PAN reconnect. This clears all state and
             * re-establishes DHCP + NAT from scratch, picking up any
             * DNS server changes on the phone (VPN, etc.). */
            tinypan_state_t state = tinypan_get_state();
            if (state != TINYPAN_STATE_IDLE) {
                /* Get the current remote address before stopping so
                 * we can reconnect to the same phone. */
                const tinypan_config_t *cfg = tinypan_internal_get_config();
                esp_bd_addr_t bda;
                if (cfg) {
                    memcpy(bda, cfg->remote_addr, sizeof(esp_bd_addr_t));
                    tinypan_stop();
                    vTaskDelay(pdMS_TO_TICKS(1000));  /* brief pause */
                    tinypan_set_remote_addr(bda);
                    tinypan_start();
                    s_reconnect_in_progress = true;
                    ESP_LOGI(TAG, "PAN reconnect initiated");
                }
            }
            s_probe_failures = 0;
        }
    }
}

int resilience_get_probe_failures(void)
{
    return s_probe_failures;
}
