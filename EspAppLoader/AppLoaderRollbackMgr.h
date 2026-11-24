/*
 * EspAppLoader
 * File: AppLoaderRollbackMgr.h
 * Purpose: Confirms the running loader after essential startup checks pass.
 * Copyright (c) 2026 Darryl Cornish
 */
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "esp_ota_ops.h"

namespace esp_apploader {
class AppLoaderRollbackMgr {
public:
  static esp_err_t confirm(bool startupReady) {
    if (!startupReady) return ESP_ERR_INVALID_STATE;
#if defined(CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE) && CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (!running) return ESP_ERR_INVALID_STATE;
    esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
    const esp_err_t result = esp_ota_get_state_partition(running, &state);
    // A USB-installed image may have no OTA state record.
    if (result == ESP_ERR_NOT_FOUND) return ESP_OK;
    if (result != ESP_OK) return result;
    if (state == ESP_OTA_IMG_PENDING_VERIFY)
      return esp_ota_mark_app_valid_cancel_rollback();
    if (state != ESP_OTA_IMG_VALID && state != ESP_OTA_IMG_UNDEFINED)
      return ESP_ERR_INVALID_STATE;
#endif
    return ESP_OK;
  }
};
} // namespace esp_apploader
