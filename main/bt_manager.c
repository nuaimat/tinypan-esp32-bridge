/**
 * @file bt_manager.c
 * @brief Bluetooth init, GAP security, device discovery, and connection management.
 *
 * Handles:
 *  - BT controller + Bluedroid host-stack initialisation
 *  - GAP callback: SSP auto-confirm (IO_CAP_NONE), PIN reject
 *  - Device inquiry / discovery
 *  - L2CAP connection initiation (BNEP PSM 0x000F)
 *
 * Thread safety: the GAP callback runs on the BTU task; this module uses
 * a spinlock to protect shared state.  Public API calls are expected from
 * the application task context.
 */

#include "bt_manager.h"
#include "bt_state.h"
#include "esp_heap_caps.h"

#include <string.h>
#include "esp_log.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_bt_device.h"
#include "esp_gap_bt_api.h"
#include "esp_l2cap_bt_api.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"

static const char *TAG = "bt_manager";

/* ── Device identity ──────────────────────────────────────────── */

#define BT_DEVICE_NAME  "TinyPAN Bridge"

/* COD: Networking / LAN Access Point */
#define BT_COD_SERVICE_NETWORKING  (0x020000)
#define BT_COD_MAJOR_LAN_NAP       0x0300
#define BT_COD                      (BT_COD_SERVICE_NETWORKING | BT_COD_MAJOR_LAN_NAP)

/* ── NVS keys for bonding persistence ──────────────────────────── */

#define BT_NVS_NAMESPACE  "bt_manager"
#define BT_NVS_KEY_BDA    "bt_bonded_bda"
#define BT_NVS_KEY_NAME   "bt_bonded_name"

/* ── Discovery defaults ────────────────────────────────────────── */

#define DEFAULT_INQ_DURATION    8    /* 8 × 1.28 s ≈ 10 s */
#define DEFAULT_INQ_MAX_RSPS    0    /* unlimited              */

/* ── Internal state ────────────────────────────────────────────── */

static bool s_initialised         = false;
static bool s_discovering         = false;
static bool s_connected           = false;
static bool s_intentional_disc    = false;  /* set during bt_manager_disconnect() */
static esp_bd_addr_t s_peer_bda  = {0};

/* Deduplicated discovery list — cleared on each bt_manager_start_discovery() */
static bt_discovered_device_t s_devices[BT_DISCOVERY_MAX_DEVICES];
static int                    s_device_count = 0;

/* ── Remote name resolution (after inquiry completes) ──────────── */

/** Max remote-name requests per scan (each unreachable device costs ~5 s) */
#define BT_NAME_RESOLVE_MAX  10

static volatile bool s_resolving_names = false;
static int           s_resolve_idx     = 0;   /* cursor into s_devices */
static int           s_resolve_issued  = 0;   /* requests issued this scan */

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

/* ── Callback slots ────────────────────────────────────────────── */

static bt_manager_discovery_cb_t  s_disc_cb  = NULL;
static bt_manager_conn_cb_t       s_conn_cb  = NULL;
static bt_manager_auth_cb_t       s_auth_cb  = NULL;

/* ── Forward declarations ──────────────────────────────────────── */

static void bt_gap_cb(esp_bt_gap_cb_event_t event,
                      esp_bt_gap_cb_param_t *param);

/* ── Helpers ────────────────────────────────────────────────────── */

/**
 * Find a device index in the dedup list by BD address.
 * @return  index (0..s_device_count-1) or -1 if not found.
 */
static int find_device_by_bda(const esp_bd_addr_t bda)
{
    for (int i = 0; i < s_device_count; i++) {
        if (memcmp(s_devices[i].bda, bda, sizeof(esp_bd_addr_t)) == 0)
            return i;
    }
    return -1;
}

/**
 * Persist the bonded device BDA and name to NVS.
 * Called from the GAP callback (BTU task context) on auth success.
 * Looks up the device name from the discovery list if available.
 */
