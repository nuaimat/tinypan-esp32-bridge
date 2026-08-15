/**
 * @file nat_router.c
 * @brief lwIP NAPT router bridging WiFi AP clients through BT PAN.
 *
 * Waits for the PAN link to acquire an IP, then enables NAPT on the AP
 * netif (the LAN side) and sets the PAN netif as the lwIP default route.
 * WiFi clients on the SoftAP subnet can then reach the internet
 * through the phone's Bluetooth PAN connection.
 *
 * NAPT implementation adapted from martin-ger/esp32_nat_router (MIT).
 * See: https://github.com/martin-ger/esp32_nat_router
 */

#include "nat_router.h"

#include "esp_log.h"
#include "lwip/netif.h"
#include "lwip/lwip_napt.h"
#include "lwip/ip4_addr.h"

#include "pan_manager.h"
#include "wifi_ap.h"
#include "tinypan.h"
#include "captive_dns.h"
#include "tinypan_bnep.h"
#include "lwip/etharp.h"

static const char *TAG = "nat_router";

/* ── Internal state ──────────────────────────────────────────── */

static bool s_active = false;

/* Gateway IP of the static ARP entry added on IP_ACQUIRED (0 = none). */
static ip4_addr_t s_static_arp_gw = { .addr = 0 };

/* ── PAN event handler ───────────────────────────────────────── */

static void nat_router_teardown(void)
{
    if (!s_active) {
        return;
    }

    /* Remove the static ARP entry so a reconnect to a different phone
     * (different gateway MAC) doesn't blackhole traffic to a stale MAC. */
    if (s_static_arp_gw.addr != 0) {
        etharp_remove_static_entry(&s_static_arp_gw);
        ESP_LOGI(TAG, "Static ARP entry removed: %s", ip4addr_ntoa(&s_static_arp_gw));
        s_static_arp_gw.addr = 0;
    }

    /* Drop the default route if it still points at the PAN netif. */
    struct netif *pan_netif = pan_manager_get_netif();
    if (pan_netif && netif_default == pan_netif) {
        netif_set_default(NULL);
        ESP_LOGI(TAG, "Default route cleared (was PAN netif)");
    }

    /* Disable NAPT on the AP netif — without an upstream link, translating
     * client traffic just blackholes it and breaks the captive portal. */
    struct netif *ap_netif = wifi_ap_get_netif();
    if (ap_netif && ap_netif->napt) {
        ip_napt_enable_netif(ap_netif, 0);
        ESP_LOGI(TAG, "NAPT disabled on AP netif");
    }

    s_active = false;
    ESP_LOGI(TAG, "NAT router torn down — PAN offline");
}

