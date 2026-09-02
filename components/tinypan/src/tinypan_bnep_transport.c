/*
 * TinyPAN BNEP Transport Backend
 *
 * Implements tinypan_transport_t for Bluetooth Classic BNEP mode.
 * Designed for zero-copy transmission via scatter-gather iovec mapping,
 * separating the synthesized BNEP header from the original pbuf payload.
 */

#include "tinypan_transport.h"
#include "tinypan_bnep.h"
#include "tinypan_internal.h"
#include "../include/tinypan.h"
#include "../include/tinypan_config.h"
#include "../include/tinypan_hal.h"

#if !TINYPAN_USE_BLE_SLIP

#if TINYPAN_ENABLE_LWIP
#include "tinypan_lwip_netif.h"
#include "lwip/opt.h"
#include "lwip/pbuf.h"
#include "lwip/netif.h"
#include "netif/ethernet.h"
#include "lwip/etharp.h"
#endif

#include <string.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

/* Forward declare supervisor callbacks we need to emit */
extern void supervisor_on_bnep_setup_response(uint16_t response_code);
extern void supervisor_on_bnep_filter_response(uint16_t response_code);

/* Weak reference to WiFi AP BT pause function (defined in main/wifi_ap.c).
 * Returns true when BT keepalive should be paused during WiFi handshake. */
__attribute__((weak)) bool wifi_ap_bt_pause_active(void) { return false; }

static hal_mutex_t s_bnep_tx_mutex = NULL;
static hal_semaphore_t s_bnep_tx_semaphore = NULL;  /* wake-up signal for tp_tx task */
static TaskHandle_t s_tp_tx_task = NULL;            /* dedicated TX drain task */

static void tp_tx_task_fn(void* arg);

/* ── TX pipeline diagnostics ─────────────────────────────────────
 * Counters at each stage of the WiFi->BT forwarding path to locate
 * where packets are dropped or stalled:
 *   enq_full     - packets rejected because the ring was full (lwIP ERR_MEM)
 *   sent_ok      - successful hal_bt_l2cap_send_iovec() calls
 *   send_busy    - send returned "congested/busy" (VFS queue full or timeout)
 *   send_err     - hard send failures (packet dropped)
 *   write_max_ms - worst-case duration of a single send call (blocking write)
 */
static struct {
    uint32_t enq_full, sent_ok, send_busy, send_err;
    uint32_t write_max_ms;
} s_txstat;

static void bnep_transport_frame_cb(const bnep_ethernet_frame_t* frame, void* user_data) {
    (void)user_data;

#if TINYPAN_ENABLE_LWIP
    if (frame != NULL) {
        tinypan_netif_input(frame->dst_addr, frame->src_addr, frame->ethertype,
                            frame->payload, frame->payload_len);
    }
#endif
}

static void bnep_transport_setup_response_cb(uint16_t response_code, void* user_data) {
    (void)user_data;
    supervisor_on_bnep_setup_response(response_code);
}

static void bnep_transport_filter_response_cb(uint16_t response_code, void* user_data) {
    (void)user_data;
    supervisor_on_bnep_filter_response(response_code);
}

static int bnep_transport_init(void) {
    bnep_init();
    bnep_register_frame_callback(bnep_transport_frame_cb, NULL);
    bnep_register_setup_response_callback(bnep_transport_setup_response_cb, NULL);
    bnep_register_filter_response_callback(bnep_transport_filter_response_cb, NULL);
    
#if TINYPAN_ENABLE_LWIP
    if (s_bnep_tx_mutex == NULL) {
        s_bnep_tx_mutex = hal_mutex_create();
    }
    if (s_bnep_tx_semaphore == NULL) {
        s_bnep_tx_semaphore = hal_semaphore_create();
    }
    if (s_tp_tx_task == NULL) {
        BaseType_t ret = xTaskCreate(tp_tx_task_fn, "tp_tx",
                                     TINYPAN_BNEP_TX_TASK_STACK, NULL,
                                     TINYPAN_BNEP_TX_TASK_PRIO, &s_tp_tx_task);
        if (ret != pdPASS) {
            TINYPAN_LOG_ERROR("transport_bnep: failed to create tp_tx task");
            s_tp_tx_task = NULL;
        }
    }
#endif
    
    const tinypan_config_t* config = tinypan_internal_get_config();
    if (config) {
        uint8_t local_addr[6];
        hal_get_local_bd_addr(local_addr);
        /* Strip multicast bit and apply Locally Administered Bit (0x02) 
         * to match lwIP netif MAC exactly. */
        local_addr[0] &= ~0x01;
        local_addr[0] |= 0x02;
        bnep_set_local_addr(local_addr);
        bnep_set_remote_addr(config->remote_addr);
    }
    
    return 0;
}

