#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
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
#include "dhcp_reservations.h"
#include "router_config.h"
#include <string.h>
#include "esp_gap_bt_api.h"
#include "tinypan.h"
#include "tinypan_internal.h"

static const char *TAG = "tinypan_bridge";

/* ── BT pause for WiFi handshake ───────────────────────────────── */
/* When a WiFi station tries to join, the EAPOL 4-way handshake needs
 * uncontested RF access. BT coexistence measures (PREFER_WIFI, PS_NONE,
 * QoS) are not enough for some devices (Chromebooks with 4s EAPOL
 * timeout). The nuclear option: temporarily disconnect the BT ACL link
 * to give WiFi 100% RF access, then reconnect after the handshake. */
static bool     s_bt_paused_for_wifi = false;
static uint32_t s_bt_pause_start_ms  = 0;
static esp_bd_addr_t s_paused_bt_bda;  /* saved for reconnect */
static bool     s_has_paused_bt_bda  = false;
static uint32_t s_bt_reconnect_start_ms = 0; /* non-blocking reconnect timer */
static bool     s_bt_suspend_pending = false;  /* wait for ACL disconnect, then suspend controller */
static bool     s_bt_suspended = false;        /* BT controller is disabled (RF given to WiFi) */

/* Resilience watchdog: we set this flag when intentionally disconnecting
 * BT for WiFi handshake, so the watchdog doesn't interpret it as a link
 * failure and try to reconnect. */

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

/* BT on-demand mode:
 *
 * The ESP32 shares a single antenna between BT and WiFi. Even with
 * ESP_COEX_PREFER_WIFI, BT BNEP keepalives (every 2s) steal RF airtime
 * from WiFi. This causes 802.11 auth/assoc and EAPOL handshake failures
 * for devices with strict timeouts (Chromebooks).
 *
 * Solution: disconnect BT whenever no WiFi stations are connected. This
 * gives WiFi 100% RF access for clients to authenticate and associate.
 * BT is reconnected only after a station completes the EAPOL handshake.
 *
 * Flow:
 * 1. Boot: BT auto-connects, PAN comes online
 * 2. No stations for > 10s → disconnect BT (WiFi gets clear RF)
 * 3. Chromebook auth/assoc succeeds (BT is off) → station joins (AID=1)
 * 4. EAPOL handshake completes (STACONNECTED) → reconnect BT
 * 5. BT reconnects, PAN online, Chromebook has internet
 * 6. Chromebook disconnects → station count drops to 0 → go to step 2
 *
 * Safety: if BT is paused for > 120s, reconnect briefly to keep the
 * phone's Bluetooth tethering alive, then disconnect again.
 */

#define BT_ONDEMAND_NO_STA_GRACE_MS   10000   /* disconnect BT after 10s with no stations */
#define BT_ONDEMAND_MAX_PAUSE_MS      120000  /* reconnect BT after 120s to keep tethering alive */
#define BT_RECONNECT_DELAY_MS         3000    /* delay between stop() and start() to drain stale L2CAP events */

static void save_bt_bda_for_reconnect(void)
{
    const tinypan_config_t *cfg = tinypan_internal_get_config();
    if (cfg && (cfg->remote_addr[0] || cfg->remote_addr[5])) {
        memcpy(s_paused_bt_bda, cfg->remote_addr, sizeof(esp_bd_addr_t));
        s_has_paused_bt_bda = true;
    }
}

static void disconnect_bt_for_wifi(const char *reason)
{
    tinypan_state_t bt_state = tinypan_get_state();
    if (bt_state == TINYPAN_STATE_IDLE || bt_state == TINYPAN_STATE_CONNECTING) {
        return;  /* already disconnected or connecting — nothing to do */
    }
    save_bt_bda_for_reconnect();
    ESP_LOGI(TAG, "Disconnecting BT (state=%s) — %s",
             tinypan_state_to_string(bt_state), reason);
    /* Tell the bt_state machine this disconnect is intentional BEFORE
     * stopping — the ACL_DISCONNECTED event arrives asynchronously
     * several seconds after tinypan_stop() (the LM keeps the ACL up
     * briefly). Without this, bt_state schedules its own backoff
     * reconnect 5s later, defeating the whole on-demand pause. */
    bt_state_set_reconnect_suppressed(true);
    tinypan_stop();
    s_bt_paused_for_wifi = true;
    s_bt_pause_start_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
    s_bt_suspend_pending = true;  /* suspend controller after ACL disconnects */
    resilience_set_reconnect_in_progress(true);
}

