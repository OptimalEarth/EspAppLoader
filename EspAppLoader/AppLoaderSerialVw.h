/*
 * EspAppLoader
 * File: AppLoaderSerialVw.h
 * Purpose: Formats loader mode and recovery information for the command console.
 *
 * Copyright (c) 2026 Darryl Cornish
 */

// SPDX-License-Identifier: MPL-2.0

#pragma once
#include "AppLoaderChannels.h"

namespace esp_apploader {

class AppLoaderSerialVw {
public:
  static void printMode(AppLoaderCmdChannel &out, bool mutationAllowed) {
    out.write(ESP_APPLOADER_TEST_MODE
      ? "Mode: APP0 BENCH TEST. Install and boot selection DISABLED.\r\n"
      : "Mode: production profile.\r\n");
    out.write(mutationAllowed ? "Flash/boot policy: enabled.\r\n"
                             : "Flash/boot policy: blocked. File operations remain available.\r\n");
  }
  static void printRecovery(AppLoaderCmdChannel &out, bool pinLow, bool markerFound) {
    out.write(pinLow ? "GPIO4 recovery input: LOW (asserted).\r\n"
                     : "GPIO4 recovery input: HIGH (inactive).\r\n");
    out.write(markerFound ? "SD recovery marker: present.\r\n"
                          : "SD recovery marker: absent or SD unavailable.\r\n");
    out.write("Inspection only; startup redirection and automatic rollback are not implemented.\r\n");
  }
};

} // namespace esp_apploader
