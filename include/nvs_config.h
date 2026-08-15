/* NVS config helpers for TinyPAN v2. */
#pragma once
#include <stddef.h>
#include "esp_err.h"

esp_err_t set_config_param_blob(const char *key, const void *data, size_t len);
