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
#include "router_config.h"
#include "wifi_config.h"

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
static uint32_t s_reconnect_set_ms = 0;   /* when the flag was last set */

/* Deferred non-blocking PAN reconnect. The original code blocked the main
 * loop for 3s between tinypan_stop() and tinypan_start(); instead we stop,
 * then wait out the teardown across subsequent resilience_process() ticks. */
static bool     s_pan_reconnect_pending = false;
static uint8_t  s_pan_reconnect_stage = 0;   /* 0 = not started, 1 = stopped & waiting */
static uint32_t s_pan_reconnect_stop_ms = 0; /* when tinypan_stop() was called */

/* TX wedge detection state (see RESILIENCE_TX_STALL_MS in resilience.h) */
static uint32_t s_tx_last_sent      = 0;
static uint32_t s_tx_stall_since_ms = 0;
static uint32_t s_last_tx_wedge_ms  = 0;
static uint32_t s_traffic_adv_ms    = 0;  /* last time TX sent counter advanced */

/* From tinypan_bnep_transport.c (same extern pattern as main.c) */
extern void bnep_transport_get_tx_stats(uint32_t* sent_ok, uint32_t* depth);

/* How long to wait for the BT stack to finish tearing down before restarting
 * PAN. Matches the original 3000ms blocking delay. */
#define RESILIENCE_PAN_RECONNECT_TEARDOWN_MS  3000

/* How long an intentional BT pause (reconnect_in_progress) suppresses the
 * offline-reboot fallback. Must exceed the on-demand max pause (120s) plus
 * reconnect time; after this, a genuinely stuck system still reboots. */
#define RESILIENCE_RECONNECT_GRACE_MS  300000

/* Probe task: runs the blocking DNS probe in a separate FreeRTOS task
 * so the main loop can continue sending BNEP keepalives (every 2s).
 * Without this, the probe blocks the main loop for 5-20s and the phone
 * drops the BNEP connection due to keepalive timeout. */
static TaskHandle_t  s_probe_task_handle   = NULL;
static volatile bool s_probe_busy          = false;
static volatile bool s_probe_done          = false;  /* result ready */
static volatile bool s_probe_result        = false;
static volatile uint32_t s_probe_rtt_ms    = 0;
static uint32_t s_probe_started_ms         = 0;      /* watchdog: when probe began */

/* Protects cross-task access to the probe result state above.
 * The probe task writes results; the main loop reads them. On a dual-core
 * ESP32, plain `volatile` does not provide ordering guarantees, so all
 * reads/writes go through this mutex. */
static SemaphoreHandle_t s_probe_state_mutex = NULL;

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
 * Send a DNS query to a specific server through the PAN interface.
 *
 * Two-stage probe strategy:
 *   1. Probe 8.8.8.8 directly (fast — phone forwards IP packet without
 *      DNS proxy processing). Should respond in <1s on cellular.
 *   2. If 8.8.8.8 is blocked by the carrier, fall back to the phone's
 *      DHCP-provided DNS forwarder. This is slower (Android's tethering
 *      DNS proxy can take 10+ seconds due to DNS-over-HTTPS or IPv6-
 *      first fallback) but works when 8.8.8.8 is blocked.
 *
 * Returns true if either probe succeeds.
 */
