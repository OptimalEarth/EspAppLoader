/*
 * EspAppLoader
 * File: AppLoaderCommon.h
 * Purpose: Defines shared configuration, data types, and image hashing support.
 */

#pragma once
#include "AppLoaderConfig.h"
/*
  WaterThing AppLoader

  Designed by Darryl Cornish.
  Written by Darryl Cornish with assistance from various AI systems.

  Copyright (c) 2026 Darryl Cornish
  SPDX-License-Identifier: MPL-2.0

  See LICENSE in the repository root.
*/

/*
  AppLoader 0.5.0-test1: incremental MVC migration for app0 bench testing.
  See AppLoaderConfig.h. Installation and boot selection are blocked in test mode.
  Operational recovery and automatic rollback are not implemented.
*/

#define ESP_APPLOADER_NAME       "EspAppLoader"
#define ESP_APPLOADER_VERSION    "2026-11-23-2"
#define ESP_APPLOADER_BUILD_ID   "2026-11-23-2"

#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <Preferences.h>
#include <Wire.h>
#include <RTClib.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ctype.h>
#include "esp_partition.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "mbedtls/sha256.h"
#include "mbedtls/version.h"
#include "WTgInteruptBtn.h"
#include "WTgTimerInterruptMgr.h"
#include "WTgUiFeedbackMgr.h"