static void bnep_transport_on_connected(void) {
    bnep_on_l2cap_connected();
}

static void bnep_transport_on_disconnected(void) {
    bnep_on_l2cap_disconnected();
}

static void bnep_transport_handle_incoming(const uint8_t* data, uint16_t len) {
    bnep_handle_incoming(data, len);
}

static void bnep_transport_retry_setup(void) {
    bnep_send_setup_request();
}

static void bnep_transport_on_can_send_now(void) {
    /* If we were stalled sending the setup request, try again now */
    if (bnep_get_state() == BNEP_STATE_WAIT_FOR_CONNECTION_RESPONSE) {
        bnep_send_setup_request();
    }
#if TINYPAN_ENABLE_LWIP
    /* Wake the dedicated TX task instead of draining inline. The drain (which
     * can block in write() for up to VFS_WRITE_TIMEOUT=500 ms under
     * congestion) runs in tp_tx_task,
     * so this handler — called from hal_bt_poll() in the app main loop — stays
     * non-blocking. */
    hal_semaphore_give(s_bnep_tx_semaphore);
#endif
}


#if TINYPAN_ENABLE_LWIP

/* TX Queue: stores unmodified pbufs alongside their synthesized BNEP headers */
typedef struct {
    struct pbuf* p;
    uint8_t hdr[15];
    uint8_t hdr_len;
    tinypan_iovec_t iov[6]; /* Reduced from 16 to 6 to save RAM; 16-chain pbufs are invalid. */
    uint16_t iov_count;
    uint32_t sent_at_ms;
    bool in_flight;
} bnep_tx_job_t;

static bnep_tx_job_t s_bnep_tx_queue[TINYPAN_TX_QUEUE_LEN];
static uint8_t s_bnep_tx_head = 0;
static uint8_t s_bnep_tx_tail = 0;

/* One accessor for both consumers: the periodic txstat log and the
 * resilience watchdog's TX-wedge detector (sent_ok advancing = data is
 * flowing; depth>0 with sent frozen = wedge). */
void bnep_transport_get_tx_stats(tinypan_tx_stats_t* stats) {
    if (stats == NULL) return;
    stats->enq_full     = s_txstat.enq_full;
    stats->sent_ok      = s_txstat.sent_ok;
    stats->send_busy    = s_txstat.send_busy;
    stats->send_err     = s_txstat.send_err;
    stats->write_max_ms = s_txstat.write_max_ms;
    stats->depth        = (uint32_t)((s_bnep_tx_tail + TINYPAN_TX_QUEUE_LEN - s_bnep_tx_head)
                                      % TINYPAN_TX_QUEUE_LEN);
}

void bnep_transport_log_stats(void) {
    tinypan_tx_stats_t s;
    bnep_transport_get_tx_stats(&s);
    TINYPAN_LOG_INFO("txstat: full=%u sent=%u busy=%u err=%u wmax=%ums depth=%u",
                     (unsigned)s.enq_full, (unsigned)s.sent_ok,
                     (unsigned)s.send_busy, (unsigned)s.send_err,
                     (unsigned)s.write_max_ms, (unsigned)s.depth);
    s_txstat.write_max_ms = 0;
}

