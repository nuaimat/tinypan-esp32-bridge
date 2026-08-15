/* Network interface hooks for TinyPAN v2 — PAN upstream.
 * Adapted from martin-ger/esp32_nat_router netif_hooks.c.
 *
 * Hooks into the lwIP PAN netif input/linkoutput chains for:
 *  - Byte counting (STA/WAN side)
 *  - TCP MSS clamping for the BT PAN link (lower MTU than Ethernet)
 *
 * AP netif hooks were a pure pass-through (no logic) and have been
 * removed to avoid unnecessary per-packet function-call overhead on
 * the high-traffic WiFi downlink.
 */

#include <inttypes.h>
#include <string.h>
#include <time.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "lwip/netif.h"
#include "lwip/ip_addr.h"
#include "lwip/pbuf.h"
#include "lwip/prot/ip4.h"
#include "lwip/inet_chksum.h"
#include "router_config.h"
#include "wifi_config.h"
#include "pan_manager.h"

static const char *TAG = "netif_hooks";

/* ── Original netif function pointers ────────────────────────── */
static netif_input_fn      original_netif_input      = NULL;
static netif_linkoutput_fn original_netif_linkoutput = NULL;
static struct netif       *pan_netif                 = NULL;

/* Forward declaration */
static void clamp_tcp_mss(struct pbuf *p, uint16_t max_mss);

/* ── PAN netif input hook (upstream RX — bytes received) ─────── */
static err_t pan_input_hook(struct pbuf *p, struct netif *netif)
{
    if (netif == pan_netif && p != NULL) {
        sta_bytes_received += p->tot_len;
    }
    if (original_netif_input != NULL) {
        return original_netif_input(p, netif);
    }
    return ERR_VAL;
}

/* ── PAN netif linkoutput hook (upstream TX — bytes sent) ────── */
static err_t pan_linkoutput_hook(struct netif *netif, struct pbuf *p)
{
    /* TCP MSS clamping for BT PAN (lower MTU than Ethernet).
     * Only SYN packets carry the MSS option, so the cost is minimal. */
    if (ap_mss_clamp > 0 && p != NULL && p->len >= 14 + sizeof(struct ip_hdr)) {
        uint8_t *payload = (uint8_t *)p->payload;
        if (payload[12] == 0x08 && payload[13] == 0x00) {
            struct ip_hdr *iphdr = (struct ip_hdr *)(payload + 14);
            if (IPH_V(iphdr) == 4 && IPH_PROTO(iphdr) == PROTO_TCP) {
                clamp_tcp_mss(p, ap_mss_clamp);
            }
        }
    }

    if (netif == pan_netif && p != NULL) {
        sta_bytes_sent += p->tot_len;
    }

    if (original_netif_linkoutput != NULL) {
        return original_netif_linkoutput(netif, p);
    }
    return ERR_IF;
}

/* ── TCP MSS clamping (ported from reference) ────────────────── */
static void clamp_tcp_mss(struct pbuf *p, uint16_t max_mss)
{
    if (max_mss == 0 || p == NULL) return;
    if (p->len < 14 + 20 + 20) return;

    uint8_t *payload = (uint8_t *)p->payload;
    if (payload[12] != 0x08 || payload[13] != 0x00) return;

    struct ip_hdr *iphdr = (struct ip_hdr *)(payload + 14);
    if (IPH_V(iphdr) != 4 || IPH_PROTO(iphdr) != PROTO_TCP) return;

    uint16_t ip_hdr_len = IPH_HL(iphdr) * 4;
    if (p->len < 14 + ip_hdr_len + 20) return;

    uint8_t *tcphdr = payload + 14 + ip_hdr_len;
    if (!(tcphdr[13] & 0x02)) return; // SYN flag

    uint8_t tcp_hdr_len = (tcphdr[12] >> 4) * 4;
    if (tcp_hdr_len < 20 || p->len < 14 + ip_hdr_len + tcp_hdr_len) return;

    uint8_t *opt = tcphdr + 20;
    uint8_t *opt_end = tcphdr + tcp_hdr_len;
    while (opt < opt_end) {
        uint8_t kind = opt[0];
        if (kind == 0) break;
        if (kind == 1) { opt++; continue; }
        if (opt + 1 >= opt_end) break;
        uint8_t opt_len = opt[1];
        if (opt_len < 2 || opt + opt_len > opt_end) break;

        if (kind == 2 && opt_len == 4) {
            uint16_t *mss_ptr = (uint16_t *)(opt + 2);
            if (ntohs(*mss_ptr) > max_mss) {
                uint16_t old_mss_net = *mss_ptr;
                *mss_ptr = htons(max_mss);
                uint16_t *chksum = (uint16_t *)(tcphdr + 16);
                uint32_t sum = (uint16_t)~(*chksum);
                sum += (uint16_t)~old_mss_net;
                sum += htons(max_mss);
                while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
                *chksum = (uint16_t)~sum;
            }
            break;
        }
        opt += opt_len;
    }
}

/* ── Public API ──────────────────────────────────────────────── */

void init_byte_counter(void)
{
    /* Hook PAN netif for byte counting (upstream) */
    pan_netif = pan_manager_get_netif();
    if (pan_netif != NULL && original_netif_input == NULL) {
        original_netif_input      = pan_netif->input;
        pan_netif->input          = pan_input_hook;
        original_netif_linkoutput = pan_netif->linkoutput;
        pan_netif->linkoutput     = pan_linkoutput_hook;
        ESP_LOGI(TAG, "Byte counter hooked on PAN netif (%c%c%d)",
                 pan_netif->name[0], pan_netif->name[1], pan_netif->num);
    } else if (pan_netif == NULL) {
        ESP_LOGW(TAG, "PAN netif not ready — byte counter deferred");
    }
}

uint64_t get_sta_bytes_sent(void)    { return sta_bytes_sent; }
uint64_t get_sta_bytes_received(void) { return sta_bytes_received; }

void reset_sta_byte_counts(void)
{
    sta_bytes_sent     = 0;
    sta_bytes_received = 0;
}

void resync_connect_count(void)
{
    wifi_sta_list_t sta_list;
    if (esp_wifi_ap_get_sta_list(&sta_list) == ESP_OK) {
        connect_count = sta_list.num;
    }
}

/* ── Uptime ──────────────────────────────────────────────────── */

uint32_t get_uptime_seconds(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000000ULL);
}

void format_uptime(uint32_t seconds, char *buf, size_t buf_len)
{
    uint32_t days  = seconds / 86400;
    uint32_t hours = (seconds % 86400) / 3600;
    uint32_t mins  = (seconds % 3600) / 60;
    uint32_t secs  = seconds % 60;
    if (days > 0)
        snprintf(buf, buf_len, "%lud %02lu:%02lu:%02lu",
                 (unsigned long)days, (unsigned long)hours,
                 (unsigned long)mins, (unsigned long)secs);
    else
        snprintf(buf, buf_len, "%02lu:%02lu:%02lu",
                 (unsigned long)hours, (unsigned long)mins, (unsigned long)secs);
}

void format_boot_time(char *buf, size_t buf_len)
{
    time_t now;
    time(&now);
    if (now < 100000) {
        snprintf(buf, buf_len, "unknown");
        return;
    }
    time_t boot_time = now - (time_t)get_uptime_seconds();
    struct tm timeinfo;
    localtime_r(&boot_time, &timeinfo);
    snprintf(buf, buf_len, "%04d-%02d-%02d %02d:%02d:%02d",
             timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
             timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
}
