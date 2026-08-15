#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_bt_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── State machine states ───────────────────────────────────────── */

/** Bluetooth application-level state machine states */
typedef enum {
    BT_STATE_IDLE,           /**< No activity, ready for a new operation      */
    BT_STATE_DISCOVERING,    /**< Device discovery / inquiry in progress      */
    BT_STATE_PAIRING,        /**< Authentication / pairing in progress        */
    BT_STATE_CONNECTED,      /**< ACL + L2CAP connection established          */
    BT_STATE_DISCONNECTING,  /**< Intentional disconnect in progress          */
} bt_state_t;

/* ── Events ──────────────────────────────────────────────────────── */

/** Events that drive state transitions */
typedef enum {
    BT_EVT_START_DISCOVERY,   /**< Discovery / inquiry started                */
    BT_EVT_STOP_DISCOVERY,    /**< Discovery / inquiry completed              */
    BT_EVT_START_PAIRING,     /**< Connection + auth initiated                */
    BT_EVT_AUTH_SUCCESS,      /**< Authentication completed successfully      */
    BT_EVT_AUTH_FAIL,         /**< Authentication failed                      */
    BT_EVT_ACL_CONNECTED,     /**< ACL link established (L2CAP may follow)    */
    BT_EVT_ACL_DISCONNECTED,  /**< Unexpected ACL drop / link loss            */
    BT_EVT_START_DISCONNECT,  /**< App-initiated disconnect request           */
    BT_EVT_DISCONNECT_DONE,   /**< Intentional disconnect completed           */
} bt_state_event_t;

/* ── Reconnection backoff constants ──────────────────────────────── */

/** Backoff stage delays in seconds */
#define BT_BACKOFF_DELAY_1      5
#define BT_BACKOFF_DELAY_2     15
#define BT_BACKOFF_DELAY_3     30

/** Maximum number of auto-reconnect attempts before resetting the cycle.
 *  After this many attempts, the retry counter resets back to 1 and the
 *  cycle repeats indefinitely (unattended IoT bridge must self-recover). */
#define BT_BACKOFF_MAX_RETRIES  6

/* ── Callbacks ───────────────────────────────────────────────────── */

/**
 * @brief  Called when the state machine wants to attempt a reconnection
 *         after a backoff timer expires.
 *
 * The callee is expected to call bt_manager_connect() or equivalent.
 * Invoked from bt_state_process() in the application task context
 * (deferred from the timer callback — see bt_state_process()).
 *
 * @param bda      Remote device BD address to reconnect to.
 * @param attempt  Which attempt this is (1, 2, or 3).
 */
typedef void (*bt_state_reconnect_cb_t)(esp_bd_addr_t bda, int attempt);

/**
 * @brief  Called whenever the state changes (for logging / UI).
 *
 * Runs in the calling task's context (BTU or app).
 *
 * @param old_state  State before transition.
 * @param new_state  State after transition.
 * @param event      The event that caused the transition.
 */
typedef void (*bt_state_change_cb_t)(bt_state_t old_state,
                                     bt_state_t new_state,
                                     bt_state_event_t event);

/* ── API ─────────────────────────────────────────────────────────── */

/**
 * @brief  Initialise the state machine to IDLE and create the backoff timer.
 * @return ESP_OK on success.
 */
esp_err_t bt_state_machine_init(void);

/**
 * @brief  Get the current state.
 */
bt_state_t bt_state_machine_get_state(void);

/**
 * @brief  Human-readable name for a state (for logging).
 * @return String literal (do not free).
 */
const char *bt_state_to_string(bt_state_t state);

/**
 * @brief  Human-readable name for an event (for logging).
 * @return String literal (do not free).
 */
const char *bt_state_event_to_string(bt_state_event_t event);

/**
 * @brief  Feed an event into the state machine.
 *
 * If the event triggers a valid transition the state is updated and
 * any side-effects (backoff scheduling, callback invocation) are
 * performed.  Invalid transitions are logged and return an error.
 *
 * @param event  The event to process.
 * @param bda    Remote device BD address (may be NULL for events
 *               not tied to a specific device).
 * @return ESP_OK on success (including valid no-op events),
 *         ESP_ERR_INVALID_STATE if the transition is illegal.
 */
esp_err_t bt_state_machine_event(bt_state_event_t event,
                                 const esp_bd_addr_t bda);

/**
 * @brief  Check whether a transition from @p from to @p to is valid.
 * @return true if the transition exists in the transition table.
 */
bool bt_state_can_transition(bt_state_t from, bt_state_t to);

/**
 * @brief  Register the reconnect callback.
 *
 * Only one callback is supported; subsequent calls overwrite.
 */
void bt_state_register_reconnect_cb(bt_state_reconnect_cb_t cb);

/**
 * @brief  Register an optional state-change callback.
 */
void bt_state_register_change_cb(bt_state_change_cb_t cb);

/**
 * @brief  Cancel any pending backoff timer (e.g. when the user
 *         initiates a manual connect).
 */
void bt_state_cancel_backoff(void);

/**
 * @brief  Run a deferred reconnect attempt, if one is due.
 *
 * The backoff timer callback only flags the attempt; this function
 * performs it. Must be called periodically from the main application
 * task — never from the FreeRTOS timer task (stack overflow risk).
 */
void bt_state_process(void);

/**
 * @brief  Whether a backoff reconnect is currently scheduled.
 */
bool bt_state_backoff_pending(void);

/**
 * @brief  Suppress (or re-enable) automatic reconnection on ACL loss.
 *
 * Used when the application intentionally disconnects BT (e.g. BT
 * on-demand mode frees the antenna for WiFi). While suppressed, an
 * ACL_DISCONNECTED event does NOT schedule a backoff reconnect.
 * Enabling suppression also cancels any pending backoff/reconnect.
 */
void bt_state_set_reconnect_suppressed(bool suppressed);

#ifdef __cplusplus
}
#endif
