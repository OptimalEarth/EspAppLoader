/*
 * EspAppLoader
 * File: AppLoaderProvisionMgr.h
 * Purpose: Copies and verifies the running loader in an inactive OTA slot.
 *
 * Copyright (c) 2026 Darryl Cornish
 */

// SPDX-License-Identifier: MPL-2.0

#pragma once
#include <cstring>
#include "esp_image_format.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"

namespace esp_apploader {

/** Embedded product identity; independent of Arduino's generic app descriptor. */
struct AppLoaderProvisionIdentity {
  char marker[64];
  char version[32];
  char build[48];
};

/** Does not change boot selection; the controller commits the handoff later. */
class AppLoaderProvisionMgr {
public:
  static int bootOption(const char *argument) {
    if (!argument || !argument[0] || argument[1]) return -1;
    return argument[0] == '0' ? 0 : (argument[0] == '1' ? 1 : -1);
  }

  static const char *identityMarker() {
    return "EspAppLoader.identity:1:8a7c6d21-4fa0-4e58-b2e5-7a2fc158d402";
  }

  const AppLoaderProvisionIdentity &sourceIdentity() const { return identity_; }

  esp_err_t inspectIdentity(const esp_partition_t *source,
                            const AppLoaderProvisionIdentity &expected) {
    identity_ = {};
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (!source || !running || source->type != ESP_PARTITION_TYPE_APP ||
        source->address != running->address || source->size != running->size)
      return ESP_ERR_INVALID_ARG;
    esp_partition_pos_t position = {};
    position.offset = source->address;
    position.size = source->size;
    esp_image_metadata_t metadata = {};
    metadata.start_addr = source->address;
    esp_err_t result = esp_image_verify(ESP_IMAGE_VERIFY, &position, &metadata);
    if (result != ESP_OK) return result;
    if (!metadata.image_len || metadata.image_len > source->size)
      return ESP_ERR_INVALID_SIZE;
    // Match the full product/version/build record against the one compiled
    // into this running loader. Carry bytes across flash-read boundaries.
    const size_t markerLength = strlen(identityMarker()) + 1;
    if (memcmp(expected.marker, identityMarker(), markerLength) ||
        !memchr(expected.version, '\0', sizeof(expected.version)) ||
        !expected.version[0] ||
        !memchr(expected.build, '\0', sizeof(expected.build)) || !expected.build[0])
      return ESP_ERR_INVALID_ARG;
    size_t carry = 0;
    for (uint32_t offset = 0; offset < metadata.image_len;) {
      const size_t remaining = metadata.image_len - offset;
      const size_t capacity = sizeof(buffer_) - carry;
      const size_t count = remaining < capacity ? remaining : capacity;
      result = esp_partition_read(source, offset, buffer_ + carry, count);
      if (result != ESP_OK) return result;
      const size_t total = carry + count;
      for (size_t i = 0; i + sizeof(expected) <= total; ++i) {
        if (!memcmp(buffer_ + i, &expected, sizeof(expected))) {
          memcpy(&identity_, buffer_ + i, sizeof(identity_));
          return ESP_OK;
        }
      }
      carry = total < sizeof(expected) - 1 ? total : sizeof(expected) - 1;
      memmove(buffer_, buffer_ + total - carry, carry);
      offset += count;
    }
    return ESP_ERR_NOT_FOUND;
  }

  esp_err_t copy(bool testMode, const esp_partition_t *source,
                 const esp_partition_t *destination,
                 const esp_partition_t *operational,
                 const AppLoaderProvisionIdentity &expected,
                 void (*service)() = nullptr) {
    if (testMode) return ESP_ERR_INVALID_STATE;
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *boot = esp_ota_get_boot_partition();
    if (!source || !destination || !operational || !running || !boot ||
        source->type != ESP_PARTITION_TYPE_APP ||
        destination->type != ESP_PARTITION_TYPE_APP ||
        source->address != running->address ||
        source->address != operational->address ||
        source->address != boot->address ||
        source->size != running->size ||
        source->size != operational->size || source->size == 0 ||
        destination->size == 0) return ESP_ERR_INVALID_ARG;
    // Reject aliases and overlapping ranges before any OTA erase.
    if (static_cast<uint64_t>(source->address) <
            static_cast<uint64_t>(destination->address) + destination->size &&
        static_cast<uint64_t>(destination->address) <
            static_cast<uint64_t>(source->address) + source->size)
      return ESP_ERR_INVALID_ARG;

    esp_partition_pos_t position = {};
    position.offset = source->address;
    position.size = source->size;
    esp_image_metadata_t metadata = {};
    metadata.start_addr = source->address;
    esp_err_t result = esp_image_verify(ESP_IMAGE_VERIFY, &position, &metadata);
    if (result != ESP_OK) return result;
    if (!metadata.image_len || metadata.image_len > source->size ||
        metadata.image_len > destination->size) return ESP_ERR_INVALID_SIZE;

    result = inspectIdentity(source, expected);
    if (result != ESP_OK) return result;

    esp_ota_handle_t handle = 0;
    result = esp_ota_begin(destination, metadata.image_len, &handle);
    if (result != ESP_OK) return result;
    for (uint32_t offset = 0; offset < metadata.image_len;) {
      const size_t remaining = metadata.image_len - offset;
      const size_t count = remaining < sizeof(buffer_) ? remaining : sizeof(buffer_);
      result = esp_partition_read(source, offset, buffer_, count);
      if (result == ESP_OK) result = esp_ota_write(handle, buffer_, count);
      if (result != ESP_OK) {
        esp_ota_abort(handle);
        return result;
      }
      offset += count;
      if (service) service();
    }
    // OTA end validates the destination and releases the handle on all paths.
    result = esp_ota_end(handle);
    if (result != ESP_OK) return result;

    // Compare every copied byte independently of the image's embedded digest.
    for (uint32_t offset = 0; offset < metadata.image_len;) {
      const size_t remaining = metadata.image_len - offset;
      const size_t count = remaining < sizeof(compare_) ? remaining : sizeof(compare_);
      result = esp_partition_read(source, offset, buffer_, count);
      if (result == ESP_OK)
        result = esp_partition_read(destination, offset, compare_, count);
      if (result != ESP_OK) return result;
      if (memcmp(buffer_, compare_, count) != 0) return ESP_ERR_INVALID_CRC;
      offset += count;
      if (service) service();
    }
    return ESP_OK;
  }
private:
  AppLoaderProvisionIdentity identity_ = {};
  uint8_t buffer_[4096] = {};
  uint8_t compare_[512] = {};
};

} // namespace esp_apploader
