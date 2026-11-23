/*
 * EspAppLoader
 * File: WTgPlatform.h
 * Purpose: Implements peripheral bring-up for the WaterThing hardware profile.
 *
 * Copyright (c) 2026 Darryl Cornish
 */

// SPDX-License-Identifier: MPL-2.0

#pragma once
#include "AppLoaderCommon.h"

namespace esp_apploader {

/** WaterThing 0.5 device results retained for status and bringup? output. */
struct WTgBringupResults {
  bool sd = false;
  bool rtc = false;
  bool lora = false;
  bool barometer = false;
  bool ads1115 = false;
  uint8_t loraVersion = 0;
  uint8_t barometerAddress = 0;
  uint8_t barometerChipId = 0;
};

/** Low-level WaterThing 0.5 peripheral initialization and identity checks. */
class WTgBoardBringupMgr {
public:
  WTgBoardBringupMgr() : loraSpi_(HSPI) {}

  void begin(bool sdReady, bool rtcReady) {
    results_ = {};
    results_.sd = sdReady;
    results_.rtc = rtcReady;
    Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
    initializeBarometer();
    initializeAds1115();
    initializeLoRa();
  }

  const WTgBringupResults &results() const { return results_; }

  template <typename Channel>
  void printTo(Channel &channel) const {
    char line[192];
    channel.write("WaterThing 0.5 board bring-up:\r\n");
    snprintf(line, sizeof(line), "  SD:         %s  VSPI CS=GPIO%u\r\n",
             passFail(results_.sd), SD_CS_PIN);
    channel.write(line);
    snprintf(line, sizeof(line), "  RTC:        %s  DS3231 address=0x68\r\n",
             passFail(results_.rtc));
    channel.write(line);
    snprintf(line, sizeof(line),
             "  LoRa:       %s  SX1276 version=0x%02X HSPI CS=GPIO%u\r\n",
             passFail(results_.lora), results_.loraVersion, LORA_CS_PIN);
    channel.write(line);
    const char *baroName = results_.barometerChipId == 0x60 ? "BME280" :
                           results_.barometerChipId == 0x58 ? "BMP280" : "unknown";
    snprintf(line, sizeof(line),
             "  Barometer:  %s  %s address=0x%02X chip_id=0x%02X\r\n",
             passFail(results_.barometer), baroName, results_.barometerAddress,
             results_.barometerChipId);
    channel.write(line);
    snprintf(line, sizeof(line), "  ADS1115:    %s  address=0x%02X\r\n",
             passFail(results_.ads1115), ADS1115_ADDRESS);
    channel.write(line);
    snprintf(line, sizeof(line), "  OVERALL:    %s\r\n", passFail(allPassed()));
    channel.write(line);
  }

  bool allPassed() const {
    return results_.sd && results_.rtc && results_.lora &&
           results_.barometer && results_.ads1115;
  }

private:
  SPIClass loraSpi_;
  WTgBringupResults results_;

  static const char *passFail(bool passed) { return passed ? "PASS" : "FAIL"; }

  static bool readI2cRegister(uint8_t address, uint8_t reg, uint8_t &value) {
    Wire.beginTransmission(address);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0) return false;
    if (Wire.requestFrom(address, static_cast<uint8_t>(1)) != 1) return false;
    value = Wire.read();
    return true;
  }

  static bool writeI2cRegister16(uint8_t address, uint8_t reg, uint16_t value) {
    Wire.beginTransmission(address);
    Wire.write(reg);
    Wire.write(static_cast<uint8_t>(value >> 8));
    Wire.write(static_cast<uint8_t>(value));
    return Wire.endTransmission() == 0;
  }

  void initializeBarometer() {
    const uint8_t addresses[] = {0x76, 0x77};
    for (uint8_t address : addresses) {
      uint8_t chipId = 0;
      if (readI2cRegister(address, 0xD0, chipId) &&
          (chipId == 0x58 || chipId == 0x60)) {
        results_.barometer = true;
        results_.barometerAddress = address;
        results_.barometerChipId = chipId;
        // Temperature and pressure x1 oversampling, normal mode.
        Wire.beginTransmission(address);
        Wire.write(0xF4);
        Wire.write(0x27);
        if (Wire.endTransmission() != 0) results_.barometer = false;
        return;
      }
    }
  }

  void initializeAds1115() {
    // Single-shot AIN0, +/-4.096 V, 128 SPS. Reading back the configuration
    // distinguishes a responding ADS1115 from a simple I2C ACK.
    if (!writeI2cRegister16(ADS1115_ADDRESS, 0x01, 0xC383)) return;
    uint8_t msb = 0;
    results_.ads1115 = readI2cRegister(ADS1115_ADDRESS, 0x01, msb) &&
                       (msb & 0x7F) == 0x43;
  }

  uint8_t readLoRaRegister(uint8_t reg) {
    loraSpi_.beginTransaction(SPISettings(4000000, MSBFIRST, SPI_MODE0));
    digitalWrite(LORA_CS_PIN, LOW);
    loraSpi_.transfer(reg & 0x7F);
    const uint8_t value = loraSpi_.transfer(0x00);
    digitalWrite(LORA_CS_PIN, HIGH);
    loraSpi_.endTransaction();
    return value;
  }

  void writeLoRaRegister(uint8_t reg, uint8_t value) {
    loraSpi_.beginTransaction(SPISettings(4000000, MSBFIRST, SPI_MODE0));
    digitalWrite(LORA_CS_PIN, LOW);
    loraSpi_.transfer(reg | 0x80);
    loraSpi_.transfer(value);
    digitalWrite(LORA_CS_PIN, HIGH);
    loraSpi_.endTransaction();
  }

  void initializeLoRa() {
    pinMode(LORA_CS_PIN, OUTPUT);
    digitalWrite(LORA_CS_PIN, HIGH);
    pinMode(LORA_DIO0_PIN, INPUT);
    pinMode(LORA_DIO1_PIN, INPUT);
    pinMode(LORA_RESET_PIN, OUTPUT);
    digitalWrite(LORA_RESET_PIN, HIGH);
    loraSpi_.begin(LORA_SCK_PIN, LORA_MISO_PIN, LORA_MOSI_PIN, LORA_CS_PIN);
    digitalWrite(LORA_RESET_PIN, LOW);
    delay(2);
    digitalWrite(LORA_RESET_PIN, HIGH);
    delay(10);
    results_.loraVersion = readLoRaRegister(0x42);
    results_.lora = results_.loraVersion == 0x12;
    if (results_.lora) {
      // LoRa sleep mode at 915 MHz; RF remains inactive after bring-up.
      writeLoRaRegister(0x01, 0x80);
      writeLoRaRegister(0x06, 0xE4);
      writeLoRaRegister(0x07, 0xC0);
      writeLoRaRegister(0x08, 0x00);
    }
  }
};

} // namespace esp_apploader
