/*
 * EspAppLoader
 * File: AppLoaderFlashImageMgr.h
 * Purpose: Inspects an existing flash image and reports validation and identity data.
 *
 * Copyright (c) 2026 Darryl Cornish
 */

// SPDX-License-Identifier: MPL-2.0

#pragma once
#include "esp_image_format.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"

namespace esp_apploader {

/** Read-only validation of an existing application; no SD or boot changes. */
class AppLoaderFlashImageMgr {
public:
  esp_err_t inspect(const esp_partition_t *target,
                    const esp_partition_t *running) {
    valid_ = false;
    length_ = 0;
    if (!target || !running || target->type != ESP_PARTITION_TYPE_APP ||
        target->address == running->address) return ESP_ERR_INVALID_ARG;
    esp_partition_pos_t position = {};
    position.offset = target->address;
    position.size = target->size;
    esp_image_metadata_t metadata = {};
    metadata.start_addr = position.offset;
    esp_err_t result = esp_image_verify(ESP_IMAGE_VERIFY, &position, &metadata);
    if (result != ESP_OK) return result;
    if (!metadata.image_len || metadata.image_len > target->size)
      return ESP_ERR_INVALID_SIZE;
    result = esp_ota_get_partition_description(target, &description_);
    if (result != ESP_OK) return result;
    result = esp_partition_get_sha256(target, digest_);
    if (result != ESP_OK) return result;
    length_ = metadata.image_len;
    valid_ = true;
    return ESP_OK;
  }
  bool valid() const { return valid_; }
  uint32_t length() const { return length_; }
  const esp_app_desc_t &description() const { return description_; }
  const uint8_t *digest() const { return digest_; }
private:
  bool valid_ = false;
  uint32_t length_ = 0;
  esp_app_desc_t description_ = {};
  uint8_t digest_[32] = {};
};

} // namespace esp_apploader
