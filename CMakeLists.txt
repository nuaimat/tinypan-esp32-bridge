cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(tinypan_bridge)

# ── Override NAPT timeouts for high-latency Bluetooth PAN link ────
# Default ICMP=2s, UDP=2s — too short when BT sniff mode adds 200-800ms
target_compile_definitions(__idf_lwip PRIVATE
    IP_NAPT_TIMEOUT_MS_ICMP=30000
    IP_NAPT_TIMEOUT_MS_UDP=30000
    ETHARP_SUPPORT_STATIC_ENTRIES=1
)

# ── Disable Bluetooth sniff mode to prevent periodic latency spikes ──
# The phone (master) initiates sniff every ~5-7s, causing 200-800ms latency.
# Remove SNIFF and PARK from link policy so ESP32 rejects sniff requests.
# NOTE: the IDLE_TO_SNIFF delays are stored in a uint16_t PM-spec field
# (plus a 197 offset) by Bluedroid — 999999 + 197 truncates to ~17s.
# 0xFF00 (65280) + 197 = 65477, the largest value that fits in uint16_t.
# These are only a backup: the link policy override above is what
# actually blocks sniff.
target_compile_definitions(__idf_bt PRIVATE
    BTA_DM_LINK_POLICY_SETTINGS=0x0003
    BTA_FTS_OPS_IDLE_TO_SNIFF_DELAY_MS=0xFF00
    BTA_FTC_OPS_IDLE_TO_SNIFF_DELAY_MS=0xFF00
    # L2CAP TX congestion quota. Each queued GAP TX buffer costs 4112 bytes
    # (GAP_DATA_BUF_SIZE) regardless of packet size. Default quota is
    # 10 * (tx_rate + rx_rate) = 40 buffers = ~164 KB — far more than free
    # heap, so under WiFi->BT forwarding load the heap is exhausted
    # (BT_OSI malloc failed, size=4112) before L2CAP congestion ever trips.
    # Quota 1 -> 4 buffers (~8 KB with GAP_DATA_BUF_SIZE=2048) in flight.
    # Load test showed quota=1 wedges the channel into permanent congestion
    # under load (sent froze at 57, busy climbed to 273, ~0.4 pkt/s):
    # bluedroid stops draining the VFS queue once buff_quota is exhausted.
    # Raising to 2 -> 8 buffers (~16 KB) gives headroom; still far below the
    # default 40 buffers, so heap stays safe.
    L2CAP_CBB_DEFAULT_DATA_RATE_BUFF_QUOTA=2
    # Each GAP TX chunk buffer is GAP_DATA_BUF_SIZE bytes regardless of
    # payload (default 4112). BNEP L2CAP MTU is fixed at 1691, so the
    # worst case need is BT_HDR(8) + L2CAP_MIN_OFFSET(13) + 1691 = 1712.
    # 2048 halves the per-buffer heap cost with a safe margin.
    GAP_DATA_BUF_SIZE=2048
    # NOTE: do NOT try to override VFS_WRITE_TIMEOUT here. Stock ESP-IDF
    # (v5.4 / v5.5 / v6.1) hard-#defines it to (40 * 1000) in btc_l2cap.c,
    # so -DVFS_WRITE_TIMEOUT=500 only emits a "macro redefined" warning and
    # has NO effect — write() can still block up to 40 s. The firmware is
    # designed to tolerate that: ALL blocking L2CAP writes (data-ring drain
    # AND BNEP keepalives) run in the dedicated tp_tx task, never the app
    # main loop; close(fd) wakes a blocked write() with EPIPE
    # (SLOT_CLOSE_BIT); and the resilience TX-wedge detector bounces PAN if
    # sent stalls for 15 s. A 40 s block stalls only tp_tx.
)

# ── SPIFFS partition image from main/www/ directory ──────────────
spiffs_create_partition_image(www main/www FLASH_IN_PROJECT)