static bool probe_single_dns(uint32_t dns_server_h, uint32_t pan_ip_h,
                              uint32_t timeout_ms, uint16_t txid,
                              uint32_t *out_rtt_ms)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        return false;
    }

    /* Bind to PAN IP so the query routes through Bluetooth */
    struct sockaddr_in bind_addr = {0};
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_addr.s_addr = pan_ip_h;
    bind_addr.sin_port = 0;
    if (bind(sock, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0) {
        close(sock);
        return false;
    }

    struct timeval tv = {
        .tv_sec  = timeout_ms / 1000,
        .tv_usec = (timeout_ms % 1000) * 1000,
    };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    uint8_t query[512];
    uint16_t qlen = build_dns_query(query, sizeof(query),
                                    RESILIENCE_PROBE_DOMAIN, txid);
    if (qlen == 0) {
        close(sock);
        return false;
    }

    struct sockaddr_in dst = {0};
    dst.sin_family = AF_INET;
    dst.sin_port = htons(53);
    dst.sin_addr.s_addr = dns_server_h;

    int sent = sendto(sock, query, qlen, 0,
                      (struct sockaddr *)&dst, sizeof(dst));
    if (sent <= 0) {
        close(sock);
        return false;
    }

    uint32_t start = now_ms();
    uint8_t reply[512];
    struct sockaddr_in from;
    socklen_t from_len = sizeof(from);
    int n = recvfrom(sock, reply, sizeof(reply), 0,
                     (struct sockaddr *)&from, &from_len);
    uint32_t rtt = now_ms() - start;
    close(sock);

    if (out_rtt_ms) *out_rtt_ms = rtt;

    if (n < 12) {
        return false;
    }

    /* Verify transaction ID */
    uint16_t reply_txid = ((uint16_t)reply[0] << 8) | reply[1];
    if (reply_txid != txid) {
        return false;
    }

    /* Check RCODE */
    uint8_t rcode = reply[3] & 0x0F;
    if (rcode != 0) {
        return false;
    }

    /* Check at least one answer */
    uint16_t ancount = ((uint16_t)reply[6] << 8) | reply[7];
    if (ancount == 0) {
        return false;
    }

    return true;
}

/**
 * Probe task body — runs the blocking DNS probe.
 * This runs in its own FreeRTOS task so the main loop can continue
 * sending BNEP keepalives (every 2s) while the probe waits for a
 * DNS response (which can take 5-20s through Bluetooth PAN).
 */