/* Must be exposed to drain the BNEP tx queue */
void bnep_transport_drain_tx_queue(void) {
    hal_mutex_lock(s_bnep_tx_mutex);

    if (!bnep_drain_control_tx_queue()) {
        hal_mutex_unlock(s_bnep_tx_mutex);
        return;
    }

    if (s_bnep_tx_head == s_bnep_tx_tail) {
        hal_mutex_unlock(s_bnep_tx_mutex);
        return;
    }

    /* Synchronous HAL: drain the whole queue in one pass. Blocking on write()
     * is harmless here because this runs in the dedicated tp_tx task, not the
     * app main loop. */
    while (s_bnep_tx_head != s_bnep_tx_tail) {
        if (!hal_bt_l2cap_can_send()) {
            hal_bt_l2cap_request_can_send_now();
            break;
        }
        
        bnep_tx_job_t* job = &s_bnep_tx_queue[s_bnep_tx_head];
        
        if (job->in_flight) {
            /* Check for hardware/link timeout. If a packet is in flight but the 
             * TX_COMPLETE interrupt was lost (e.g. stack glitch or link drop), 
             * we must forcibly time out to reclaim memory. */
            uint32_t now = hal_get_tick_ms();
            if (now - job->sent_at_ms > TINYPAN_BNEP_TX_TIMEOUT_MS) {
                TINYPAN_LOG_ERROR("transport_bnep: TX timeout (in_flight=%d, job=%p, p=%p)", 
                                   job->in_flight, (void*)job, (void*)job->p);
                struct pbuf* q = job->p;
                job->p = NULL;
                job->in_flight = false;
                /* FATAL: We cannot just slide the queue forward on a DMA timeout. 
                 * We must tear down the entire link to safely cancel the hardware state. */
                hal_bt_l2cap_disconnect();
                if (q) pbuf_free(q);
                return; /* Stop draining until reconnection */
            }
            break; /* Packet still legitimately in flight */
        }
        
        struct pbuf* q = job->p;
        int result = 0;
        
        /* Convert to iovec array: 
         * iov[0] = Synthesized BNEP header
         * iov[1..N] = Original pbuf, skipping the 14-byte Ethernet header */
        job->iov[0].iov_base = job->hdr;
        job->iov[0].iov_len = job->hdr_len;
        job->iov_count = 1;

        /* Determine offset to skip the Ethernet header (typically 14 bytes) */
        uint16_t skip_bytes = 14;

        /* VLAN 802.1Q check: If the original header was 0x8100, we must 
         * skip 18 bytes (14 header + 4 VLAN tag). */
        uint8_t* eth_header;
#if defined(ETH_PAD_SIZE) && ETH_PAD_SIZE > 0
        eth_header = (uint8_t*)q->payload + ETH_PAD_SIZE;
        skip_bytes += ETH_PAD_SIZE;
#else
        eth_header = (uint8_t*)q->payload;
#endif
        if (eth_header[12] == 0x81 && eth_header[13] == 0x00) {
            skip_bytes += 4;
        }

        struct pbuf* iter = q;
        uint16_t max_iov = sizeof(job->iov) / sizeof(job->iov[0]);
        while (iter != NULL && job->iov_count < max_iov) {
            if (iter->len > 0) {
                if (skip_bytes >= iter->len) {
                    /* Entire pbuf is skipped */
                    skip_bytes -= iter->len;
                } else {
                    job->iov[job->iov_count].iov_base = (const uint8_t*)iter->payload + skip_bytes;
                    job->iov[job->iov_count].iov_len = iter->len - skip_bytes;
                    job->iov_count++;
                    skip_bytes = 0; /* Only skip in the first block(s) */
                }
            }
            iter = iter->next;
        }
        
        if (job->iov_count <= 1) {
            /* Empty frame (Ethernet header entirely skipped, no payload).
             * Drop cleanly as there is nothing to send besides the BNEP header. */
            TINYPAN_LOG_WARN("transport_bnep: Dropping empty payload frame");
            result = -1;
        } else if (iter != NULL) {
            TINYPAN_LOG_ERROR("transport_bnep: PBUF chain too long for iovec");
            result = -1;
        } else {
            hal_mutex_unlock(s_bnep_tx_mutex);
            uint32_t t0 = hal_get_tick_ms();
            result = hal_bt_l2cap_send_iovec(job->iov, job->iov_count);
            uint32_t dt = hal_get_tick_ms() - t0;
            if (dt > s_txstat.write_max_ms) s_txstat.write_max_ms = dt;
            hal_mutex_lock(s_bnep_tx_mutex);
        }
        
        if (result == 0) {
            /* Success. */
            s_txstat.sent_ok++;
#if TINYPAN_HAL_SYNCHRONOUS_TX
            /* Synchronous HAL: send_iovec() copied the payload into the L2CAP
             * VFS/ring buffer before returning, so the pbuf is no longer needed.
             * Free it and advance immediately instead of waiting for the deferred
             * TX_COMPLETE — this removes the one-packet-per-poll throughput
             * ceiling. This runs in the dedicated tp_tx task, so a congested link
             * (where write() blocks on L2CAP backpressure) stalls only this task. */
            job->p = NULL;
            s_bnep_tx_head = (s_bnep_tx_head + 1) % TINYPAN_TX_QUEUE_LEN;
            pbuf_free(q);
            continue;
#else
            /* Async HAL: hardware DMA holds the pbuf until TX_COMPLETE fires. */
            job->in_flight = true;
            job->sent_at_ms = hal_get_tick_ms();
            break; /* one packet in flight at a time */
#endif
        } else if (result > 0) {
            s_txstat.send_busy++;
            hal_bt_l2cap_request_can_send_now();
            break;
        } else {
            s_txstat.send_err++;
            /* Drop packet on hard failure */
            TINYPAN_LOG_ERROR("transport_bnep: Queue flush failed: %d", result);
            job->p = NULL;
            s_bnep_tx_head = (s_bnep_tx_head + 1) % TINYPAN_TX_QUEUE_LEN;
            pbuf_free(q);
        }
    }
    
    hal_mutex_unlock(s_bnep_tx_mutex);
}

