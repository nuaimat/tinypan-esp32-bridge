/**
 * @file captive_dns.c
 * @brief DNS server with IoT-friendly offline mode and forwarding.
 *
 * Uses the lwIP raw UDP API (no sockets, no FreeRTOS task) so it runs
 * entirely inside the lwIP TCP/IP thread.
 *
 * When PAN is offline: responds to all queries with DNS RCODE=2
 * (SERVFAIL) and 0 answer records. IoT clients interpret this as a
 * temporary failure and retry quickly, rather than caching a wrong
 * captive-portal A record that they cannot resolve by opening a
 * browser page.
 *
 * When PAN is online: forwards queries to the phone's DNS server and
 * relays replies back to the client.  This keeps the DNS listener on
 * :53 so existing DHCP clients (which received 192.168.99.1 as their
 * DNS server) continue to work without needing to re-lease.
 */

#include "captive_dns.h"
#include "wifi_ap.h"

#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include "esp_log.h"
#include "lwip/udp.h"
#include "lwip/ip_addr.h"
#include "lwip/pbuf.h"
#include "lwip/netif.h"
#include "lwip/tcpip.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

static const char *TAG = "captive_dns";

/* ── Internal state ──────────────────────────────────────────── */

static struct udp_pcb *s_dns_pcb = NULL;       /* listens on :53 */
static bool s_passthrough = false;              /* forward mode */
static ip4_addr_t s_fwd_dns_server;             /* phone's DNS server */

/* Socket-based DNS forwarding.
 * The raw UDP API (udp_sendto) fails with ERR_MEM because pbufs from the
 * WiFi stack lack headroom for UDP/IP/Ethernet headers.  A BSD socket
 * handles pbuf allocation internally and avoids this issue. */
static int s_fwd_sock = -1;                      /* UDP socket for forwarding + receiving replies */

/* Queue to pass DNS queries from lwIP callback to forward task.
 * sendto() cannot be called from the tcpip_thread (it posts back to
 * the same thread), so we defer to a separate task. */
#define DNS_FWD_QUEUE_LEN 8
typedef struct {
    uint8_t data[512];
    uint16_t len;
    ip_addr_t client_addr;
    uint16_t client_port;
} dns_fwd_msg_t;
static QueueHandle_t s_fwd_queue = NULL;

/* ── DNS constants ───────────────────────────────────────────── */

#define DNS_MIN_QUERY_LEN   12      /* header with no question */
#define DNS_OFFSET_FLAGS    2
#define DNS_OFFSET_QDCOUNT  4

#define DNS_FLAG_QR_RESP    0x8000
#define DNS_FLAG_RD         0x0100

/* Answer record */
#define DNS_ANSWER_NAME_PTR 0xC00C  /* compressed pointer to offset 12 */
#define DNS_TYPE_A          0x0001
#define DNS_CLASS_IN        0x0001
#define DNS_TTL              300
#define DNS_RDLENGTH_A      4

#define DNS_REDIRECT_IP_OCT1 BRIDGE_AP_IP_OCT1
#define DNS_REDIRECT_IP_OCT2 BRIDGE_AP_IP_OCT2
#define DNS_REDIRECT_IP_OCT3 BRIDGE_AP_IP_OCT3
#define DNS_REDIRECT_IP_OCT4 BRIDGE_AP_IP_OCT4

/* ── Forward task (separate FreeRTOS task) ──────────────────── */

