#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_timer.h"
#include "bt_manager.h"
#include "bt_state.h"
#include "wifi_ap.h"
#include "pan_manager.h"
#include "nat_router.h"
#include "http_server.h"
#include "captive_dns.h"
#include "led_indicator.h"
#include "console_repl.h"
#include "resilience.h"
#include <string.h>
#include "esp_gap_bt_api.h"

static const char *TAG = "tinypan_bridge";

/* ── LED error tracking ─────────────────────────────────────────── */

/** If > 0, the LED shows the error pattern until this timestamp (us). */
static int64_t s_error_until_us = 0;

/** Signal a transient error — LED shows error pattern for 5 seconds. */
static void led_signal_error(void)
{
    s_error_until_us = esp_timer_get_time() + 5000000;
}

/** Compute the correct LED pattern from current system state. */
static void update_led_pattern(void)
{
    /* Error override has highest priority */
    if (s_error_until_us > 0) {
        if (esp_timer_get_time() < s_error_until_us) {
            led_indicator_set_pattern(LED_PATTERN_ERROR);
            return;
        }
        s_error_until_us = 0;   /* error display expired */
    }

    bt_state_t bt = bt_state_machine_get_state();
    bool nat_active = nat_router_is_active();

    if (bt == BT_STATE_CONNECTED && nat_active) {
        led_indicator_set_pattern(LED_PATTERN_CONNECTED);
    } else if (bt == BT_STATE_DISCOVERING ||
               bt == BT_STATE_PAIRING ||
               bt == BT_STATE_DISCONNECTING ||
               bt == BT_STATE_CONNECTED) {
        /* BT is active but NAT is not up yet (or we're in transition) */
        led_indicator_set_pattern(LED_PATTERN_BT_CONNECTING);
    } else {
        led_indicator_set_pattern(LED_PATTERN_AP_READY);
    }
}

/* ── BT state-change callback ───────────────────────────────────── */

static void on_bt_state_change(bt_state_t old_state,
                               bt_state_t new_state,
                               bt_state_event_t event)
{
    ESP_LOGI(TAG, "BT state: %s → %s  (event: %s)",
             bt_state_to_string(old_state),
             bt_state_to_string(new_state),
             bt_state_event_to_string(event));

    /* Show error pattern on auth failure or unexpected disconnect */
    if (event == BT_EVT_AUTH_FAIL ||
        (event == BT_EVT_ACL_DISCONNECTED &&
         (old_state == BT_STATE_CONNECTED ||
          old_state == BT_STATE_PAIRING))) {
        led_signal_error();
    }

    update_led_pattern();
}

/* ── Station join/leave polling ─────────────────────────────────── */

/**
 * Poll the driver station list to detect joins/leaves. The
 * WIFI_EVENT_AP_STACONNECTED / AP_STADISCONNECTED events never reach the
 * event handler on this build (only boot events arrive), so the
 * reason-15 BT-pause mitigation never engaged. A station count change
 * means either a new station mid-EAPOL (join) or a failed handshake
 * (leave, with a retry typically following within seconds) — both cases
 * benefit from pausing BT keepalive to give EAPOL uncontested RF.
 */
static void poll_station_changes(void)
{
    static uint32_t s_last_check_ms = 0;
    static int      s_last_count = 0;

    uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;
    if (now - s_last_check_ms < 250) {
        return;
    }
    s_last_check_ms = now;

    int n = wifi_ap_get_connected_clients();
    if (n < 0) {
        return;  /* AP not running / query failed — keep previous state */
    }
    if (n != s_last_count) {
        ESP_LOGI(TAG, "WiFi station count changed %d -> %d — pausing BT keepalive for 6s",
                 s_last_count, n);
        wifi_ap_bt_pause(6000);
        s_last_count = n;
    }
}

/* ==================================================================
 *  app_main
 * ================================================================== */