static void save_bonded_device(const esp_bd_addr_t bda)
{
    nvs_handle_t handle;
    esp_err_t ret = nvs_open(BT_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "NVS open for bond save failed: %s", esp_err_to_name(ret));
        return;
    }

    /* Save BD address as a blob */
    ret = nvs_set_blob(handle, BT_NVS_KEY_BDA, bda, sizeof(esp_bd_addr_t));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "NVS set_blob (bda) failed: %s", esp_err_to_name(ret));
        nvs_close(handle);
        return;
    }

    /* Try to find the device name from the discovery list */
    char name[BT_BONDED_NAME_MAX] = {0};
    portENTER_CRITICAL_SAFE(&s_lock);
    int idx = find_device_by_bda(bda);
    if (idx >= 0) {
        strncpy(name, s_devices[idx].name, sizeof(name) - 1);
    }
    portEXIT_CRITICAL_SAFE(&s_lock);

    ret = nvs_set_str(handle, BT_NVS_KEY_NAME, name);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "NVS set_str (name) failed: %s", esp_err_to_name(ret));
        nvs_close(handle);
        return;
    }

    ret = nvs_commit(handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "NVS commit failed: %s", esp_err_to_name(ret));
    }

    nvs_close(handle);

    ESP_LOGI(TAG, "Bonded device saved: %02x:%02x:%02x:%02x:%02x:%02x (%s)",
             bda[0], bda[1], bda[2], bda[3], bda[4], bda[5],
             name[0] ? name : "unknown");
}

/* ── Remote name resolution ────────────────────────────────────── */

/**
 * Request the name of the next unnamed discovered device.
 * Runs in BTU task context (GAP callback) — issues one request at a
 * time; the READ_REMOTE_NAME_EVT handler chains to the next device.
 */
static void resolve_next_name(void)
{
    while (true) {
        esp_bd_addr_t bda;
        bool have = false;

        portENTER_CRITICAL_SAFE(&s_lock);
        while (s_resolve_idx < s_device_count) {
            if (s_devices[s_resolve_idx].name[0] == '\0') {
                memcpy(bda, s_devices[s_resolve_idx].bda,
                       sizeof(esp_bd_addr_t));
                have = true;
                break;
            }
            s_resolve_idx++;
        }
        portEXIT_CRITICAL_SAFE(&s_lock);

        if (!have || s_resolve_issued >= BT_NAME_RESOLVE_MAX) {
            s_resolving_names = false;
            ESP_LOGI(TAG, "Remote name resolution finished (%d request(s))",
                     s_resolve_issued);
            return;
        }

        s_resolve_idx++;      /* move past this entry; reply matched by BDA */
        s_resolve_issued++;

        if (esp_bt_gap_read_remote_name(bda) == ESP_OK) {
            return;           /* wait for ESP_BT_GAP_READ_REMOTE_NAME_EVT */
        }
        ESP_LOGW(TAG, "read_remote_name request failed — skipping device");
    }
}

bool bt_manager_is_resolving_names(void)
{
    return s_resolving_names;
}

/* ── State machine reconnect callback ─────────────────────────── */

/**
 * Called by bt_state when a backoff timer expires.
 * Reloads the bonded device from NVS and re-initiates the connection.
 *
 * Runs in FreeRTOS timer task context.
 */
static void bt_state_reconnect_handler(esp_bd_addr_t bda, int attempt)
{
    (void)bda; /* prefer NVS as source of truth */

    ESP_LOGI(TAG, "Auto-reconnect attempt %d/%d triggered by state machine",
             attempt, BT_BACKOFF_MAX_RETRIES);

    bt_bonded_device_t bonded;
    if (bt_manager_load_bonded_device(&bonded) == ESP_OK) {
        bt_manager_connect(bonded.bda);
    } else {
        ESP_LOGW(TAG, "No bonded device in NVS — cannot reconnect");
    }
}

/* ==================================================================
 *  GAP callback (runs in BTU task context)
 * ================================================================== */