static void probe_task_fn(void *arg)
{
    (void)arg;
    tinypan_ip_info_t ip_info;
    bool result = false;
    uint32_t rtt_ms = 0;

    if (pan_manager_get_ip_info(&ip_info) != ESP_OK) {
        ESP_LOGD(TAG, "probe: no PAN IP info");
        goto done;
    }

    uint32_t pan_ip_h = ip_info.ip_addr;
    uint16_t txid = (uint16_t)(now_ms() & 0xFFFF);
    if (txid == 0) txid = 1;

    /* Stage 1: Probe 8.8.8.8 directly (fast — bypasses phone's slow
     * DNS proxy). Should respond in <1s on cellular. */
    if (probe_single_dns(RESILIENCE_PROBE_DNS_FALLBACK, pan_ip_h,
                         RESILIENCE_PROBE_TIMEOUT_DIRECT_MS, txid, &rtt_ms)) {
        ESP_LOGI(TAG, "probe: upstream OK via 8.8.8.8 (rtt=%lu ms)",
                 (unsigned long)rtt_ms);
        result = true;
        goto done;
    }

    /* Stage 2: Fall back to the phone's DHCP DNS forwarder.
     * This is slower (Android tethering DNS proxy can take 10+ seconds
     * due to DNS-over-HTTPS or IPv6-first fallback) but works when
     * the carrier blocks direct 8.8.8.8 queries. */
    uint32_t phone_dns = ip_info.dns_server;
    if (phone_dns == 0 || phone_dns == RESILIENCE_PROBE_DNS_FALLBACK) {
        ESP_LOGD(TAG, "probe: 8.8.8.8 failed, no fallback DNS available");
        goto done;
    }

    uint16_t txid2 = txid ^ 0x55AA;
    if (txid2 == 0) txid2 = 1;

    if (probe_single_dns(phone_dns, pan_ip_h,
                         RESILIENCE_PROBE_TIMEOUT_FALLBACK_MS, txid2, &rtt_ms)) {
        ESP_LOGI(TAG, "probe: upstream OK via phone DNS (rtt=%lu ms, fallback)",
                 (unsigned long)rtt_ms);
        result = true;
        goto done;
    }

    ESP_LOGD(TAG, "probe: both 8.8.8.8 and phone DNS failed");

done:
    if (s_probe_state_mutex != NULL &&
        xSemaphoreTake(s_probe_state_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        s_probe_result = result;
        s_probe_rtt_ms = rtt_ms;
        s_probe_done = true;
        s_probe_busy = false;
        s_probe_task_handle = NULL;  /* clear under mutex — see watchdog */
        xSemaphoreGive(s_probe_state_mutex);
    } else {
        /* Fallback: never leave the watchdog thinking a probe is running.
         * This is an unsynchronized write (the mutex was unavailable), so it
         * is only reached in pathological cases. Log to aid debugging. */
        ESP_LOGW(TAG, "probe: failed to acquire state mutex, writing without lock");
        s_probe_result = result;
        s_probe_rtt_ms = rtt_ms;
        s_probe_done = true;
        s_probe_busy = false;
        s_probe_task_handle = NULL;
    }
    vTaskDelete(NULL);
}

/**
 * Start the DNS probe in a separate task (non-blocking).
 * Returns true if the probe was started, false if one is already running.
 */
static bool start_probe(void)
{
    if (s_probe_busy) {
        return false;  /* previous probe still running */
    }
    s_probe_busy = true;
    s_probe_result = false;
    s_probe_rtt_ms = 0;
    s_probe_started_ms = now_ms();

    BaseType_t ret = xTaskCreate(probe_task_fn, "probe", 4096, NULL,
                                 5, &s_probe_task_handle);
    if (ret != pdPASS) {
        ESP_LOGW(TAG, "probe: failed to create probe task");
        s_probe_busy = false;
        return false;
    }
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
    if (!pan_netif) {
        return;
    }

    /* Check default route */
    if (netif_default != pan_netif) {
        ESP_LOGW(TAG, "NAT health: default route lost — restoring");
        netif_set_default(pan_netif);
    }

    /* Re-enable NAPT on the AP netif only if it's actually disabled.
     * Blindly calling ip_napt_enable() every cycle re-runs the full
     * netif_list scan and re-initializes the NAPT table, which is
     * wasteful and can briefly disrupt active sessions. */
    if (my_ap_ip != 0 && ap_nat_enabled) {
        struct netif *ap_netif = wifi_ap_get_netif();
        if (ap_netif != NULL && !ap_netif->napt) {
            ESP_LOGW(TAG, "NAT health: NAPT disabled on AP netif — re-enabling");
            ip_napt_enable_netif(ap_netif, 1);
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
    s_pan_reconnect_pending = false;
    s_pan_reconnect_stage = 0;
    s_pan_reconnect_stop_ms = 0;
    s_tx_last_sent = 0;
    s_tx_stall_since_ms = 0;
    s_last_tx_wedge_ms = 0;
    s_initialized = true;

    /* Create the mutex protecting probe result state. */
    if (s_probe_state_mutex == NULL) {
        s_probe_state_mutex = xSemaphoreCreateMutex();
        if (s_probe_state_mutex == NULL) {
            ESP_LOGE(TAG, "Failed to create probe state mutex");
            return ESP_ERR_NO_MEM;
        }
    }

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

void resilience_set_reconnect_in_progress(bool in_progress)
{
    s_reconnect_in_progress = in_progress;
    if (in_progress) {
        /* Reset probe failures so the watchdog doesn't immediately
         * try to reconnect when we clear the flag. */
        s_probe_failures = 0;
        s_offline_since_ms = 0;  /* don't count this as offline time */
        s_reconnect_set_ms = now_ms();
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
        if (s_reconnect_in_progress &&
            (now - s_reconnect_set_ms) <= RESILIENCE_RECONNECT_GRACE_MS) {
            /* BT is intentionally paused (on-demand mode) or a reconnect
             * is being orchestrated — don't accrue offline time toward
             * the reboot fallback. NOTE: the online→offline edge above
             * re-arms s_offline_since_ms AFTER set_reconnect_in_progress()
             * cleared it, so we must keep sliding it here. Grace-limited
             * so a genuinely stuck reconnect still reboots eventually. */
            s_offline_since_ms = now;
        } else {
            uint32_t offline_dur = now - s_offline_since_ms;
            if (offline_dur >= RESILIENCE_OFFLINE_REBOOT_MS) {
                ESP_LOGE(TAG, "Continuously offline for %lu ms — REBOOTING",
                         (unsigned long)offline_dur);
                vTaskDelay(pdMS_TO_TICKS(500));  /* let log flush */
                esp_restart();
            }
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

    /* ── TX wedge self-recovery ──────────────────────────────── */
    /* Phone-side BT-PAN throttling can stall TX indefinitely while the
     * ACL stays alive; probes then fail in "patient" mode which by design
     * never reconnects. Signature: TX ring holds data (depth > 0) but the
     * transport's sent counter stops advancing. A PAN bounce reliably
     * restores flow, so force one after RESILIENCE_TX_STALL_MS of stall
     * (rate-limited by RESILIENCE_TX_STALL_COOLDOWN_MS). */
    {
        uint32_t sent = 0, depth = 0;
        bnep_transport_get_tx_stats(&sent, &depth);
        if (sent != s_tx_last_sent) {
            s_traffic_adv_ms = now;  /* TX active — used by the STALLED threshold */
        }
        if (depth == 0 || s_reconnect_in_progress || s_pan_reconnect_pending) {
            s_tx_last_sent = sent;
            s_tx_stall_since_ms = 0;
        } else if (sent != s_tx_last_sent) {
            s_tx_last_sent = sent;
            s_tx_stall_since_ms = 0;
        } else if (s_tx_stall_since_ms == 0) {
            s_tx_stall_since_ms = now;
        } else if (now - s_tx_stall_since_ms >= RESILIENCE_TX_STALL_MS &&
                   now - s_last_tx_wedge_ms >= RESILIENCE_TX_STALL_COOLDOWN_MS) {
            ESP_LOGW(TAG, "TX wedge: sent frozen for %lu ms with depth=%lu — "
                     "forcing PAN reconnect (phone-side BT-PAN stall)",
                     (unsigned long)(now - s_tx_stall_since_ms),
                     (unsigned long)depth);
            s_last_tx_wedge_ms = now;
            s_tx_stall_since_ms = 0;
            s_probe_failures = 0;
            s_pan_reconnect_pending = true;
        }
    }

    /* Upstream connectivity probe (async, non-blocking) */
    if (s_reconnect_in_progress) {
        if (pan_online) {
            ESP_LOGI(TAG, "Reconnect completed, resuming probes");
            s_reconnect_in_progress = false;
        } else {
            return;
        }
    }

    /* If probe task is still running, wait for it */
    if (s_probe_busy) {
        /* Watchdog: probe must never run longer than the sum of both stage
         * timeouts (5s + 15s = 20s) plus margin. If it does, the task likely
         * faulted before reaching `done:` and would otherwise leave the
         * watchdog stuck forever. Force-reset the state. */
        const uint32_t probe_max_ms =
            RESILIENCE_PROBE_TIMEOUT_DIRECT_MS + RESILIENCE_PROBE_TIMEOUT_FALLBACK_MS + 5000;
        if (now - s_probe_started_ms > probe_max_ms) {
            ESP_LOGE(TAG, "Probe task watchdog timeout (%lu ms) — resetting state",
                     (unsigned long)(now - s_probe_started_ms));
            if (s_probe_state_mutex != NULL &&
                xSemaphoreTake(s_probe_state_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                s_probe_busy = false;
                s_probe_done = false;
                s_probe_result = false;
                /* Delete the hung task while holding the mutex. The probe
                 * task clears s_probe_task_handle and self-deletes under this
                 * same mutex, so it cannot be mid-cleanup while we delete it. */
                if (s_probe_task_handle != NULL) {
                    vTaskDelete(s_probe_task_handle);
                    s_probe_task_handle = NULL;
                }
                xSemaphoreGive(s_probe_state_mutex);
            } else {
                s_probe_busy = false;
                s_probe_done = false;
                s_probe_result = false;
            }
        } else {
            return;
        }
    }

    /* If a probe just completed, process the result. Read s_probe_done and
     * s_probe_result under the mutex so we don't race the probe task. */
    bool ok = false;
    bool probe_completed = false;
    if (s_probe_state_mutex != NULL &&
        xSemaphoreTake(s_probe_state_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        if (s_probe_done) {
            s_probe_done = false;
            ok = s_probe_result;
            probe_completed = true;
        }
        xSemaphoreGive(s_probe_state_mutex);
    }

    if (probe_completed) {
        if (ok) {
            if (s_probe_failures > 0) {
                ESP_LOGI(TAG, "Upstream probe OK — failures reset (%d -> 0)",
                         s_probe_failures);
            }
            s_probe_failures = 0;
        } else {
            s_probe_failures++;

            bool link_alive = bt_link_is_alive();
            int threshold = link_alive ? RESILIENCE_PROBE_FAIL_THRESHOLD_ALIVE
                                       : RESILIENCE_PROBE_FAIL_THRESHOLD_DEAD;
            if (link_alive &&
                (now - s_traffic_adv_ms) <= RESILIENCE_TRAFFIC_ACTIVE_MS) {
                /* Client actively pushing traffic but upstream is dead —
                 * likely the Android BT-PAN "TX flows, RX dead" stall.
                 * Don't wait 5 min; bounce the PAN link sooner. */
                threshold = RESILIENCE_PROBE_FAIL_THRESHOLD_STALLED;
            }

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

                /* Defer the stop/start sequence to avoid blocking the main
                 * loop for 3s. We set a flag and let the next call complete it.
                 * The tinypan_stop() is safe to do here (it just tears down
                 * sockets); the 3s wait + restart happens on the next tick. */
                s_probe_failures = 0;
                if (!s_pan_reconnect_pending) {
                    s_pan_reconnect_pending = true;
                    ESP_LOGI(TAG, "PAN reconnect scheduled (non-blocking)");
                }
            }
        }
        return;
    }

    /* Complete a deferred PAN reconnect (was pending from previous tick).
     * Stage 0: stop PAN, record time. Stage 1: after the teardown window,
     * set remote address and restart. Fully non-blocking — no vTaskDelay. */
    if (s_pan_reconnect_pending) {
        if (s_pan_reconnect_stage == 0) {
            tinypan_state_t state = tinypan_get_state();
            if (state != TINYPAN_STATE_IDLE) {
                tinypan_stop();
                s_pan_reconnect_stop_ms = now;
                s_pan_reconnect_stage = 1;
                ESP_LOGI(TAG, "PAN reconnect: stopped, waiting %dms for teardown",
                         RESILIENCE_PAN_RECONNECT_TEARDOWN_MS);
            } else {
                /* PAN already idle — nothing to tear down. Match the original
                 * behaviour and skip the reconnect entirely. */
                s_pan_reconnect_pending = false;
                s_pan_reconnect_stage = 0;
            }
        } else if (now - s_pan_reconnect_stop_ms >= RESILIENCE_PAN_RECONNECT_TEARDOWN_MS) {
            const tinypan_config_t *cfg = tinypan_internal_get_config();
            if (cfg) {
                esp_bd_addr_t bda;
                memcpy(bda, cfg->remote_addr, sizeof(esp_bd_addr_t));
                tinypan_set_remote_addr(bda);
                tinypan_start();
                s_reconnect_in_progress = true;
                ESP_LOGI(TAG, "PAN reconnect initiated (deferred)");
            }
            s_pan_reconnect_pending = false;
            s_pan_reconnect_stage = 0;
        }
        return;
    }

    /* Start a new probe if interval has elapsed */
    if (now - s_last_probe_ms < RESILIENCE_PROBE_INTERVAL_MS) {
        return;
    }
    s_last_probe_ms = now;
    start_probe();
}

int resilience_get_probe_failures(void)
{
    return s_probe_failures;
}
