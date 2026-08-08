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
)

# ── SPIFFS partition image from main/www/ directory ──────────────
spiffs_create_partition_image(www main/www FLASH_IN_PROJECT)