static void bt_gap_cb(esp_bt_gap_cb_event_t event,
                      esp_bt_gap_cb_param_t *param)
{
    switch (event) {

    /* ── Discovery ─────────────────────────────────────── */
    case ESP_BT_GAP_DISC_RES_EVT: {
        /* Extract device name from EIR if present, else empty */
        char name[32] = {0};
        int8_t rssi = 0;
        uint32_t cod = 0;

        for (int i = 0; i < param->disc_res.num_prop; i++) {
            esp_bt_gap_dev_prop_t *p = &param->disc_res.prop[i];
            switch (p->type) {
            case ESP_BT_GAP_DEV_PROP_BDNAME:
                if (p->len > 0 && p->val) {
                    size_t copy = (size_t)p->len < sizeof(name) - 1
                                      ? (size_t)p->len : sizeof(name) - 1;
                    memcpy(name, p->val, copy);
                    name[copy] = '\0';
                }
                break;
            case ESP_BT_GAP_DEV_PROP_RSSI:
                if (p->len > 0 && p->val)
                    rssi = *(int8_t *)p->val;
                break;
            case ESP_BT_GAP_DEV_PROP_COD:
                if (p->len > 0 && p->val)
                    cod = *(uint32_t *)p->val;
                break;
            default:
                break;
            }
        }

        ESP_LOGI(TAG, "Discovered: %02x:%02x:%02x:%02x:%02x:%02x  RSSI=%d  name=\"%s\"",
                 param->disc_res.bda[0], param->disc_res.bda[1],
                 param->disc_res.bda[2], param->disc_res.bda[3],
                 param->disc_res.bda[4], param->disc_res.bda[5],
                 rssi, name);

        /* Dedup by BDA — keep the highest RSSI, fill in name if missing */
        portENTER_CRITICAL_SAFE(&s_lock);
        int idx = find_device_by_bda(param->disc_res.bda);
        if (idx >= 0) {
            if (rssi > s_devices[idx].rssi)
                s_devices[idx].rssi = rssi;
            if (s_devices[idx].name[0] == '\0' && name[0] != '\0') {
                memcpy(s_devices[idx].name, name,
                       sizeof(s_devices[idx].name));
            }
            if (s_devices[idx].cod == 0 && cod != 0)
                s_devices[idx].cod = cod;
        } else if (s_device_count < BT_DISCOVERY_MAX_DEVICES) {
            idx = s_device_count++;
            memcpy(s_devices[idx].bda, param->disc_res.bda,
                   sizeof(esp_bd_addr_t));
            memcpy(s_devices[idx].name, name,
                   sizeof(s_devices[idx].name));
            s_devices[idx].cod  = cod;
            s_devices[idx].rssi = rssi;
        }
        portEXIT_CRITICAL_SAFE(&s_lock);

        if (s_disc_cb)
            s_disc_cb(param->disc_res.bda, name, rssi, cod);
        break;
    }

    case ESP_BT_GAP_DISC_STATE_CHANGED_EVT: {
        portENTER_CRITICAL_SAFE(&s_lock);
        s_discovering = (param->disc_st_chg.state == ESP_BT_GAP_DISCOVERY_STARTED);
        if (s_discovering) {
            /* New discovery session — clear the device list */
            s_device_count = 0;
            memset(s_devices, 0, sizeof(s_devices));
        }
        portEXIT_CRITICAL_SAFE(&s_lock);
        ESP_LOGI(TAG, "Discovery %s",
                 s_discovering ? "started" : "stopped");

        /* When discovery stops (natural completion or cancel),
         * tell the state machine so polling endpoints see completed=true */
        if (!s_discovering) {
            bt_state_machine_event(BT_EVT_STOP_DISCOVERY, NULL);

            /* Inquiry responses rarely carry names (no EIR name from many
             * phones) — fetch them explicitly, one device at a time. */
            s_resolve_idx    = 0;
            s_resolve_issued = 0;
            s_resolving_names = true;
            resolve_next_name();
        }
        break;
    }

    case ESP_BT_GAP_READ_REMOTE_NAME_EVT: {
        if (param->read_rmt_name.stat == ESP_BT_STATUS_SUCCESS &&
            param->read_rmt_name.rmt_name[0] != '\0') {
            portENTER_CRITICAL_SAFE(&s_lock);
            int idx = find_device_by_bda(param->read_rmt_name.bda);
            if (idx >= 0 && s_devices[idx].name[0] == '\0') {
                size_t copy = strnlen((const char *)param->read_rmt_name.rmt_name,
                                      sizeof(s_devices[idx].name) - 1);
                memcpy(s_devices[idx].name,
                       param->read_rmt_name.rmt_name, copy);
                s_devices[idx].name[copy] = '\0';
            }
            portEXIT_CRITICAL_SAFE(&s_lock);
            ESP_LOGI(TAG, "Remote name: %02x:%02x:%02x:%02x:%02x:%02x = \"%s\"",
                     param->read_rmt_name.bda[0], param->read_rmt_name.bda[1],
                     param->read_rmt_name.bda[2], param->read_rmt_name.bda[3],
                     param->read_rmt_name.bda[4], param->read_rmt_name.bda[5],
                     param->read_rmt_name.rmt_name);
        } else {
            ESP_LOGD(TAG, "Remote name request failed (stat=%d)",
                     param->read_rmt_name.stat);
        }

        if (s_resolving_names) {
            resolve_next_name();
        }
        break;
    }

    /* ── Authentication ───────────────────────────────── */
    case ESP_BT_GAP_AUTH_CMPL_EVT: {
        bool ok = (param->auth_cmpl.stat == ESP_BT_STATUS_SUCCESS);
        ESP_LOGI(TAG, "Auth complete with %02x:%02x:%02x:%02x:%02x:%02x — %s",
                 param->auth_cmpl.bda[0], param->auth_cmpl.bda[1],
                 param->auth_cmpl.bda[2], param->auth_cmpl.bda[3],
                 param->auth_cmpl.bda[4], param->auth_cmpl.bda[5],
                 ok ? "success" : "failed");
        if (ok) {
            save_bonded_device(param->auth_cmpl.bda);
            bt_state_machine_event(BT_EVT_AUTH_SUCCESS,
                                   param->auth_cmpl.bda);
        } else {
            bt_state_machine_event(BT_EVT_AUTH_FAIL,
                                   param->auth_cmpl.bda);
        }
        if (s_auth_cb)
            s_auth_cb(param->auth_cmpl.bda, ok);
        break;
    }

    case ESP_BT_GAP_CFM_REQ_EVT: {
        /* SSP: IO_CAP_NONE  auto-confirm numeric comparison */
        ESP_LOGI(TAG, "SSP confirm req from %02x:%02x:%02x:%02x:%02x:%02x "
                 "(num=%lu) — auto-accepting",
                 param->cfm_req.bda[0], param->cfm_req.bda[1],
                 param->cfm_req.bda[2], param->cfm_req.bda[3],
                 param->cfm_req.bda[4], param->cfm_req.bda[5],
                 param->cfm_req.num_val);
        esp_bt_gap_ssp_confirm_reply(param->cfm_req.bda, true);
        break;
    }

    case ESP_BT_GAP_PIN_REQ_EVT: {
        /* Legacy PIN: reject — we use SSP only */
        ESP_LOGW(TAG, "Legacy PIN request from %02x:%02x:%02x:%02x:%02x:%02x "
                 "— rejecting (SSP only)",
                 param->pin_req.bda[0], param->pin_req.bda[1],
                 param->pin_req.bda[2], param->pin_req.bda[3],
                 param->pin_req.bda[4], param->pin_req.bda[5]);
        esp_bt_gap_pin_reply(param->pin_req.bda, false, 0, NULL);
        break;
    }

    /* ── ACL connection events ────────────────────────── */
    case ESP_BT_GAP_ACL_CONN_CMPL_STAT_EVT: {
        bool ok = (param->acl_conn_cmpl_stat.stat == ESP_BT_STATUS_SUCCESS);
        ESP_LOGI(TAG, "ACL conn to %02x:%02x:%02x:%02x:%02x:%02x — %s",
                 param->acl_conn_cmpl_stat.bda[0],
                 param->acl_conn_cmpl_stat.bda[1],
                 param->acl_conn_cmpl_stat.bda[2],
                 param->acl_conn_cmpl_stat.bda[3],
                 param->acl_conn_cmpl_stat.bda[4],
                 param->acl_conn_cmpl_stat.bda[5],
                 ok ? "ok" : "failed");

        if (ok) {
            portENTER_CRITICAL_SAFE(&s_lock);
            s_connected = true;
            memcpy(s_peer_bda, param->acl_conn_cmpl_stat.bda,
                   sizeof(esp_bd_addr_t));
            portEXIT_CRITICAL_SAFE(&s_lock);
            bt_state_machine_event(BT_EVT_ACL_CONNECTED,
                                   param->acl_conn_cmpl_stat.bda);
        } else {
            /* Link never came up — treat as disconnect for backoff */
            bt_state_machine_event(BT_EVT_ACL_DISCONNECTED,
                                   param->acl_conn_cmpl_stat.bda);
        }
        if (s_conn_cb)
            s_conn_cb(param->acl_conn_cmpl_stat.bda, ok, 0);
        break;
    }

    case ESP_BT_GAP_ACL_DISCONN_CMPL_STAT_EVT: {
        ESP_LOGI(TAG, "ACL disconn from %02x:%02x:%02x:%02x:%02x:%02x "
                 "reason=0x%04x",
                 param->acl_disconn_cmpl_stat.bda[0],
                 param->acl_disconn_cmpl_stat.bda[1],
                 param->acl_disconn_cmpl_stat.bda[2],
                 param->acl_disconn_cmpl_stat.bda[3],
                 param->acl_disconn_cmpl_stat.bda[4],
                 param->acl_disconn_cmpl_stat.bda[5],
                 param->acl_disconn_cmpl_stat.reason);

        portENTER_CRITICAL_SAFE(&s_lock);
        s_connected = false;
        memset(s_peer_bda, 0, sizeof(esp_bd_addr_t));
        portEXIT_CRITICAL_SAFE(&s_lock);

        /* Distinguish intentional vs unexpected for state machine */
        if (s_intentional_disc) {
            bt_state_machine_event(BT_EVT_DISCONNECT_DONE,
                                   param->acl_disconn_cmpl_stat.bda);
            s_intentional_disc = false;
        } else {
            bt_state_machine_event(BT_EVT_ACL_DISCONNECTED,
                                   param->acl_disconn_cmpl_stat.bda);
        }

        if (s_conn_cb)
            s_conn_cb(param->acl_disconn_cmpl_stat.bda, false,
                      (int)param->acl_disconn_cmpl_stat.reason);
        break;
    }

    default:
        break;
    }
}