/* Dedicated TX drain task.
 *
 * Blocks on s_bnep_tx_semaphore until the transport has TX work, then drains
 * the queue to empty-or-busy. The drain (and its blocking L2CAP VFS write())
 * runs here, at a priority BELOW the BTU task, so a congested link stalls only
 * this task — never the app main loop that owns hal_bt_poll / watchdog, and
 * never the BTU task that unblocks write() by draining the VFS queue. */
static void tp_tx_task_fn(void* arg) {
    (void)arg;
    for (;;) {
        hal_semaphore_take(s_bnep_tx_semaphore);
        bnep_transport_drain_tx_queue();
    }
}

static void bnep_transport_process(void) {
    /* BNEP Keepalive: Send a tiny control packet periodically to keep
     * the BNEP session alive. Sniff mode is now disabled via link policy
     * (BTA_DM_LINK_POLICY_SETTINGS), so we don't need aggressive keepalives.
     * 2s interval balances BT airtime with session stability.
     * Also skip keepalive when WiFi station handshake is in progress. */
#ifndef TINYPAN_BNEP_KEEPALIVE_MS
#define TINYPAN_BNEP_KEEPALIVE_MS 2000
#endif
    static uint32_t s_last_keepalive_ms = 0;
    if (bnep_is_connected()) {
        uint32_t now = hal_get_tick_ms();
        if (now - s_last_keepalive_ms > TINYPAN_BNEP_KEEPALIVE_MS) {
            if (!wifi_ap_bt_pause_active()) {
                s_last_keepalive_ms = now;
                static const uint8_t keepalive_pkt[] = { 0x01, 0x02, 0x00, 0x00 };
                hal_bt_l2cap_send(keepalive_pkt, sizeof(keepalive_pkt));
            }
        }
    }

    /* Periodic Garbage Collection: Reclaim timed-out pbufs even if no new traffic is arriving.
     * This prevents resource leaks if the hardware link stalls after enqueuing a packet. */
    hal_mutex_lock(s_bnep_tx_mutex);
    if (s_bnep_tx_head != s_bnep_tx_tail) {
        bnep_tx_job_t* job = &s_bnep_tx_queue[s_bnep_tx_head];
        if (job->in_flight) {
            uint32_t now = hal_get_tick_ms();
            if (now - job->sent_at_ms > TINYPAN_BNEP_TX_TIMEOUT_MS) {
                TINYPAN_LOG_ERROR("transport_bnep: TX cleanup timeout (job=%p)", (void*)job);
                struct pbuf* q = job->p;
                job->p = NULL;
                job->in_flight = false;
                /* Hardware/Link stall detected. Disconnect to request link termination.
                 * Note: While this requests the Bluetooth controller to tear down the link,
                 * it may not instantly stop an active DMA transfer. Advancing the queue
                 * and freeing the buffer is still a potential race condition on some silicon,
                 * but necessary to avoid permanent system stall. */
                hal_bt_l2cap_disconnect();
                
                /* Update ring buffer head to reflect packet reclamation. */
                s_bnep_tx_head = (s_bnep_tx_head + 1) % TINYPAN_TX_QUEUE_LEN;
                
                if (q) pbuf_free(q);
            }
        }
    }
    hal_mutex_unlock(s_bnep_tx_mutex);
}