static void reconnect_bt(void)
{
    if (!s_has_paused_bt_bda) {
        ESP_LOGW(TAG, "Cannot reconnect BT — no saved BDA");
        s_bt_paused_for_wifi = false;
        bt_state_set_reconnect_suppressed(false);
        return;
    }
    ESP_LOGI(TAG, "Reconnecting BT to saved BDA (delayed)");
    tinypan_set_remote_addr(s_paused_bt_bda);
    /* Non-blocking delay: schedule the actual start() after BT_RECONNECT_DELAY_MS.
     * We do NOT call vTaskDelay here — that would block the entire main task
     * for 3 seconds, stalling LED updates, DNS passthrough toggling,
     * pan_manager_process(), and bt_state_process(). */
    s_bt_reconnect_start_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
}

/**
 * Poll non-blocking reconnect timer.
 * Called every iteration from poll_station_changes().
 */
static void poll_bt_reconnect_timer(void)
{
    if (s_bt_reconnect_start_ms == 0) {
        return; /* No pending reconnect */
    }
    uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;
    if ((int32_t)(now - s_bt_reconnect_start_ms) >= BT_RECONNECT_DELAY_MS) {
        ESP_LOGI(TAG, "BT reconnect timer expired — starting connection");
        /* Re-enable the BT controller + Bluedroid before initiating
         * a new connection. bt_manager_suspend() disabled them to give
         * WiFi exclusive RF access during the EAPOL handshake. */
        if (s_bt_suspended) {
            bt_manager_resume();
            s_bt_suspended = false;
        }
        tinypan_start();
        s_bt_paused_for_wifi = false;
        bt_state_set_reconnect_suppressed(false);
        /* resilience_set_reconnect_in_progress(false) will be called
         * by resilience_notify_online() when PAN comes back up. */
        s_bt_reconnect_start_ms = 0; /* Reset timer */
    }
}