static void fwd_task(void *arg)
{
    (void)arg;
    dns_fwd_msg_t msg;
    uint8_t reply_buf[512];

    while (1) {
        if (xQueueReceive(s_fwd_queue, &msg, portMAX_DELAY) != pdPASS) {
            continue;
        }
        if (s_fwd_sock < 0) {
            continue;
        }

        /* Forward query to phone's DNS server */
        struct sockaddr_in dst;
        memset(&dst, 0, sizeof(dst));
        dst.sin_family = AF_INET;
        dst.sin_port = htons(53);
        dst.sin_addr.s_addr = s_fwd_dns_server.addr;

        int sent = sendto(s_fwd_sock, msg.data, msg.len, 0,
                          (struct sockaddr *)&dst, sizeof(dst));
        if (sent <= 0) {
            ESP_LOGW(TAG, "DNS forward sendto failed: errno=%d", errno);
            continue;
        }
        ESP_LOGD(TAG, "DNS query forwarded (%d bytes)", sent);

        /* Wait for the matching reply. Replies whose transaction ID does
         * not match the query are stale (arrived after a previous query's
         * timeout) — discard them and keep waiting rather than relaying a
         * wrong response to the current client. */
        uint16_t query_txid = ((uint16_t)msg.data[0] << 8) | msg.data[1];
        for (;;) {
            struct sockaddr_in from;
            socklen_t from_len = sizeof(from);
            int n = recvfrom(s_fwd_sock, reply_buf, sizeof(reply_buf), 0,
                             (struct sockaddr *)&from, &from_len);
            if (n <= 0) {
                /* Forward failed — send SERVFAIL to client so it retries */
                if (s_dns_pcb) {
                    /* Walk past the question name in the original query */
                    uint16_t qname_end = DNS_MIN_QUERY_LEN;
                    while (qname_end < msg.len) {
                        uint8_t label_len = msg.data[qname_end];
                        if (label_len == 0) { qname_end++; break; }
                        if (label_len >= 64) { qname_end = 0; break; }
                        qname_end += label_len + 1;
                        if (qname_end > msg.len) { qname_end = 0; break; }
                    }
                    if (qname_end > DNS_MIN_QUERY_LEN) {
                        uint16_t q_end = qname_end + 4;
                        if (q_end <= msg.len) {
                            uint16_t q_len = q_end - DNS_MIN_QUERY_LEN;
                            uint16_t resp_len = DNS_MIN_QUERY_LEN + q_len;
                            struct pbuf *resp = pbuf_alloc(PBUF_TRANSPORT, resp_len, PBUF_RAM);
                            if (resp) {
                                uint8_t *rdata = (uint8_t *)resp->payload;
                                memcpy(rdata, msg.data, DNS_MIN_QUERY_LEN);
                                rdata[2] = 0x81; rdata[3] = 0x82;  /* SERVFAIL */
                                rdata[6] = rdata[7] = rdata[8] = rdata[9] = rdata[10] = rdata[11] = 0;
                                memcpy(rdata + DNS_MIN_QUERY_LEN, msg.data + DNS_MIN_QUERY_LEN, q_len);
                                LOCK_TCPIP_CORE();
                                udp_sendto(s_dns_pcb, resp, &msg.client_addr, msg.client_port);
                                UNLOCK_TCPIP_CORE();
                                pbuf_free(resp);
                            } else {
                                ESP_LOGE(TAG, "pbuf_alloc(%u) failed for SERVFAIL", resp_len);
                            }
                        }
                    }
                }
                break;
            }
            if (n >= 2) {
                uint16_t reply_txid = ((uint16_t)reply_buf[0] << 8) | reply_buf[1];
                if (reply_txid != query_txid) {
                    ESP_LOGW(TAG, "Discarding stale DNS reply (txid 0x%04X != 0x%04X)",
                             reply_txid, query_txid);
                    continue;
                }
            }
            if (s_dns_pcb && s_passthrough) {
                /* Relay the DNS reply back to the original client via raw UDP.
                 * Must lock lwIP core since we're outside the tcpip_thread. */
                struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, n, PBUF_RAM);
                if (p) {
                    memcpy(p->payload, reply_buf, n);
                    LOCK_TCPIP_CORE();
                    udp_sendto(s_dns_pcb, p, &msg.client_addr, msg.client_port);
                    UNLOCK_TCPIP_CORE();
                    pbuf_free(p);
                } else {
                    ESP_LOGE(TAG, "pbuf_alloc(%d) failed — DNS reply silently dropped", n);
                }
            }
            break;
        }
    }
    vTaskDelete(NULL);
}

/* ── Recv callback (runs in lwIP thread) ────────────────────── */

