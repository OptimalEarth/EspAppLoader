/*
 * EspAppLoader
 * File: AppLoaderProvisionState.h
 * Purpose: Persists the provisioning handoff and loader hold across resets.
 *
 * Copyright (c) 2026 Darryl Cornish
 */

// SPDX-License-Identifier: MPL-2.0

#pragma once
#include <cstdint>
#include <cstring>
#include <Preferences.h>

namespace esp_apploader {

template <size_t FilenameCapacity>
class AppLoaderProvisionStateT {
public:
  enum Mode : uint32_t { None = 0, Install = 1, Hold = 2 };
  struct Record {
    uint32_t version = 1;
    uint32_t mode = None;
    uint32_t size = 0;
    uint8_t digest[32] = {};
    char filename[FilenameCapacity] = {};
  };

  // A single NVS blob avoids partially updated filename/mode combinations.
  bool load(Record &record) {
    record = Record{};
    Preferences preferences;
    if (!preferences.begin("ldrprovision", false)) return false;
    if (!preferences.isKey("handoff")) {
      preferences.end();
      return true;
    }
    const bool loaded = preferences.getBytesLength("handoff") == sizeof(record) &&
        preferences.getBytes("handoff", &record, sizeof(record)) == sizeof(record);
    preferences.end();
    return loaded && record.version == 1 && record.mode <= Hold &&
        memchr(record.filename, '\0', sizeof(record.filename)) != nullptr &&
        (record.mode != Install || (record.filename[0] && record.size));
  }

  bool save(const Record &record) {
    Preferences preferences;
    if (!preferences.begin("ldrprovision", false)) return false;
    const bool saved = preferences.putBytes("handoff", &record, sizeof(record)) == sizeof(record);
    preferences.end();
    return saved;
  }

  bool clear() { return save(Record{}); }
};

} // namespace esp_apploader
