/*
 * EspAppLoader
 * File: esp_partition.h
 * Purpose: Supplies partition API stubs for host tests.
 *
 * Copyright (c) 2026 Darryl Cornish
 */

// SPDX-License-Identifier: MPL-2.0

#pragma once
#include <cstdint>
#include <cstddef>
using esp_err_t = int;
constexpr int ESP_OK=0, ESP_ERR_INVALID_ARG=1, ESP_ERR_INVALID_SIZE=2;
constexpr int ESP_PARTITION_TYPE_APP=0;
constexpr int ESP_ERR_INVALID_STATE=3, ESP_ERR_INVALID_CRC=4, ESP_ERR_NOT_FOUND=5;
struct esp_partition_t { int type; uint32_t address; uint32_t size; char label[17] = {}; };
extern int hashResult;
inline int esp_partition_get_sha256(const esp_partition_t *, uint8_t *) { return hashResult; }

esp_err_t esp_partition_read(const esp_partition_t *, size_t, void *, size_t);
