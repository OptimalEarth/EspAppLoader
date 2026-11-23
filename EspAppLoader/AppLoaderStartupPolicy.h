/*
 * EspAppLoader
 * File: AppLoaderStartupPolicy.h
 * Purpose: Defines when startup falls back to the existing operational image.
 *
 * Copyright (c) 2026 Darryl Cornish
 */

// SPDX-License-Identifier: MPL-2.0

#pragma once

namespace esp_apploader {

/** Recovery takes precedence over storage-independent startup. */
class AppLoaderStartupPolicy {
public:
  static constexpr bool fallback(bool sdReady, bool sdHasApps,
                                 bool recoveryRequested) {
    return !recoveryRequested && (!sdReady || !sdHasApps);
  }
};

} // namespace esp_apploader
