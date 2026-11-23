/*
 * EspAppLoader
 * File: EspAppLoader.ino
 * Purpose: Connects the Arduino sketch entry points to EspAppLoader.
 *
 * Copyright (c) 2026 Darryl Cornish
 */

// SPDX-License-Identifier: MPL-2.0

#include "AppLoaderMgr.h"
void setup() { esp_apploader::appLoaderSetup(); }
void loop() { esp_apploader::appLoaderLoop(); }
