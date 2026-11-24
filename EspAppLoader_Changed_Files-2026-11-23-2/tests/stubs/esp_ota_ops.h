/*
 * EspAppLoader
 * File: esp_ota_ops.h
 * Purpose: Supplies OTA API stubs for host tests.
 *
 * Copyright (c) 2026 Darryl Cornish
 */

// SPDX-License-Identifier: MPL-2.0

#pragma once
#include "esp_partition.h"
struct esp_app_desc_t { char project_name[32]; char version[32]; };
extern int descriptionResult;
inline int esp_ota_get_partition_description(const esp_partition_t *, esp_app_desc_t *) { return descriptionResult; }

using esp_ota_handle_t = uint32_t;
const esp_partition_t *esp_ota_get_running_partition();
const esp_partition_t *esp_ota_get_boot_partition();
esp_err_t esp_ota_begin(const esp_partition_t *, size_t, esp_ota_handle_t *);
esp_err_t esp_ota_write(esp_ota_handle_t, const void *, size_t);
esp_err_t esp_ota_end(esp_ota_handle_t);
esp_err_t esp_ota_abort(esp_ota_handle_t);
