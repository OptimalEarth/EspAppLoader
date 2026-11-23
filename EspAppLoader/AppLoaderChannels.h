/*
 * EspAppLoader
 * File: AppLoaderChannels.h
 * Purpose: Implements the serial and BLE command channels and BLE authorization.
 *
 * Copyright (c) 2026 Darryl Cornish
 */

// SPDX-License-Identifier: MPL-2.0

#pragma once
#include "AppLoaderCommon.h"

namespace esp_apploader {

/** Transport-independent, line-oriented AppLoader command interface. */
class AppLoaderCmdChannel {
public:
  virtual ~AppLoaderCmdChannel() = default;
  virtual void begin() = 0;
  virtual void poll() = 0;
  virtual bool getLine(char *line, size_t capacity) = 0;
  virtual void write(const char *text) = 0;
  virtual const char *name() const = 0;
};

/** USB serial implementation of the AppLoader command interface. */
class AppLoaderSerialCmdChannel : public AppLoaderCmdChannel {
public:
  void begin() override {
    Serial.begin(USB_SERIAL_BAUD);
    delay(200);
  }

  void poll() override {
    while (Serial.available() > 0) {
      const char c = static_cast<char>(Serial.read());
      accept(c);
    }
  }

  bool getLine(char *line, size_t capacity) override {
    if (!lineReady_ || capacity == 0) return false;
    strlcpy(line, completed_, capacity);
    lineReady_ = false;
    completed_[0] = '\0';
    return true;
  }

  void write(const char *text) override {
    Serial.print(text != nullptr ? text : "");
  }

  const char *name() const override { return "Serial"; }

private:
  char incoming_[CMD_BUFFER_SIZE] = {};
  char completed_[CMD_BUFFER_SIZE] = {};
  size_t length_ = 0;
  bool lineReady_ = false;