/* ==================================================================
 *  Public API
 * ================================================================== */

esp_err_t bt_manager_init(void)
{
    if (s_initialised) {
        ESP_LOGW(TAG, "Already initialised");
        return ESP_OK;
    }

    esp_err_t ret;

    /* 1. Init and enable Bluetooth controller (Classic BT only) */
    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ret = esp_bt_controller_init(&bt_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "BT controller init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "BT controller enable failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* 2. Init and enable Bluedroid host stack */
    ret = esp_bluedroid_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Bluedroid init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_bluedroid_enable();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Bluedroid enable failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "BT controller + Bluedroid enabled");

    /* 3. Register GAP callback */
    ret = esp_bt_gap_register_callback(bt_gap_cb);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "GAP callback register failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* 4. Set scan mode: connectable + general discoverable */
    ret = esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE,
                                   ESP_BT_GENERAL_DISCOVERABLE);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Set scan mode failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* 5. SSP: IO_CAP_NONE → auto-confirm numeric comparison, no user I/O */
    esp_bt_io_cap_t iocap = ESP_BT_IO_CAP_NONE;
    ret = esp_bt_gap_set_security_param(ESP_BT_SP_IOCAP_MODE,
                                        &iocap, sizeof(iocap));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Set IO cap failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* 6. Device name */
    ret = esp_bt_gap_set_device_name(BT_DEVICE_NAME);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Set device name failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* 7. Class of Device: Networking / LAN Access Point */
    esp_bt_cod_t cod;
    cod.reserved_2 = 0;
    cod.minor      = 0;
    cod.major      = ESP_BT_COD_MAJOR_DEV_LAN_NAP;
    cod.service    = ESP_BT_COD_SRVC_NETWORKING;
    cod.reserved_8 = 0;
    ret = esp_bt_gap_set_cod(cod, ESP_BT_INIT_COD);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Set CoD failed: %s", esp_err_to_name(ret));
        /* Non-fatal — continue */
    }

    s_initialised = true;
    ESP_LOGI(TAG, "BT manager initialised — \"%s\", CoD LAN/NAP, SSP IO_CAP_NONE",
             BT_DEVICE_NAME);

    /* Initialise BT state machine and register reconnect handler */
    bt_state_machine_init();
    bt_state_register_reconnect_cb(bt_state_reconnect_handler);

    return ESP_OK;
}

