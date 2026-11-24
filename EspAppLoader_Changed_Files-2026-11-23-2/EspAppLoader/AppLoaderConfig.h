/*
 * EspAppLoader
 * File: AppLoaderConfig.h
 * Purpose: Configures the loader build mode, partition labels, and recovery marker.
 *
 * Copyright (c) 2026 Darryl Cornish
 */

// SPDX-License-Identifier: MPL-2.0

#pragma once
// Operational bench profile. Provision this image into app1.
// Normal flash/boot mutations require running in the configured loader partition.
// appLdr2App1 is a guarded provisioning path from app0 to inactive app1.
#define ESP_APPLOADER_TEST_MODE 0
#define ESP_APPLOADER_LOADER_LABEL "app1"
#define ESP_APPLOADER_OPERATIONAL_LABEL "app0"
#define ESP_APPLOADER_RECOVERY_MARKER "/RECOVERY.WTG"
