/**
 * @file bt_state.c
 * @brief Bluetooth application-level state machine with reconnection backoff.
 *
 * Tracks the lifecycle of Bluetooth operations and handles automatic
 * reconnection after unexpected link loss with exponential backoff
 * (5 s → 15 s → 30 s, max 3 attempts).
 *
 * Thread safety: the public API is called from the BTU task (GAP
 * callback) or the application task.  The backoff timer callback runs
 * in the FreeRTOS timer task but only flags the attempt — the actual
 * reconnect runs via bt_state_process() in the application task.
 * All three are cooperative (single-core assumption) so no additional
 * locking is used.
 */

#include "bt_state.h"

#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"

static const char *TAG = "bt_state";

/* ── Transition table ──────────────────────────────────────────── */

typedef struct {
    bt_state_t       from;
    bt_state_event_t event;
    bt_state_t       to;
} bt_transition_t;

static const bt_transition_t s_transitions[] = {
    /* IDLE → active operations */
    {BT_STATE_IDLE,          BT_EVT_START_DISCOVERY,  BT_STATE_DISCOVERING},
    {BT_STATE_IDLE,          BT_EVT_START_PAIRING,    BT_STATE_PAIRING},
    {BT_STATE_IDLE,          BT_EVT_ACL_CONNECTED,    BT_STATE_CONNECTED},

    /* DISCOVERING → done / link loss */
    {BT_STATE_DISCOVERING,   BT_EVT_STOP_DISCOVERY,   BT_STATE_IDLE},
    {BT_STATE_DISCOVERING,   BT_EVT_ACL_DISCONNECTED, BT_STATE_IDLE},

    /* PAIRING → connected / failure / link loss */
    {BT_STATE_PAIRING,       BT_EVT_ACL_CONNECTED,    BT_STATE_CONNECTED},
    {BT_STATE_PAIRING,       BT_EVT_AUTH_FAIL,        BT_STATE_IDLE},
    {BT_STATE_PAIRING,       BT_EVT_ACL_DISCONNECTED, BT_STATE_IDLE},

    /* CONNECTED → intentional / unexpected disconnect */
    {BT_STATE_CONNECTED,     BT_EVT_START_DISCONNECT, BT_STATE_DISCONNECTING},
    {BT_STATE_CONNECTED,     BT_EVT_ACL_DISCONNECTED, BT_STATE_IDLE},

    /* DISCONNECTING → done / fallback */
    {BT_STATE_DISCONNECTING, BT_EVT_DISCONNECT_DONE,  BT_STATE_IDLE},
    {BT_STATE_DISCONNECTING, BT_EVT_ACL_DISCONNECTED, BT_STATE_IDLE},
};

#define TRANSITION_COUNT \
    (sizeof(s_transitions) / sizeof(s_transitions[0]))

/* ── Internal state ────────────────────────────────────────────── */

static bt_state_t              s_state          = BT_STATE_IDLE;
static bt_state_reconnect_cb_t s_reconnect_cb   = NULL;
static bt_state_change_cb_t    s_change_cb      = NULL;
static TimerHandle_t           s_backoff_timer  = NULL;
static esp_bd_addr_t           s_reconnect_bda  = {0};
static int                     s_retry_count    = 0;

/* ── State / event name helpers ────────────────────────────────── */

const char *bt_state_to_string(bt_state_t state)
{
    switch (state) {
    case BT_STATE_IDLE:          return "IDLE";
    case BT_STATE_DISCOVERING:   return "DISCOVERING";
    case BT_STATE_PAIRING:       return "PAIRING";
    case BT_STATE_CONNECTED:     return "CONNECTED";
    case BT_STATE_DISCONNECTING: return "DISCONNECTING";
    default:                     return "UNKNOWN";
    }
}