static void on_pan_event(pan_manager_event_t event)
{
    if (event == PAN_MANAGER_EVENT_DISCONNECTED ||
        event == PAN_MANAGER_EVENT_ERROR) {
        nat_router_teardown();
        return;
    }

    if (event != PAN_MANAGER_EVENT_IP_ACQUIRED) {
        return;
    }

    struct netif *pan_netif = pan_manager_get_netif();
    if (!pan_netif) {
        ESP_LOGE(TAG, "PAN netif is NULL after IP_ACQUIRED — cannot enable NAT");
        return;
    }

    /* Enable NAPT on the AP (LAN) interface.
     *
     * ESP-IDF lwIP NAPT logic:
     * - ip_napt_forward() is called in ip4_forward() only when the OUTPUT
     *   netif does NOT have napt set, and internally only translates when
     *   the INPUT netif HAS napt set.
     * - ip_napt_recv() is called in ip4_input() only when the INPUT netif
     *   does NOT have napt set.
     *
     * For our topology (AP=LAN, PAN=WAN):
     * - Outbound: inp=AP(napt=1), out=PAN(napt=0) → ip_napt_forward runs,
     *   rewrites source from 192.168.99.x to PAN IP.
     * - Return: inp=PAN(napt=0), dest=PAN IP → ip_napt_recv runs,
     *   de-translates dest back to 192.168.99.x, then forwarded to AP.
     *
     * Previously NAPT was on PAN (WAN), which caused both paths to be
     * skipped — packets went out with private source IPs and replies
     * were never de-translated. */
    struct netif *ap_netif = wifi_ap_get_netif();
    if (!ap_netif) {
        ESP_LOGE(TAG, "AP netif is NULL — cannot enable NAPT");
        return;
    }
    if (!ip_napt_enable_netif(ap_netif, 1)) {
        ESP_LOGE(TAG, "ip_napt_enable_netif failed — is the AP netif up?");
        return;
    }
    ESP_LOGI(TAG, "NAPT enabled on AP netif (LAN side)");

    /* Route all outbound traffic through the PAN interface */
    netif_set_default(pan_netif);
    ESP_LOGI(TAG, "Default route set through PAN netif");

    /* Propagate DNS: set captive DNS forward server to phone's DNS.
     * DHCP clients use 192.168.99.1 as DNS (set at init), and the captive
     * DNS server forwards queries to the phone's DNS when PAN is online. */
    tinypan_ip_info_t ip_info;
    if (tinypan_get_ip_info(&ip_info) == TINYPAN_OK) {
        captive_dns_set_forward_server(ip_info.dns_server);

        /* Add static ARP entry for the PAN gateway/DNS server.
         * ARP over Bluetooth PAN is unreliable — the phone is slow to respond
         * to ARP requests, causing etharp_query to return ERR_MEM and drop
         * packets (especially DNS forwards).  A static entry eliminates this. */
        const uint8_t *remote_mac = bnep_get_remote_addr();
        if (remote_mac && (remote_mac[0] || remote_mac[5])) {
            ip4_addr_t gw_ip;
            gw_ip.addr = ip_info.gateway;
            struct eth_addr gw_eth;
            memcpy(gw_eth.addr, remote_mac, 6);
            err_t arp_err = etharp_add_static_entry(&gw_ip, &gw_eth);
            if (arp_err == ERR_OK) {
                s_static_arp_gw = gw_ip;
                ESP_LOGI(TAG, "Static ARP added: %s -> %02X:%02X:%02X:%02X:%02X:%02X",
                         ip4addr_ntoa(&gw_ip),
                         remote_mac[0], remote_mac[1], remote_mac[2],
                         remote_mac[3], remote_mac[4], remote_mac[5]);
            } else {
                ESP_LOGW(TAG, "etharp_add_static_entry failed: %d", arp_err);
            }
        } else {
            ESP_LOGW(TAG, "BNEP remote addr not set — skipping static ARP");
        }
    } else {
        ESP_LOGW(TAG, "Could not get PAN IP info — DNS forwarding will use fallback");
    }

    /* Diagnostics: log netif state and forwarding readiness */
    struct netif *def = netif_default;
    ESP_LOGI(TAG, "diag: default netif=%c%c%d, PAN netif=%c%c%d",
             def ? def->name[0] : '?', def ? def->name[1] : '?', def ? def->num : -1,
             pan_netif->name[0], pan_netif->name[1], pan_netif->num);
    ESP_LOGI(TAG, "diag: PAN up=%d link_up=%d flags=0x%02x mtu=%d",
             netif_is_up(pan_netif), netif_is_link_up(pan_netif),
             pan_netif->flags, pan_netif->mtu);
    ESP_LOGI(TAG, "diag: PAN ip=%s", ip4addr_ntoa(netif_ip4_addr(pan_netif)));

    ESP_LOGI(TAG, "diag: AP netif=%c%c%d up=%d link_up=%d flags=0x%02x napt=%d",
             ap_netif->name[0], ap_netif->name[1], ap_netif->num,
             netif_is_up(ap_netif), netif_is_link_up(ap_netif),
             ap_netif->flags, ap_netif->napt);

    /* List all netifs to verify both are registered */
    struct netif *n;
    int idx = 0;
    for (n = netif_list; n != NULL; n = n->next) {
        ESP_LOGI(TAG, "diag: netif[%d]=%c%c%d ip=%s flags=0x%02x",
                 idx++, n->name[0], n->name[1], n->num,
                 ip4addr_ntoa(netif_ip4_addr(n)), n->flags);
    }

    s_active = true;
    ESP_LOGI(TAG, "NAT router active — WiFi clients can now reach the internet");
}

/* ==================================================================
 *  Public API
 * ================================================================== */

esp_err_t nat_router_init(void)
{
    if (s_active) {
        ESP_LOGW(TAG, "Already initialised");
        return ESP_OK;
    }

    /* Register for PAN IP acquisition events.
     * NAT setup is deferred until the PAN link actually comes online. */
    pan_manager_register_event_cb(on_pan_event);

    ESP_LOGI(TAG, "NAT router initialised — waiting for PAN IP...");
    return ESP_OK;
}

bool nat_router_is_active(void)
{
    return s_active;
}