  /** Accumulate one character and publish one complete, newline-terminated command. */
  void accept(char c) {
    if (c == '\r' || c == '\n') {
      if (length_ > 0 && !lineReady_) {
        incoming_[length_] = '\0';
        strlcpy(completed_, incoming_, sizeof(completed_));
        lineReady_ = true;
      }
      length_ = 0;
      incoming_[0] = '\0';
      return;
    }
    if (lineReady_) return;
    if (length_ + 1 < sizeof(incoming_)) incoming_[length_++] = c;
  }
};

/** Nordic-UART BLE command channel with a reusable, lazily allocated object tree. */
class AppLoaderBleCmdChannel : public AppLoaderCmdChannel,
                         public BLEServerCallbacks,
                         public BLECharacteristicCallbacks {
public:
  void setDeviceName(int unitNum) {
    snprintf(deviceName_, sizeof(deviceName_), "%s-%d", ESP_APPLOADER_NAME, unitNum);
  }

  void begin() override {
    // BLE is initialized lazily by activate() after the physical gesture.
  }

  /** Start advertising, creating the BLE server once on the first activation. */
  bool activate() {
    resetSessionState();

    if (initialized_) {
      BLEAdvertising *advertising = BLEDevice::getAdvertising();
      advertising->stop();
      delay(50);
      advertising->start();
      enabled_ = true;
      Serial.print("AppLoader BLE advertising restarted. Free heap=");
      Serial.println(ESP.getFreeHeap());
      return true;
    }

    if (initializationFailed_) {
      Serial.println(
          "ERROR: AppLoader BLE one-time initialization previously failed; reboot required.");
      return false;
    }

    Serial.print("Initializing AppLoader one-time BLE object tree. Free heap=");
    Serial.println(ESP.getFreeHeap());
    BLEDevice::init(deviceName_);
    BLEDevice::setMTU(185);
    server_ = BLEDevice::createServer();
    if (server_ == nullptr) return failInitialization("server");
    server_->setCallbacks(this);
    BLEService *service = server_->createService(BLE_SERVICE_UUID);
    if (service == nullptr) return failInitialization("service");
    tx_ = service->createCharacteristic(BLE_TX_UUID, BLECharacteristic::PROPERTY_NOTIFY);
    BLECharacteristic *rx = service->createCharacteristic(
      BLE_RX_UUID, BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR);
    if (tx_ == nullptr || rx == nullptr) return failInitialization("characteristic");
    tx_->addDescriptor(&txDescriptor_);
    rx->setCallbacks(this);
    service->start();
    BLEAdvertising *advertising = BLEDevice::getAdvertising();
    advertising->addServiceUUID(BLE_SERVICE_UUID);
    advertising->setScanResponse(true);
    advertising->start();
    initialized_ = true;
    enabled_ = true;
    Serial.print("AppLoader BLE one-time initialization complete. Free heap=");
    Serial.println(ESP.getFreeHeap());
    return true;
  }

  /** Stop connections and advertising while retaining the one-time BLE objects. */
  void deactivate() {
    if (!enabled_) return;
    enabled_ = false;
    restartAdvertising_ = false;
    if (connected_ && server_ != nullptr) {
      server_->disconnect(connId_);
      delay(100);
    }
    BLEDevice::stopAdvertising();
    delay(100);
    resetSessionState();
    Serial.print("AppLoader BLE advertising stopped; stack retained. Free heap=");
    Serial.println(ESP.getFreeHeap());
  }

  void poll() override {
    if (!enabled_) return;
    if (restartAdvertising_) {
      restartAdvertising_ = false;
      if (server_ != nullptr) server_->getAdvertising()->start();
    }
  }

  bool getLine(char *line, size_t capacity) override {
    if (!enabled_ || !lineReady_ || capacity == 0) return false;
    strlcpy(line, completed_, capacity);
    lineReady_ = false;
    completed_[0] = '\0';
    return true;
  }

  void write(const char *text) override {
    if (!enabled_ || !connected_ || tx_ == nullptr || text == nullptr) return;
    const size_t length = strlen(text);
    size_t offset = 0;
    while (offset < length) {
      const size_t chunk = min(static_cast<size_t>(180), length - offset);
      tx_->setValue(reinterpret_cast<const uint8_t *>(text + offset), chunk);
      tx_->notify();
      offset += chunk;
      delay(8);
    }
  }

  const char *name() const override { return "BLE"; }
  bool enabled() const { return enabled_; }
  bool initialized() const { return initialized_; }

  void onConnect(BLEServer *) override {
    connected_ = true;
    connectionEvent_ = true;
    peerKnown_ = false;
  }

  void onConnect(BLEServer *, esp_ble_gatts_cb_param_t *param) override {
    connected_ = true;
    connectionEvent_ = true;
    if (param != nullptr) {
      connId_ = param->connect.conn_id;
      snprintf(peerId_, sizeof(peerId_), "%02X%02X%02X%02X%02X%02X",
               param->connect.remote_bda[0], param->connect.remote_bda[1],
               param->connect.remote_bda[2], param->connect.remote_bda[3],
               param->connect.remote_bda[4], param->connect.remote_bda[5]);
      peerKnown_ = true;
    } else {
      peerKnown_ = false;
      peerId_[0] = '\0';
    }
  }

  void onDisconnect(BLEServer *) override {
    connected_ = false;
    disconnectionEvent_ = true;
    restartAdvertising_ = enabled_;
  }

  void onWrite(BLECharacteristic *characteristic) override {
    const String value = characteristic->getValue();
    for (size_t i = 0; i < value.length(); ++i) accept(value[i]);
  }

  /** Consume a pending connection event and return its peer identity when known. */
  bool takeConnectionEvent(char *peerId, size_t capacity, bool &peerKnown) {
    if (!connectionEvent_) return false;
    connectionEvent_ = false;
    peerKnown = peerKnown_;
    if (capacity > 0) strlcpy(peerId, peerKnown_ ? peerId_ : "unknown", capacity);
    return true;
  }

  /** Consume and clear the pending disconnection event flag. */
  bool takeDisconnectionEvent() {
    if (!disconnectionEvent_) return false;
    disconnectionEvent_ = false;
    return true;
  }

  void disconnect() {
    if (connected_ && server_ != nullptr) server_->disconnect(connId_);
  }

private:
  BLEServer *server_ = nullptr;
  BLECharacteristic *tx_ = nullptr;
  BLE2902 txDescriptor_;
  char deviceName_[40] = ESP_APPLOADER_NAME "--1";
  bool initialized_ = false;
  bool initializationFailed_ = false;
  volatile bool enabled_ = false;
  volatile bool connected_ = false;
  volatile bool restartAdvertising_ = false;
  volatile bool connectionEvent_ = false;
  volatile bool disconnectionEvent_ = false;
  bool peerKnown_ = false;
  uint16_t connId_ = 0;
  char peerId_[13] = {};
  char incoming_[CMD_BUFFER_SIZE] = {};
  char completed_[CMD_BUFFER_SIZE] = {};
  size_t length_ = 0;
  volatile bool lineReady_ = false;

  /** Latch an unrecoverable one-time BLE construction failure. */
  bool failInitialization(const char *component) {
    initializationFailed_ = true;
    enabled_ = false;
    Serial.print("ERROR: AppLoader BLE ");
    Serial.print(component);
    Serial.println(" creation failed; reboot required.");
    return false;
  }

  /** Clear connection-specific state without destroying reusable BLE objects. */
  void resetSessionState() {
    connected_ = false;
    restartAdvertising_ = false;
    connectionEvent_ = false;
    disconnectionEvent_ = false;
    peerKnown_ = false;
    peerId_[0] = '\0';
    incoming_[0] = '\0';
    completed_[0] = '\0';
    length_ = 0;
    lineReady_ = false;
  }

  void accept(char c) {
    if (c == '\r' || c == '\n') {
      if (length_ > 0 && !lineReady_) {
        incoming_[length_] = '\0';
        strlcpy(completed_, incoming_, sizeof(completed_));
        lineReady_ = true;
      }
      length_ = 0;
      incoming_[0] = '\0';
      return;
    }
    if (lineReady_) return;
    if (length_ + 1 < sizeof(incoming_)) incoming_[length_++] = c;
  }
};

/** Enforces BLE authorization timeout and 24-hour trusted-peer reconnects. */
class AppLoaderBleAuth {
public:
  void begin() {
    Preferences preferences;
    if (preferences.begin("wtgbleauth", true)) {
      preferences.getString("peer", trustedPeer_, sizeof(trustedPeer_));
      trustedUntil_ = preferences.getULong64("until", 0);
      preferences.end();
    }
  }