void bnep_transport_flush_tx_queue(void) {
    hal_mutex_lock(s_bnep_tx_mutex);
    while (s_bnep_tx_head != s_bnep_tx_tail) {
        bnep_tx_job_t* job = &s_bnep_tx_queue[s_bnep_tx_head];
        
        /* Hardening: We MUST NOT free an in-flight packet during a standard flush
         * because the hardware DMA controller might still be physically reading it
         * over the memory bus. If the link is truly dead, the BNEP TX timeout logic
         * will independently reclaim this memory after waiting for DMA shutdown. */
        if (job->in_flight) {
            break;
        }

        if (job->p != NULL) {
            pbuf_free(job->p);
            job->p = NULL;
        }
        job->in_flight = false;
        s_bnep_tx_head = (s_bnep_tx_head + 1) % TINYPAN_TX_QUEUE_LEN;
    }
    hal_mutex_unlock(s_bnep_tx_mutex);
}

static void bnep_transport_on_tx_complete(void) {
    hal_mutex_lock(s_bnep_tx_mutex);
    if (s_bnep_tx_head != s_bnep_tx_tail) {
        bnep_tx_job_t* job = &s_bnep_tx_queue[s_bnep_tx_head];
        if (job->in_flight) {
            struct pbuf* q = job->p;
            job->p = NULL;
            job->in_flight = false;
            s_bnep_tx_head = (s_bnep_tx_head + 1) % TINYPAN_TX_QUEUE_LEN;
            if (q) pbuf_free(q);
            
            /* Wake the TX task to process the next packet (async HAL path). */
            hal_mutex_unlock(s_bnep_tx_mutex);
            hal_semaphore_give(s_bnep_tx_semaphore);
            return;
        }
    }
    hal_mutex_unlock(s_bnep_tx_mutex);
}