bool bt_manager_is_initialised(void)
{
    return s_initialised;
}

/* ── Discovery ─────────────────────────────────────────────────── */

esp_err_t bt_manager_start_discovery(uint8_t duration_sec)
{
    if (!s_initialised) {
        ESP_LOGE(TAG, "Not initialised");
        return ESP_ERR_INVALID_STATE;
    }

    if (s_discovering) {
        ESP_LOGW(TAG, "Discovery already in progress — stop it first");
        return ESP_ERR_INVALID_STATE;
    }

    /* Check state machine allows this transition */
    if (bt_state_machine_get_state() != BT_STATE_IDLE) {
        ESP_LOGE(TAG, "Cannot start discovery in state %s",
                 bt_state_to_string(bt_state_machine_get_state()));
        return ESP_ERR_INVALID_STATE;
    }

    /* Convert seconds to 1.28 s units (ESP-IDF expects 0x01–0x30).
     * Clamp to valid range: 1–48 slots (≈ 1.3–61.4 s). */
    uint8_t inq_len;
    if (duration_sec == 0) {
        inq_len = DEFAULT_INQ_DURATION;
    } else {
        inq_len = (uint8_t)((duration_sec * 100u + 127u) / 128u);
        if (inq_len < 1)  inq_len = 1;
        if (inq_len > 48) inq_len = 48;
    }

    /* Cancel any in-flight remote-name resolution before clearing the
     * device list — resolve_next_name() walks s_devices via s_resolve_idx
     * and a stale index against a cleared array would read garbage. */
    if (s_resolving_names) {
        ESP_LOGW(TAG, "Cancelling in-progress name resolution for new discovery");
        s_resolving_names = false;
    }

    /* Clear the device list for this new inquiry session */
    portENTER_CRITICAL_SAFE(&s_lock);
    s_device_count = 0;
    memset(s_devices, 0, sizeof(s_devices));
    portEXIT_CRITICAL_SAFE(&s_lock);

    esp_err_t ret = esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY,
                                                inq_len, 0 /* unlimited */);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Start discovery failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "Discovery started (duration_sec=%u → inq_len=%u×1.28s ≈ %.1fs)",
             duration_sec, inq_len, inq_len * 1.28f);

    bt_state_machine_event(BT_EVT_START_DISCOVERY, NULL);
    return ESP_OK;
}