const char *bt_state_event_to_string(bt_state_event_t event)
{
    switch (event) {
    case BT_EVT_START_DISCOVERY:  return "START_DISCOVERY";
    case BT_EVT_STOP_DISCOVERY:   return "STOP_DISCOVERY";
    case BT_EVT_AUTH_SUCCESS:     return "AUTH_SUCCESS";
    case BT_EVT_AUTH_FAIL:        return "AUTH_FAIL";
    case BT_EVT_START_PAIRING:    return "START_PAIRING";
    case BT_EVT_ACL_CONNECTED:    return "ACL_CONNECTED";
    case BT_EVT_ACL_DISCONNECTED: return "ACL_DISCONNECTED";
    case BT_EVT_START_DISCONNECT: return "START_DISCONNECT";
    case BT_EVT_DISCONNECT_DONE:  return "DISCONNECT_DONE";
    default:                      return "UNKNOWN";
    }
}

/* ── Transition lookup ─────────────────────────────────────────── */

static const bt_transition_t *find_transition(bt_state_t       from,
                                              bt_state_event_t event)
{
    for (size_t i = 0; i < TRANSITION_COUNT; i++) {
        if (s_transitions[i].from == from &&
            s_transitions[i].event == event) {
            return &s_transitions[i];
        }
    }
    return NULL;
}

/* ── Backoff timer ─────────────────────────────────────────────── */

static uint32_t backoff_delay_sec(int attempt)
{
    switch (attempt) {
    case 1:  return BT_BACKOFF_DELAY_1;
    case 2:  return BT_BACKOFF_DELAY_2;
    default: return BT_BACKOFF_DELAY_3;
    }
}

/** Set by the backoff timer, consumed by bt_state_process() in app context. */
static volatile bool s_reconnect_pending = false;

static void backoff_timer_cb(TimerHandle_t timer)
{
    (void)timer;

    /* Do NOT run the reconnect here — the callback chain (bt_manager →
     * pan_manager → tinypan → esp_bt_l2cap_connect) is far too deep for
     * the small FreeRTOS timer daemon stack and caused a Tmr Svc stack
     * overflow crash. Defer to bt_state_process() in the app task. */
    ESP_LOGI(TAG, "Backoff timer expired — queueing reconnect "
             "attempt %d/%d", s_retry_count, BT_BACKOFF_MAX_RETRIES);

    s_reconnect_pending = true;
}

void bt_state_process(void)
{
    if (!s_reconnect_pending) {
        return;
    }
    s_reconnect_pending = false;

    if (s_reconnect_cb) {
        s_reconnect_cb(s_reconnect_bda, s_retry_count);
    } else {
        ESP_LOGW(TAG, "No reconnect callback registered — "
                 "cannot auto-reconnect");
    }
}

static void schedule_backoff(void)
{
    if (s_retry_count > BT_BACKOFF_MAX_RETRIES) {
        /* Instead of giving up permanently, reset the cycle and keep
         * trying. This is an unattended IoT bridge — it must keep
         * attempting to recover the BT link. */
        ESP_LOGI(TAG, "Retry cycle exhausted (%d) — resetting counter "
                 "and continuing", BT_BACKOFF_MAX_RETRIES);
        s_retry_count = 1;
    }

    uint32_t delay_sec = backoff_delay_sec(s_retry_count);

    ESP_LOGI(TAG, "Scheduling reconnect attempt %d/%d in %lu s "
             "(BDA %02x:%02x:%02x:%02x:%02x:%02x)",
             s_retry_count, BT_BACKOFF_MAX_RETRIES,
             (unsigned long)delay_sec,
             s_reconnect_bda[0], s_reconnect_bda[1],
             s_reconnect_bda[2], s_reconnect_bda[3],
             s_reconnect_bda[4], s_reconnect_bda[5]);

    if (s_backoff_timer) {
        xTimerChangePeriod(s_backoff_timer,
                           pdMS_TO_TICKS(delay_sec * 1000),
                           portMAX_DELAY);
        xTimerStart(s_backoff_timer, portMAX_DELAY);
    }
}

/* ── Public API ────────────────────────────────────────────────── */