static int bnep_transport_output(struct netif* netif, struct pbuf* p) {
    (void)netif;
    if (p == NULL) return ERR_ARG;
    
    if (!bnep_is_connected()) {
        return ERR_CONN;
    }
    
    /* BNEP iovec Architecture (Structural Zero-Copy).
     * Instead of mutating the shared pbuf using pbuf_header(), we synthesize
     * the BNEP header locally in the queue struct, and pass it directly into
     * iov[0] while the remaining pbuf segments map to iov[1..N]. This 
     * architecture supports hardware-level zero-copy on HALs that implement 
     * scatter-gather DMA. */

    /* Read Ethernet header via safe byte indexing to avoid unaligned access faults
     * on architectures that do not support non-word-aligned pointers (e.g. ARM Cortex-M0).
     * We must also respect ETH_PAD_SIZE to correctly locate the start of the header. */
#if defined(ETH_PAD_SIZE) && ETH_PAD_SIZE > 0
    uint8_t* eth_ptr = (uint8_t*)p->payload + ETH_PAD_SIZE;
    uint16_t eth_avail = (p->len > ETH_PAD_SIZE) ? (p->len - ETH_PAD_SIZE) : 0;
#else
    uint8_t* eth_ptr = (uint8_t*)p->payload;
    uint16_t eth_avail = p->len;
#endif
    /* The Ethernet header must be contiguous in the first pbuf segment —
     * bail out rather than reading past the segment boundary. */
    if (eth_avail < 14) {
        TINYPAN_LOG_WARN("bnep_output: first pbuf segment too short for Ethernet header (%u)",
                         (unsigned)eth_avail);
        return ERR_ARG;
    }
    uint8_t* dst_addr = &eth_ptr[0];
    uint8_t* src_addr = &eth_ptr[6];
    uint16_t ethertype = (eth_ptr[12] << 8) | eth_ptr[13];

    /* VLAN 802.1Q Support: If ethertype is 0x8100, the real ethertype 
     * is at offset 16 and the payload starts at offset 18. */
    if (ethertype == 0x8100) {
        if (eth_avail < 18) {
            TINYPAN_LOG_WARN("bnep_output: VLAN tag truncated");
            return ERR_ARG;
        }
        ethertype = (eth_ptr[16] << 8) | eth_ptr[17];
    }

    uint8_t bnep_hdr_len = bnep_get_ethernet_header_len(dst_addr, src_addr);

    /* Enqueue the job */
    hal_mutex_lock(s_bnep_tx_mutex);
    uint8_t next_tail = (s_bnep_tx_tail + 1) % TINYPAN_TX_QUEUE_LEN;
    if (next_tail == s_bnep_tx_head) {
        /* Queue full, report backpressure */
        s_txstat.enq_full++;
        hal_mutex_unlock(s_bnep_tx_mutex);
        return ERR_MEM;
    }

    bnep_tx_job_t* job = &s_bnep_tx_queue[s_bnep_tx_tail];
    pbuf_ref(p);
    job->p = p;
    job->hdr_len = bnep_hdr_len;
    job->in_flight = false;
    job->sent_at_ms = 0;
    bnep_write_ethernet_header(job->hdr, bnep_hdr_len, dst_addr, src_addr, ethertype);
    
    s_bnep_tx_tail = next_tail;
    hal_mutex_unlock(s_bnep_tx_mutex);

    /* Wake the dedicated TX task. It drains the queue via
     * bnep_transport_drain_tx_queue(); the drain (which can block in write()
     * for up to VFS_WRITE_TIMEOUT=500 ms under congestion) runs in tp_tx_task,
     * not here. */
    hal_semaphore_give(s_bnep_tx_semaphore);
    
    return ERR_OK;
}
#endif

const tinypan_transport_t transport_bnep = {
    .name = "BNEP",
    .requires_setup = true,
    .init = bnep_transport_init,
    .on_connected = bnep_transport_on_connected,
    .on_disconnected = bnep_transport_on_disconnected,
    .handle_incoming = bnep_transport_handle_incoming,
    .retry_setup = bnep_transport_retry_setup,
    .on_can_send_now = bnep_transport_on_can_send_now,
    .on_tx_complete = bnep_transport_on_tx_complete,
    .flush_queues = bnep_transport_flush_tx_queue,
    .process = bnep_transport_process,
#if TINYPAN_ENABLE_LWIP
    .output = bnep_transport_output
#endif
};

#else
/* Stub for when BNEP is disabled */
const tinypan_transport_t transport_bnep = {
    .name = "BNEP (Disabled)"
};
#endif