namespace esp_apploader {

// uWatt hardware pin assignments and host-interface settings.
static constexpr uint8_t SD_CS_PIN = 5;
static constexpr uint8_t I2C_SDA_PIN = 21;
static constexpr uint8_t I2C_SCL_PIN = 22;
static constexpr uint8_t LORA_CS_PIN = 15;
static constexpr uint8_t LORA_SCK_PIN = 14;
static constexpr uint8_t LORA_MISO_PIN = 12;
static constexpr uint8_t LORA_MOSI_PIN = 13;
static constexpr uint8_t LORA_RESET_PIN = 0;
static constexpr uint8_t LORA_DIO0_PIN = 34;
static constexpr uint8_t LORA_DIO1_PIN = 35;
static constexpr uint8_t ADS1115_ADDRESS = 0x48;
static constexpr uint8_t CTL0_PIN = 25;
static constexpr uint8_t CTL1_PIN = 26;
static constexpr uint8_t CTL2_PIN = 32;
static constexpr uint8_t CTL3_PIN = 33;
static constexpr uint32_t USB_SERIAL_BAUD = 115200;
static constexpr char BLE_ADMIN_KEY[] = "4034837251";  // Ten-digit privileged-command key.
static constexpr uint32_t BLE_AUTH_TIMEOUT_MS = 60000UL;
static constexpr uint32_t BLE_TRUST_SECONDS = 24UL * 60UL * 60UL; // Authorized-peer trust lifetime.
static constexpr uint8_t BLE_ENABLE_BUTTON_PIN = 4;
static constexpr uint8_t BLE_FEEDBACK_LED_PIN = 2;
static constexpr uint32_t BLE_BUTTON_PRESS_PERIOD_MS = 75UL;
static constexpr uint32_t BLE_REED_TOGGLE_MIN_US = 2000000UL;
static constexpr uint32_t BLE_REED_TOGGLE_MAX_US = 10000000UL;
static constexpr uint16_t BLE_BUTTON_MAX_VALIDATION_TRANSITIONS = 8U;
static constexpr uint32_t BLE_BUTTON_RELEASE_IGNORE_US = 1000UL;
static constexpr uint32_t BLE_UI_TIMER_TICK_US = 10000UL;
static constexpr uint32_t BLE_UI_FLASH_ON_US = 100000UL;
static constexpr uint32_t BLE_UI_FLASH_OFF_US = 100000UL;
static constexpr uint32_t BLE_UI_POST_RELEASE_QUIET_US = 300000UL; // Dark interval before result flashes.
static constexpr uint32_t BLE_MODE_LED_ON_US = 1000000UL;
static constexpr uint32_t BLE_MODE_LED_OFF_US = 1000000UL;

// SD-card directory and transactional-file locations used by AppLoader.
static constexpr char APPS_DIR[] = "/apps";
static constexpr char INCOMING_DIR[] = "/apps/.incoming";
static constexpr char BOOT_CFG_PATH[] = "/boot.cfg";
static constexpr char BOOT_CFG_TMP_PATH[] = "/boot.cfg.new";
static constexpr char BOOT_CFG_BACKUP_PATH[] = "/boot.cfg.bak";
static constexpr char APPLOADER_DIR[] = "/apploader";
static constexpr char EVENT_LOG_PATH[] = "/apploader/apploader.log";
static constexpr char INSTALL_LOG_PATH[] = "/apploader/install_history_v2.csv";
static constexpr char INSTALLED_CFG_PATH[] = "/apploader/installed.cfg";
static constexpr char INSTALLED_TMP_PATH[] = "/apploader/installed.tmp";
static constexpr char TRANSACTION_CFG_PATH[] = "/apploader/install_transaction.cfg";
static constexpr char TRANSACTION_TMP_PATH[] = "/apploader/install_transaction.tmp";
static constexpr char SETUP_CFG_PATH[] = "/setup.cfg";

// Default WTg profile roles; configurable in AppLoaderConfig.h.
// Explicit roles for this firmware version. Do not use a generic "next OTA"
// target because AppLoader must never overwrite itself.
static constexpr const char *APPLOADER_PARTITION_LABEL = ESP_APPLOADER_LOADER_LABEL;
static constexpr const char *APP_TARGET_PARTITION_LABEL = ESP_APPLOADER_OPERATIONAL_LABEL;

// Nordic UART Service identifiers used for the BLE command console.
static constexpr char BLE_SERVICE_UUID[] = "6E400001-B5A3-F393-E0A9-E50E24DCCA9E";
static constexpr char BLE_RX_UUID[]      = "6E400002-B5A3-F393-E0A9-E50E24DCCA9E";
static constexpr char BLE_TX_UUID[]      = "6E400003-B5A3-F393-E0A9-E50E24DCCA9E";

// Fixed capacities keep command, repository, and flash operations deterministic.
static constexpr size_t CMD_BUFFER_SIZE = 192;
static constexpr size_t MAX_APP_FILES = 24;
static constexpr size_t MAX_APP_FILENAME = 96;
static constexpr size_t FLASH_BUFFER_SIZE = 4096;
static constexpr char APP_UPLOAD_IP_TEXT[] = "192.168.4.1";
static constexpr uint8_t ESP_IMAGE_MAGIC = 0xE9;
static constexpr uint32_t ESP_APP_DESC_MAGIC = 0xABCD5432;

/** Observable phase of the AppLoader command/install workflow. */
enum class AppLoaderState : uint8_t {
  Starting,
  Idle,
  ScanningApps,
  UploadMode,
  ReceivingApp,
  ValidatingSource,
  AwaitingConfirmation,
  Installing,
  ValidatingTarget,
  ReadyToBoot,
  InstallationFailed
};

/** Return the stable diagnostic label for an AppLoader workflow state. */
static const char *stateName(AppLoaderState state) {
  switch (state) {
    case AppLoaderState::Starting:             return "Starting";
    case AppLoaderState::Idle:                 return "Idle";
    case AppLoaderState::ScanningApps:         return "ScanningApps";
    case AppLoaderState::UploadMode:           return "UploadMode";
    case AppLoaderState::ReceivingApp:         return "ReceivingApp";
    case AppLoaderState::ValidatingSource:     return "ValidatingSource";
    case AppLoaderState::AwaitingConfirmation: return "AwaitingConfirmation";
    case AppLoaderState::Installing:           return "Installing";
    case AppLoaderState::ValidatingTarget:     return "ValidatingTarget";
    case AppLoaderState::ReadyToBoot:          return "ReadyToBoot";
    case AppLoaderState::InstallationFailed:   return "InstallationFailed";
  }
  return "Unknown";
}

/** Start a SHA-256 calculation across mbedTLS 2.x and 3.x APIs. */
static int wtgSha256Start(mbedtls_sha256_context *context) {
#if MBEDTLS_VERSION_MAJOR >= 3
  return mbedtls_sha256_starts(context, 0);
#else
  return mbedtls_sha256_starts_ret(context, 0);
#endif
}

/** Add one byte range to a portable SHA-256 calculation. */
static int wtgSha256Update(mbedtls_sha256_context *context,
                           const uint8_t *data, size_t length) {
#if MBEDTLS_VERSION_MAJOR >= 3
  return mbedtls_sha256_update(context, data, length);
#else
  return mbedtls_sha256_update_ret(context, data, length);
#endif
}

/** Finish a portable SHA-256 calculation and write its 32-byte digest. */
static int wtgSha256Finish(mbedtls_sha256_context *context, uint8_t output[32]) {
#if MBEDTLS_VERSION_MAJOR >= 3
  return mbedtls_sha256_finish(context, output);
#else
  return mbedtls_sha256_finish_ret(context, output);
#endif
}

// The standard ESP application descriptor is located at offset 0x20 in a
// normal ESP32 application image. This local definition avoids depending on
// Arduino-core-private headers merely to display build metadata.
/** On-flash layout of the ESP application descriptor at image offset 0x20. */
struct AppLoaderEmbeddedAppDesc {
  uint32_t magicWord;
  uint32_t secureVersion;
  uint32_t reserved1[2];
  char version[32];
  char projectName[32];
  char buildTime[16];
  char buildDate[16];
  char idfVersion[32];
  uint8_t appElfSha256[32];
  uint32_t reserved2[20];
};

/** Metadata and validation results collected for one candidate application. */
struct AppLoaderAppImage {
  char filename[MAX_APP_FILENAME];
  uint32_t fileSize;
  char appName[33];
  char appVersion[33];
  char buildId[40];
  uint8_t sha256[32];
  uint8_t flashSha256[32];
  bool metadataAvailable;
  bool hashAvailable;
  bool flashHashAvailable;
  bool flashHashMatches;
};

} // namespace esp_apploader