esp_err_t bt_manager_stop_discovery(void)
{
    if (!s_initialised) {
        return ESP_ERR_INVALID_STATE;
    }

    if (!s_discovering) {
        return ESP_OK;
    }

    esp_err_t ret = esp_bt_gap_cancel_discovery();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Stop discovery failed: %s", esp_err_to_name(ret));
    }
    /* State machine transition (DISCOVERING → IDLE) now handled
     * by the GAP callback's DISC_STATE_CHANGED_EVT handler */
    return ret;
}

esp_err_t bt_manager_get_discovered_devices(
    const bt_discovered_device_t **devices,
    size_t                         *count)
{
    if (!devices || !count)
        return ESP_ERR_INVALID_ARG;

    portENTER_CRITICAL_SAFE(&s_lock);
    *devices = s_devices;
    *count   = (size_t)s_device_count;
    portEXIT_CRITICAL_SAFE(&s_lock);

    return ESP_OK;
}

/* ── Connection ────────────────────────────────────────────────── */

esp_err_t bt_manager_connect(esp_bd_addr_t bda)
{
    if (!s_initialised) {
        ESP_LOGE(TAG, "Not initialised");
        return ESP_ERR_INVALID_STATE;
    }

    if (s_connected) {
        ESP_LOGW(TAG, "Already connected — disconnect first");
        return ESP_ERR_INVALID_STATE;
    }

    /* Check state machine allows connection from current state */
    bt_state_t cur = bt_state_machine_get_state();
    if (cur != BT_STATE_IDLE) {
        ESP_LOGE(TAG, "Cannot connect in state %s",
                 bt_state_to_string(cur));
        return ESP_ERR_INVALID_STATE;
    }

    /* Cancel any pending backoff — this is a manual connect */
    bt_state_cancel_backoff();

    ESP_LOGI(TAG, "Initiating L2CAP connect to %02x:%02x:%02x:%02x:%02x:%02x (PSM 0x%04x, free_heap=%"PRIu32")",
             bda[0], bda[1], bda[2], bda[3], bda[4], bda[5],
             BT_MGR_BNEP_PSM, esp_get_free_heap_size());

    /* Initiate L2CAP connection on BNEP PSM.
     * The TinyPAN HAL receives L2CAP events (OPEN/CLOSE) via its own
     * callback and manages the VFS fd.  We track the ACL-level state
     * via GAP's ACL_CONN_CMPL_STAT_EVT. */
    esp_err_t ret = esp_bt_l2cap_connect(
        ESP_BT_L2CAP_SEC_AUTHENTICATE | ESP_BT_L2CAP_SEC_ENCRYPT,
        BT_MGR_BNEP_PSM, bda);

    ESP_LOGI(TAG, "L2CAP connect returned: %s (%d)", esp_err_to_name(ret), ret);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "L2CAP connect failed: %s", esp_err_to_name(ret));
        return ret;
    }

    portENTER_CRITICAL_SAFE(&s_lock);
    memcpy(s_peer_bda, bda, sizeof(esp_bd_addr_t));
    portEXIT_CRITICAL_SAFE(&s_lock);

    ESP_LOGI(TAG, "Connecting to %02x:%02x:%02x:%02x:%02x:%02x on BNEP PSM 0x%04x",
             bda[0], bda[1], bda[2], bda[3], bda[4], bda[5],
             BT_MGR_BNEP_PSM);

    bt_state_machine_event(BT_EVT_START_PAIRING, bda);
    return ESP_OK;
}