void app_main(void) {
    /* ── Phase 1: Platform init ──────────────────────────────── */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    /* Start LED indicator — begins with BOOTING pattern (fast blink) */
    ESP_ERROR_CHECK(led_indicator_init());

    ESP_LOGI(TAG, "TinyPAN Bridge firmware starting...");

    /* ── Phase 2: Bluetooth ──────────────────────────────────── */
    ESP_ERROR_CHECK(bt_manager_init());
    ESP_LOGI(TAG, "Bluetooth initialised");

    /* Register for BT state changes so the LED stays in sync */
    bt_state_register_change_cb(on_bt_state_change);

    /* ── Phase 3: WiFi SoftAP ────────────────────────────────── */
    /* Load saved config from NVS (web UI changes) — falls back to
     * compile-time defaults when NVS is empty or unreadable. */
    bridge_ap_config_t wifi_cfg;
    ESP_ERROR_CHECK(wifi_ap_get_config(&wifi_cfg));
    ESP_ERROR_CHECK(wifi_ap_init(&wifi_cfg));
    ESP_ERROR_CHECK(wifi_ap_start());
    ESP_LOGI(TAG, "WiFi AP started (SSID: %s)", wifi_cfg.ssid);

    /* AP is up, no BT connection yet → slow blink */
    led_indicator_set_pattern(LED_PATTERN_AP_READY);

    /* ── Phase 4: Captive portal DNS ──────────────────────────── */
    ESP_ERROR_CHECK(captive_dns_start());
    ESP_LOGI(TAG, "Captive DNS started (all queries → " BRIDGE_AP_IP_STR ")");

    /* ── Phase 5: PAN manager ────────────────────────────────── */
    ESP_ERROR_CHECK(pan_manager_init());
    ESP_LOGI(TAG, "PAN manager initialised");

    /* ── Phase 6: NAT router ─────────────────────────────────── */
    ESP_ERROR_CHECK(nat_router_init());
    ESP_LOGI(TAG, "NAT router initialised");

    /* ── Phase 6b: Resilience watchdog ───────────────────────── */
    ESP_ERROR_CHECK(resilience_init());
    ESP_LOGI(TAG, "Resilience watchdog initialised");

    /* ── Phase 7: HTTP server + SPIFFS /www ──────────────────── */
    ESP_ERROR_CHECK(http_server_init());
    ESP_LOGI(TAG, "HTTP server started");

    /* ── Phase 7b: UART console REPL ───────────────────────────── */
    ESP_ERROR_CHECK(console_repl_start());
    ESP_LOGI(TAG, "Console REPL started on UART0");

    /* ── Phase 8: Load bonded device (connect delayed to main loop) ── */
    bt_bonded_device_t bonded;
    bool has_bonded = false;
    if (bt_manager_load_bonded_device(&bonded) == ESP_OK) {
        int bond_num = esp_bt_gap_get_bond_device_num();
        if (bond_num > 0) {
            esp_bd_addr_t bond_list[8];
            int n = bond_num < 8 ? bond_num : 8;
            esp_bt_gap_get_bond_device_list(&n, bond_list);
            for (int i = 0; i < n; i++) {
                if (memcmp(bond_list[i], bonded.bda,
                          sizeof(esp_bd_addr_t)) == 0) {
                    has_bonded = true;
                    break;
                }
            }
        }
        if (has_bonded) {
            ESP_LOGI(TAG, "Found bonded device \"%s\" — will auto-connect after delay",
                     bonded.name);
        } else {
            ESP_LOGW(TAG, "Saved device \"%s\" no longer bonded — skipping",
                     bonded.name);
        }
    } else {
        ESP_LOGI(TAG, "No bonded device in NVS — waiting for pairing");
    }

    ESP_LOGI(TAG, "Bridge ready. AP: %s, IP: " BRIDGE_AP_IP_STR,
             wifi_cfg.ssid);

    /* ── Main loop ───────────────────────────────────────────── */
    bool auto_connect_attempted = false;
    while (1) {
        pan_manager_process();

        /* Run deferred BT reconnect attempts (flagged by the backoff
         * timer — must run in task context, not the timer daemon) */
        bt_state_process();

        /* Delayed auto-connect: wait 5s for BT stack to fully settle */
        if (!auto_connect_attempted && has_bonded &&
            esp_timer_get_time() > 5000000) {
            ESP_LOGI(TAG, "Auto-connecting to bonded device \"%s\"",
                     bonded.name);
            pan_manager_connect(bonded.bda);
            auto_connect_attempted = true;
        }

        /* Detect station joins/leaves to pause BT keepalive during EAPOL */
        poll_station_changes();

        /* Periodically re-evaluate the LED — catches NAT coming online
         * (which has no dedicated callback we can hook without
         * disturbing the nat_router's own pan_manager event handler). */
        update_led_pattern();

        /* Toggle captive DNS passthrough based on PAN online state */
        {
            static bool s_dns_passthrough = false;
            bool pan_online = pan_manager_is_online();
            if (pan_online != s_dns_passthrough) {
                s_dns_passthrough = pan_online;
                captive_dns_set_passthrough(pan_online);
            }
        }

        /* Resilience watchdog: upstream probes, NAT health, reboot fallback */
        resilience_process();

        vTaskDelay(pdMS_TO_TICKS(5));
    }
}