esp_err_t bt_state_machine_init(void)
{
    if (s_backoff_timer) {
        ESP_LOGW(TAG, "State machine already initialised");
        return ESP_OK;
    }

    s_backoff_timer = xTimerCreate(
        "bt_backoff",
        pdMS_TO_TICKS(1000),   /* dummy — overwritten on schedule */
        pdFALSE,               /* one-shot                        */
        NULL,                  /* timer ID (unused)               */
        backoff_timer_cb
    );

    if (!s_backoff_timer) {
        ESP_LOGE(TAG, "Failed to create backoff timer");
        return ESP_ERR_NO_MEM;
    }

    s_state       = BT_STATE_IDLE;
    s_retry_count = 0;

    ESP_LOGI(TAG, "State machine initialised (state=IDLE)");
    return ESP_OK;
}

bt_state_t bt_state_machine_get_state(void)
{
    return s_state;
}

bool bt_state_can_transition(bt_state_t from, bt_state_t to)
{
    for (size_t i = 0; i < TRANSITION_COUNT; i++) {
        if (s_transitions[i].from == from &&
            s_transitions[i].to == to) {
            return true;
        }
    }
    return false;
}

esp_err_t bt_state_machine_event(bt_state_event_t event,
                                 const esp_bd_addr_t bda)
{
    const bt_transition_t *t = find_transition(s_state, event);

    if (!t) {
        ESP_LOGW(TAG, "Illegal transition: %s + %s → (rejected)",
                 bt_state_to_string(s_state),
                 bt_state_event_to_string(event));
        return ESP_ERR_INVALID_STATE;
    }

    /* No-op — already in the target state */
    if (t->to == s_state) {
        ESP_LOGD(TAG, "No-op: %s + %s → %s",
                 bt_state_to_string(s_state),
                 bt_state_event_to_string(event),
                 bt_state_to_string(t->to));
        return ESP_OK;
    }

    bt_state_t old = s_state;
    s_state = t->to;

    ESP_LOGI(TAG, "%s + %s → %s",
             bt_state_to_string(old),
             bt_state_event_to_string(event),
             bt_state_to_string(s_state));

    /* ── Side-effects ───────────────────────────────────────── */

    /* Remember BDA for potential reconnection */
    if (bda && (event == BT_EVT_ACL_DISCONNECTED ||
                event == BT_EVT_START_PAIRING)) {
        memcpy(s_reconnect_bda, bda, sizeof(esp_bd_addr_t));
    }

    /* Connection established — cancel any pending backoff */
    if (s_state == BT_STATE_CONNECTED) {
        if (s_retry_count > 0) {
            ESP_LOGI(TAG, "Connection restored — clearing backoff "
                     "(was on attempt %d)", s_retry_count);
        }
        bt_state_cancel_backoff();
        s_retry_count = 0;
    }

    /* Unexpected disconnect from an active connection — backoff */
    if (event == BT_EVT_ACL_DISCONNECTED &&
        (old == BT_STATE_CONNECTED || old == BT_STATE_PAIRING)) {
        s_retry_count++;
        schedule_backoff();
    }

    /* Notify state-change listener */
    if (s_change_cb) {
        s_change_cb(old, s_state, event);
    }

    return ESP_OK;
}

void bt_state_register_reconnect_cb(bt_state_reconnect_cb_t cb)
{
    s_reconnect_cb = cb;
}

void bt_state_register_change_cb(bt_state_change_cb_t cb)
{
    s_change_cb = cb;
}

void bt_state_cancel_backoff(void)
{
    if (s_backoff_timer &&
        xTimerIsTimerActive(s_backoff_timer) == pdTRUE) {
        xTimerStop(s_backoff_timer, portMAX_DELAY);
        ESP_LOGI(TAG, "Backoff timer cancelled");
    }
}

bool bt_state_backoff_pending(void)
{
    return (s_backoff_timer &&
            xTimerIsTimerActive(s_backoff_timer) == pdTRUE);
}
