/*
 * EspAppLoader
 * File: AppLoaderMgr.h
 * Purpose: Declares the loader setup and loop entry points.
 *
 * Copyright (c) 2026 Darryl Cornish
 */

// SPDX-License-Identifier: MPL-2.0

#pragma once

namespace esp_apploader {

void appLoaderSetup();
void appLoaderLoop();

} // namespace esp_apploader