static void dns_recv_callback(void *arg, struct udp_pcb *pcb,
                               struct pbuf *p, const ip_addr_t *addr,
                               u16_t port)
{
    (void)arg;
    (void)pcb;

    /* Copy the full query into a flat buffer — pbuf chains are possible
     * and p->len only covers the first segment. */
    uint8_t query[512];
    uint16_t qlen = p->tot_len;
    if (qlen > sizeof(query)) qlen = sizeof(query);
    pbuf_copy_partial(p, query, qlen, 0);

    if (qlen < DNS_MIN_QUERY_LEN) {
        pbuf_free(p);
        return;
    }

    /* ── Forwarding mode: relay query to phone's DNS server ─── */
    if (s_passthrough && s_fwd_dns_server.addr != 0 && s_fwd_sock >= 0 && s_fwd_queue) {
        if (p->tot_len <= sizeof(((dns_fwd_msg_t *)0)->data)) {
            dns_fwd_msg_t msg = {0};
            memcpy(msg.data, query, qlen);
            msg.len = qlen;
            ip_addr_copy(msg.client_addr, *addr);
            msg.client_port = port;
            if (xQueueSend(s_fwd_queue, &msg, 0) == pdPASS) {
                pbuf_free(p);
                return;
            }
            ESP_LOGW(TAG, "DNS fwd queue full — serving SERVFAIL response");
        }
        /* Oversized query or queue full: fall through to SERVFAIL response */
    }

    /* ── Offline response: SERVFAIL for IoT-friendly behavior ────
     *
     * When the PAN link is down, respond with DNS RCODE=2 (SERVFAIL)
     * and 0 answer records. This tells IoT clients the DNS server is
     * temporarily unavailable, so they retry quickly (typically within
     * a few seconds) instead of caching a wrong A record.
     *
     * The previous captive-portal behavior (answering with the AP
     * gateway IP) is harmful for IoT devices: they cache the wrong IP
     * for 300s (TTL) and can't reach the internet even after the PAN
     * link recovers, because they don't open a captive portal page to
     * "log in". */
    const uint8_t *data = query;

    /* Walk past the question name (label-length encoded). */
    uint16_t qname_end = DNS_MIN_QUERY_LEN;
    while (qname_end < qlen) {
        uint8_t label_len = data[qname_end];
        if (label_len == 0) {
            qname_end++;               /* skip null terminator */
            break;
        }
        if (label_len >= 64) {         /* compressed or malformed — bail */
            pbuf_free(p);
            return;
        }
        qname_end += label_len + 1;
        if (qname_end > qlen) {        /* truncated */
            pbuf_free(p);
            return;
        }
    }

    uint16_t q_end = qname_end + 4;    /* + QTYPE(2) + QCLASS(2) */
    if (q_end > qlen) {
        pbuf_free(p);
        return;
    }

    uint16_t q_len = q_end - DNS_MIN_QUERY_LEN; /* bytes to copy */

    /* Allocate response: header(12) + question(q_len) — no answer records */
    uint16_t resp_len = DNS_MIN_QUERY_LEN + q_len;
    struct pbuf *resp = pbuf_alloc(PBUF_TRANSPORT, resp_len, PBUF_RAM);
    if (!resp) {
        ESP_LOGE(TAG, "pbuf_alloc(%u) failed", resp_len);
        pbuf_free(p);
        return;
    }

    uint8_t *rdata = (uint8_t *)resp->payload;

    /* ── Header: copy TXID, set response flags with SERVFAIL ──── */
    memcpy(rdata, data, DNS_MIN_QUERY_LEN);
    rdata[2] = 0x81;   /* QR=1, OPCODE=0, AA=0, TC=0, RD=1 */
    rdata[3] = 0x82;   /* RA=1, Z=0, RCODE=2 (SERVFAIL) */
    rdata[6] = 0x00;   /* ANCOUNT = 0 */
    rdata[7] = 0x00;
    rdata[8] = 0x00;   /* NSCOUNT = 0 */
    rdata[9] = 0x00;
    rdata[10] = 0x00;  /* ARCOUNT = 0 */
    rdata[11] = 0x00;

    /* ── Copy question section verbatim ──────────────────────── */
    memcpy(rdata + DNS_MIN_QUERY_LEN, data + DNS_MIN_QUERY_LEN, q_len);

    udp_sendto(s_dns_pcb, resp, addr, port);

    pbuf_free(resp);
    pbuf_free(p);
}

/* ==================================================================
 *  Public API
 * ================================================================== */

