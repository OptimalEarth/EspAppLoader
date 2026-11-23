/*
 * EspAppLoader
 * File: AppLoaderPolicy.h
 * Purpose: Defines when the loader may write images or change the boot partition.
 *
 * Copyright (c) 2026 Darryl Cornish
 */

// SPDX-License-Identifier: MPL-2.0

#pragma once

namespace esp_apploader {

// Pure policy: independently testable without ESP32 or Arduino dependencies.
class AppLoaderPolicy {
public:
  static constexpr bool mayMutate(bool testMode, bool runningFound,
      bool loaderFound, bool targetFound, bool runningIsLoader,
      bool targetDiffersFromRunning) {
    return !testMode && runningFound && loaderFound && targetFound &&
        runningIsLoader && targetDiffersFromRunning;
  }
};

} // namespace esp_apploader