esp_err_t bt_manager_disconnect(void)
{
    if (!s_initialised) {
        return ESP_ERR_INVALID_STATE;
    }

    if (!s_connected) {
        ESP_LOGW(TAG, "Not connected");
        return ESP_OK;
    }

    /* Signal to the state machine this is intentional */
    s_intentional_disc = true;
    bt_state_machine_event(BT_EVT_START_DISCONNECT, s_peer_bda);

    /* The TinyPAN HAL manages the underlying L2CAP fd and will close
     * it via close(fd) in hal_bt_deinit().  Here we just clear local
     * state; the ACL_DISCONN_CMPL_STAT_EVT will fire when the physical
     * link drops. */
    portENTER_CRITICAL_SAFE(&s_lock);
    s_connected = false;
    memset(s_peer_bda, 0, sizeof(esp_bd_addr_t));
    portEXIT_CRITICAL_SAFE(&s_lock);

    ESP_LOGI(TAG, "Disconnected (local state cleared)");
    return ESP_OK;
}

bool bt_manager_is_connected(void)
{
    bool connected;
    portENTER_CRITICAL_SAFE(&s_lock);
    connected = s_connected;
    portEXIT_CRITICAL_SAFE(&s_lock);
    return connected;
}

esp_err_t bt_manager_get_peer_bda(esp_bd_addr_t bda)
{
    portENTER_CRITICAL_SAFE(&s_lock);
    if (!s_connected) {
        portEXIT_CRITICAL_SAFE(&s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    memcpy(bda, s_peer_bda, sizeof(esp_bd_addr_t));
    portEXIT_CRITICAL_SAFE(&s_lock);
    return ESP_OK;
}

/* ── RF suspend/resume for WiFi coexistence ─────────────────────── */

esp_err_t bt_manager_suspend(void)
{
    if (!s_initialised) {
        ESP_LOGW(TAG, "suspend: not initialised — nothing to suspend");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Suspending BT stack (Bluedroid + controller) for WiFi RF exclusivity");

    /* Disable Bluedroid first, then the controller (required order). */
    esp_err_t ret1 = esp_bluedroid_disable();
    if (ret1 != ESP_OK) {
        ESP_LOGW(TAG, "Bluedroid disable failed: %s", esp_err_to_name(ret1));
    }

    esp_err_t ret2 = esp_bt_controller_disable();
    if (ret2 != ESP_OK) {
        ESP_LOGW(TAG, "BT controller disable failed: %s", esp_err_to_name(ret2));
    }

    if (ret1 == ESP_OK && ret2 == ESP_OK) {
        ESP_LOGI(TAG, "BT stack suspended — WiFi has exclusive RF access (coex=0)");
    }

    return (ret1 == ESP_OK && ret2 == ESP_OK) ? ESP_OK : ESP_FAIL;
}

esp_err_t bt_manager_resume(void)
{
    if (!s_initialised) {
        ESP_LOGE(TAG, "resume: not initialised");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Resuming BT stack (controller + Bluedroid)");

    /* Enable controller first, then Bluedroid (required order). */
    esp_err_t ret1 = esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT);
    if (ret1 != ESP_OK) {
        ESP_LOGE(TAG, "BT controller enable failed: %s", esp_err_to_name(ret1));
        return ret1;
    }

    esp_err_t ret2 = esp_bluedroid_enable();
    if (ret2 != ESP_OK) {
        ESP_LOGE(TAG, "Bluedroid enable failed: %s", esp_err_to_name(ret2));
        return ret2;
    }

    ESP_LOGI(TAG, "BT stack resumed — coex active again");
    return ESP_OK;
}

/* ── Pairing / Bonding persistence ─────────────────────────────── */

esp_err_t bt_manager_load_bonded_device(bt_bonded_device_t *device)
{
    if (!device) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;
    esp_err_t ret = nvs_open(BT_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "NVS open for bond load failed: %s", esp_err_to_name(ret));
        return ESP_ERR_NOT_FOUND;
    }

    /* Read BD address blob */
    size_t bda_len = sizeof(esp_bd_addr_t);
    ret = nvs_get_blob(handle, BT_NVS_KEY_BDA, device->bda, &bda_len);
    if (ret != ESP_OK) {
        ESP_LOGI(TAG, "No bonded device in NVS");
        nvs_close(handle);
        return ESP_ERR_NOT_FOUND;
    }

    if (bda_len != sizeof(esp_bd_addr_t)) {
        ESP_LOGW(TAG, "Bonded BDA has unexpected size %u (expected %u)",
                 (unsigned)bda_len, (unsigned)sizeof(esp_bd_addr_t));
        nvs_close(handle);
        return ESP_ERR_NOT_FOUND;
    }

    /* Read device name */
    size_t name_len = BT_BONDED_NAME_MAX;
    ret = nvs_get_str(handle, BT_NVS_KEY_NAME, device->name, &name_len);
    if (ret != ESP_OK) {
        /* Name is optional — default to empty */
        device->name[0] = '\0';
    }

    nvs_close(handle);

    ESP_LOGI(TAG, "Loaded bonded device: %02x:%02x:%02x:%02x:%02x:%02x (%s)",
             device->bda[0], device->bda[1], device->bda[2],
             device->bda[3], device->bda[4], device->bda[5],
             device->name[0] ? device->name : "unknown");

    return ESP_OK;
}

/* ── Callback registration ─────────────────────────────────────── */

void bt_manager_register_discovery_cb(bt_manager_discovery_cb_t cb)
{
    s_disc_cb = cb;
}

void bt_manager_register_connection_cb(bt_manager_conn_cb_t cb)
{
    s_conn_cb = cb;
}

void bt_manager_register_auth_cb(bt_manager_auth_cb_t cb)
{
    s_auth_cb = cb;
}