esp_err_t captive_dns_start(void)
{
    if (s_dns_pcb) {
        ESP_LOGW(TAG, "Already running");
        return ESP_OK;
    }

    s_dns_pcb = udp_new();
    if (!s_dns_pcb) {
        ESP_LOGE(TAG, "udp_new() failed");
        return ESP_ERR_NO_MEM;
    }

    ip_addr_t bind_addr;
    ip_addr_set_any(0, &bind_addr);

    err_t err = udp_bind(s_dns_pcb, &bind_addr, 53);
    if (err != ERR_OK) {
        ESP_LOGE(TAG, "udp_bind(:53) failed: %d", err);
        udp_remove(s_dns_pcb);
        s_dns_pcb = NULL;
        return ESP_FAIL;
    }

    udp_recv(s_dns_pcb, dns_recv_callback, NULL);

    ESP_LOGI(TAG, "Captive DNS server listening on :53");
    return ESP_OK;
}

void captive_dns_stop(void)
{
    if (!s_dns_pcb) {
        return;
    }

    udp_remove(s_dns_pcb);
    s_dns_pcb = NULL;

    ESP_LOGI(TAG, "Captive DNS server stopped");
}

void captive_dns_set_passthrough(bool enabled)
{
    s_passthrough = enabled;
    if (enabled) {
        ESP_LOGI(TAG, "PAN online — DNS forwarding mode (passthrough ON)");
        /* Create the forward queue first — the recv callback and fwd_task
         * both check it before use. */
        if (!s_fwd_queue) {
            s_fwd_queue = xQueueCreate(DNS_FWD_QUEUE_LEN, sizeof(dns_fwd_msg_t));
            if (!s_fwd_queue) {
                ESP_LOGE(TAG, "Failed to create DNS fwd queue (heap exhausted?)");
                return;
            }
        }
        /* Create forward socket if not already open.
         * A single socket is used for both sending queries and receiving
         * replies — the DNS server replies to the source port of the sender.
         * The socket is intentionally kept open across passthrough toggles:
         * closing it while fwd_task is blocked in recvfrom() is unsafe in
         * lwIP (concurrent close+recv on one socket), and stale replies
         * from a previous PAN session are filtered by TXID matching. */
        if (s_fwd_sock < 0) {
            int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            if (sock >= 0) {
                /* Bind to ephemeral port */
                struct sockaddr_in bind_addr;
                memset(&bind_addr, 0, sizeof(bind_addr));
                bind_addr.sin_family = AF_INET;
                bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);
                bind_addr.sin_port = 0;
                if (bind(sock, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0) {
                    ESP_LOGE(TAG, "DNS fwd socket bind failed: errno=%d", errno);
                    close(sock);
                    return;
                }
                /* Set receive timeout so recvfrom doesn't block too long.
                 * Short timeout to avoid queue buildup during DNS bursts. */
                struct timeval tv = { .tv_sec = 2, .tv_usec = 0 };
                setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
                s_fwd_sock = sock;
                /* Start forward task (only once, only with a valid queue) */
                static bool s_fwd_task_started = false;
                if (!s_fwd_task_started) {
                    if (xTaskCreate(fwd_task, "dns_fwd", 3072, NULL, 3, NULL) == pdPASS) {
                        s_fwd_task_started = true;
                    } else {
                        ESP_LOGE(TAG, "Failed to create DNS fwd task");
                    }
                }
            } else {
                ESP_LOGE(TAG, "socket() for DNS forward failed: errno=%d", errno);
            }
        }
    } else {
        ESP_LOGI(TAG, "PAN offline — DNS SERVFAIL mode (passthrough OFF, IoT-friendly)");
        /* Release forward resources to prevent leaks on repeated toggles */
        if (s_fwd_sock >= 0) {
            close(s_fwd_sock);
            s_fwd_sock = -1;
        }
    }
}

void captive_dns_set_forward_server(uint32_t dns_addr)
{
    if (dns_addr == 0) {
        ESP_LOGW(TAG, "Forward DNS server cleared (was 0.0.0.0)");
        s_fwd_dns_server.addr = 0;
        return;
    }
    s_fwd_dns_server.addr = dns_addr;
    ESP_LOGI(TAG, "Forward DNS server set to %d.%d.%d.%d",
             (int)(dns_addr & 0xFF), (int)((dns_addr >> 8) & 0xFF),
             (int)((dns_addr >> 16) & 0xFF), (int)((dns_addr >> 24) & 0xFF));
}