static void poll_station_changes(void)
{
    static uint32_t s_last_check_ms = 0;
    static int      s_last_count = 0;
    static uint32_t s_no_stations_since_ms = 0;  /* 0 = not tracking */
    static bool     s_ondemand_active = false;   /* set true after boot grace period */

    /* Poll the non-blocking BT reconnect timer first — must run every iteration. */
    poll_bt_reconnect_timer();

    /* Deferred BT controller suspend: tinypan_stop() closes the L2CAP/BNEP
     * connection but the ACL link lingers for several seconds at the BT
     * controller level. We must wait for the ACL to fully disconnect
     * (bt_state → IDLE) before disabling the controller, otherwise
     * esp_bt_controller_disable() fails. Once suspended, the ESP32 coex
     * system sets coexist=0, giving WiFi exclusive RF access for the
     * EAPOL 4-way handshake. */
    if (s_bt_suspend_pending) {
        bt_state_t bstate = bt_state_machine_get_state();
        if (bstate == BT_STATE_IDLE) {
            ESP_LOGI(TAG, "ACL disconnected — suspending BT controller for WiFi RF exclusivity");
            bt_manager_suspend();
            s_bt_suspended = true;
            s_bt_suspend_pending = false;
        }
    }

    uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;
    if (now - s_last_check_ms < 250) {
        return;
    }
    s_last_check_ms = now;

    /* Enable BT on-demand mode 30s after boot (gives BT time to connect) */
    if (!s_ondemand_active && now > 30000) {
        s_ondemand_active = true;
        s_no_stations_since_ms = now;
        ESP_LOGI(TAG, "BT on-demand mode active — will disconnect BT when no WiFi stations");
    }

    int n = wifi_ap_get_connected_clients();
    if (n < 0) {
        return;  /* AP not running / query failed — keep previous state */
    }

    if (n != s_last_count) {
        if (n > s_last_count) {
            /* Station joining — need BT off for auth/assoc + EAPOL */
            ESP_LOGI(TAG, "WiFi station joining (%d -> %d)", s_last_count, n);
            s_no_stations_since_ms = 0;  /* cancel no-stations timer */

            if (!s_bt_paused_for_wifi) {
                /* BT is still connected — disconnect it now */
                disconnect_bt_for_wifi("WiFi station joining");
            }
            /* If BT is already paused (on-demand mode), keep it paused
             * for the EAPOL handshake */

            /* Only boost WiFi PS when BT controller is still active (coex=1).
             * When BT is suspended (coex=0), WiFi already has exclusive RF
             * and switching to WIFI_PS_NONE mid-handshake can disrupt the
             * in-progress EAPOL exchange. Keep MIN_MODEM in that case. */
            if (!s_bt_suspended) {
                wifi_ap_boost_wifi_for_handshake(15000);
            }
            wifi_ap_bt_pause(15000);
        } else {
            /* Station left — start no-stations timer */
            ESP_LOGI(TAG, "WiFi station left (%d -> %d)", s_last_count, n);
            if (s_bt_paused_for_wifi) {
                /* BT was paused for handshake — keep it paused.
                 * The no-stations timer will handle reconnection. */
                ESP_LOGI(TAG, "Station left, BT still paused — will reconnect via on-demand timer");
            }
            if (s_ondemand_active) {
                s_no_stations_since_ms = now;
            }
            wifi_ap_bt_pause(6000);  /* brief BT pause for retry */
        }
        s_last_count = n;
    }

    /* Check if the WiFi boost window has expired */
    wifi_ap_poll_handshake_timeout();

    /* If BT is paused and a station completed the handshake, reconnect BT.
     * Skip if a reconnect is already scheduled (s_bt_reconnect_start_ms != 0). */
    if (s_bt_paused_for_wifi && n > 0 && s_bt_reconnect_start_ms == 0 &&
        wifi_ap_check_handshake_completed()) {
        ESP_LOGI(TAG, "WiFi handshake completed — reconnecting BT");
        reconnect_bt();
    }

    /* BT on-demand: no stations for > grace period → disconnect BT */
    if (s_ondemand_active && n == 0 && !s_bt_paused_for_wifi &&
        s_no_stations_since_ms > 0 &&
        (now - s_no_stations_since_ms) > BT_ONDEMAND_NO_STA_GRACE_MS) {
        disconnect_bt_for_wifi("no WiFi stations (on-demand mode)");
        s_no_stations_since_ms = 0;  /* stop tracking until BT reconnects */
    }

    /* Safety: if BT has been paused for > max pause, reconnect to keep
     * phone tethering alive. The no-stations timer will disconnect again
     * after the grace period if still no stations.
     *
     * NOTE: use a FRESH timestamp and a signed diff here. The `now`
     * captured at the top of this function is stale by the time
     * disconnect_bt_for_wifi() (above) finishes — tinypan_stop() takes
     * ~50-100ms — so `now < s_bt_pause_start_ms` and an unsigned
     * subtraction underflows to ~4.29e9, firing this timeout instantly
     * and causing a BT disconnect/reconnect churn loop every ~13s. */
    if (s_bt_paused_for_wifi && s_bt_reconnect_start_ms == 0) {
        uint32_t now_fresh = xTaskGetTickCount() * portTICK_PERIOD_MS;
        if ((int32_t)(now_fresh - s_bt_pause_start_ms) > (int32_t)BT_ONDEMAND_MAX_PAUSE_MS) {
            ESP_LOGW(TAG, "BT pause timeout (%ds) — reconnecting to keep tethering alive",
                     (int)(BT_ONDEMAND_MAX_PAUSE_MS / 1000));
            reconnect_bt();
            if (s_ondemand_active) {
                s_no_stations_since_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
            }
        }
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

    /* Load DHCP reservations from NVS before WiFi AP starts so the
     * custom dhcpserver can honour them from the first client. */
    esp_err_t res_err = get_dhcp_reservations();
    if (res_err == ESP_OK) {
        ESP_LOGI(TAG, "DHCP reservations loaded from NVS");
    } else if (res_err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "No DHCP reservations in NVS (first boot)");
    } else {
        ESP_LOGW(TAG, "DHCP reservation load failed: %s", esp_err_to_name(res_err));
    }

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

        /* Periodic heap diagnostics — track heap exhaustion under load */
        {
            static uint32_t s_last_heap_log_ms = 0;
            uint32_t now_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
            if (now_ms - s_last_heap_log_ms >= 10000) {
                s_last_heap_log_ms = now_ms;
                extern void bnep_transport_log_stats(void);
                extern uint32_t hal_bt_debug_tx_complete_max_lat_us(void);
                ESP_LOGI(TAG, "heap: free=%lu min=%lu largest=%u txc_lat_max=%luus",
                         (unsigned long)esp_get_free_heap_size(),
                         (unsigned long)esp_get_minimum_free_heap_size(),
                         heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
                         (unsigned long)hal_bt_debug_tx_complete_max_lat_us());
                bnep_transport_log_stats();
            }
        }

        vTaskDelay(pdMS_TO_TICKS(5));
    }
}