  void onConnect(const char *peerId, bool peerKnown, uint32_t nowUnix) {
    connected_ = true;
    authorized_ = false;
    disconnectRequired_ = false;
    peerKnown_ = peerKnown;
    strlcpy(currentPeer_, peerKnown && peerId != nullptr ? peerId : "unknown",
            sizeof(currentPeer_));
    connectedAtUnix_ = nowUnix;
    connectedAtMs_ = millis();
    authDeadlineMs_ = connectedAtMs_ + BLE_AUTH_TIMEOUT_MS;
    promptSent_ = false;
    lastPromptMs_ = 0;

    const bool trustTimeValid = nowUnix > 0
      ? trustedUntil_ > nowUnix
      : trustedUntilMs_ != 0 && static_cast<int32_t>(millis() - trustedUntilMs_) < 0;
    if (peerKnown_ && trustTimeValid && strcmp(currentPeer_, trustedPeer_) == 0) {
      authorized_ = true;
      setEvent("BLE_TRUSTED_RECONNECT");
    } else {
      setEvent("BLE_AUTH_REQUIRED");
    }
  }

  void onDisconnect() {
    connected_ = false;
    authorized_ = false;
    disconnectRequired_ = false;
    promptSent_ = false;
    lastPromptMs_ = 0;
    setEvent("BLE_DISCONNECT");
  }

  void poll() {
    if (connected_ && !authorized_ && !disconnectRequired_ &&
        static_cast<int32_t>(millis() - authDeadlineMs_) >= 0) {
      disconnectRequired_ = true;
      setEvent("BLE_AUTH_TIMEOUT");
    }
  }

  bool submitKey(const char *candidate, uint32_t nowUnix) {
    if (!connected_ || authorized_ || candidate == nullptr) return false;
    const char *key = candidate;
    if (strncasecmp(key, "key ", 4) == 0) key += 4;
    if (strlen(key) == 10 && strcmp(key, BLE_ADMIN_KEY) == 0) {
      authorized_ = true;
      disconnectRequired_ = false;
      promptSent_ = false;
      if (peerKnown_) {
        strlcpy(trustedPeer_, currentPeer_, sizeof(trustedPeer_));
        trustedUntilMs_ = millis() + BLE_TRUST_SECONDS * 1000UL;
        if (nowUnix > 0) {
          trustedUntil_ = static_cast<uint64_t>(nowUnix) + BLE_TRUST_SECONDS;
          saveTrust();
        }
      }
      setEvent("BLE_AUTH_SUCCESS");
      return true;
    }
    disconnectRequired_ = true;
    setEvent("BLE_AUTH_FAILED");
    return false;
  }

  bool authorized() const { return connected_ && authorized_; }
  bool disconnectRequired() const { return disconnectRequired_; }
  bool promptDue() const {
    if (!connected_ || authorized_ || disconnectRequired_) return false;
    const uint32_t referenceMs = promptSent_ ? lastPromptMs_ : connectedAtMs_;
    const uint32_t intervalMs = promptSent_ ? 5000UL : 1000UL;
    return static_cast<uint32_t>(millis() - referenceMs) >= intervalMs;
  }

  void markPromptSent() {
    promptSent_ = true;
    lastPromptMs_ = millis();
  }

  uint32_t connectedAtUnix() const { return connectedAtUnix_; }
  uint64_t trustedUntil() const { return trustedUntil_; }
  const char *currentPeer() const { return currentPeer_; }

  bool takeEvent(char *event, size_t capacity) {
    if (!eventPending_) return false;
    eventPending_ = false;
    if (capacity > 0) strlcpy(event, event_, capacity);
    return true;
  }

private:
  bool connected_ = false;
  bool authorized_ = false;
  bool disconnectRequired_ = false;
  bool peerKnown_ = false;
  bool eventPending_ = false;
  bool promptSent_ = false;
  uint32_t connectedAtUnix_ = 0;
  uint32_t connectedAtMs_ = 0;
  uint32_t authDeadlineMs_ = 0;
  uint32_t trustedUntilMs_ = 0;
  uint32_t lastPromptMs_ = 0;
  uint64_t trustedUntil_ = 0;
  char currentPeer_[13] = "unknown";
  char trustedPeer_[13] = {};
  char event_[40] = {};

  void setEvent(const char *event) {
    strlcpy(event_, event != nullptr ? event : "BLE_AUTH_EVENT", sizeof(event_));
    eventPending_ = true;
  }

  void saveTrust() {
    Preferences preferences;
    if (!preferences.begin("wtgbleauth", false)) return;
    preferences.putString("peer", trustedPeer_);
    preferences.putULong64("until", trustedUntil_);
    preferences.end();
  }
};

} // namespace esp_apploader
