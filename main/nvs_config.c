/* NVS config helpers for TinyPAN v2.
 * Adapted from martin-ger/esp32_nat_router.
 */
#include <string.h>
#include "esp_log.h"
#include "nvs.h"
#include "router_config.h"

static const char *TAG = "nvs_cfg";

esp_err_t set_config_param_blob(const char *key, const void *data, size_t len) {
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(PARAM_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        return err;
    }
    err = nvs_set_blob(nvs, key, data, len);
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return err;
}
