/*
 * EspAppLoader
 * File: AppLoaderMgr.cpp
 * Purpose: Coordinates AppLoader startup, command handling, application installation, and boot selection.
 *
 * Copyright (c) 2026 Darryl Cornish
 */

// SPDX-License-Identifier: MPL-2.0

#include "AppLoaderMgr.h"
#include "AppLoaderMdl.h"
#include "AppLoaderFlashImageMgr.h"
#include "AppLoaderStartupPolicy.h"
#include "AppLoaderProvisionMgr.h"
#include "AppLoaderProvisionState.h"
#include "AppLoaderCommandMgr.h"
#include "WTgPlatform.h"
#include "AppLoaderSerialVw.h"
#include "AppLoaderWebVw.h"

namespace esp_apploader {

using AppLoaderProvisionState = AppLoaderProvisionStateT<MAX_APP_FILENAME>;

/** Coordinates command channels, authorization, SD repository, and installation. */
// The controller coordinates startup, command handling, and installation.
// Models handle storage and flash operations, while views format responses.
class AppLoaderMgr {
public:
  /** Initialize hardware, storage, identity, boot roles, and command interfaces. */
  bool begin() {
    state_ = AppLoaderState::Starting;
    // Establish safe board output levels before storage or peripheral setup.
    forceOutputsSafe();
    serial_.begin();
    serial_.write("\r\nEspAppLoader starting...\r\n");

    pinMode(BLE_ENABLE_BUTTON_PIN, INPUT_PULLUP);
    const bool recoveryPinLow = digitalRead(BLE_ENABLE_BUTTON_PIN) == LOW;

    if (!repo_.begin()) {
      serial_.write("SD unavailable. Checking startup fallback.\r\n");
    }
    // Check the pin again after SD setup so a recovery request made during
    // initialization can still suppress automatic handoff.
    const bool recoveryRequested = recoveryPinLow ||
        digitalRead(BLE_ENABLE_BUTTON_PIN) == LOW ||
        (repo_.ready() && SD.exists(ESP_APPLOADER_RECOVERY_MARKER));
    provisioningStateReady_ = provisioningState_.load(provisioningRecord_);
    const bool provisioningHold = !provisioningStateReady_ ||
        provisioningRecord_.mode != AppLoaderProvisionState::None;
    if (provisioningHold) {
      serial_.write("PROVISION state=hold: automatic fallback suppressed.\r\n");
    }
    const bool sdHasApps = !recoveryRequested && repo_.scan() > 0;
    // Attempt a flash-only handoff before initializing peripherals needed
    // by the loader. If the handoff fails, continue into the command interface.
    if (AppLoaderStartupPolicy::fallback(
            repo_.ready(), sdHasApps, recoveryRequested || provisioningHold)) {
      serial_.write("Startup fallback: validating existing operational image.\r\n");
      bootExistingApp(serial_);
      serial_.write("Fallback could not boot; staying in AppLoader.\r\n");
    } else if (recoveryRequested) {
      serial_.write("Recovery requested; automatic application boot suppressed.\r\n");
    }
    Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
    identity_.begin();
    log_.begin();
    initializeRtc();
    if (!rtcReady_) {
      serial_.write("WARNING: RTC unavailable; BLE 24-hour trust cannot survive an AppLoader restart.\r\n");
    }
    bleAuth_.begin();
    boardBringup_.begin(repo_.ready(), rtcReady_);
    boardBringup_.printTo(serial_);
    bootCfg_.load();
    bootMgr_.inspect();
    if (bootMgr_.canMutateFlash()) {
      const esp_err_t loaderBootErr = bootMgr_.selectAppLoader();
      if (loaderBootErr != ESP_OK) {
        char warning[160];
        snprintf(warning, sizeof(warning),
                 "WARNING: Could not persist %s as the AppLoader boot partition: %s\r\n",
                 APPLOADER_PARTITION_LABEL, esp_err_to_name(loaderBootErr));
        serial_.write(warning);
      }
      bootMgr_.inspect();
    }
    ble_.setDeviceName(identity_.unitNum());
    ble_.begin();
    cmdMgr_.configure(serial_, ble_, this, dispatchCommand);
    initializeBleButtonInterface();
    log_.event("AppLoader started");
    state_ = AppLoaderState::Idle;

    char line[160];
    snprintf(line, sizeof(line), "%s %s build %s\r\nDevice: %s  Unit_Num: %d\r\n",
             ESP_APPLOADER_NAME, ESP_APPLOADER_VERSION, ESP_APPLOADER_BUILD_ID,
             identity_.deviceUid(), identity_.unitNum());
    serial_.write(line);
    snprintf(line, sizeof(line), "Reset reason: %d\r\n",
             static_cast<int>(esp_reset_reason()));
    serial_.write(line);
    AppLoaderSerialVw::printMode(serial_, bootMgr_.canMutateFlash());
    if (!bootCfg_.valid()) {
      serial_.write("WARNING: /boot.cfg is missing or invalid; no SD default application is available.\r\n");
    }
    serial_.write(
      "BLE disabled at startup. Hold GPIO4 for more than 2 seconds but "
      "less than 10 seconds to toggle BLE.\r\n");
    if (!provisioningStateReady_) {
      serial_.write("ERR appLdr2App1 code=state_read: provisioning record unavailable or invalid.\r\n");
    } else if (provisioningRecord_.mode == AppLoaderProvisionState::Install) {
      resumeProvisioning(recoveryRequested);
    }
    return repo_.ready() && bootMgr_.targetAvailable();
  }

  /** Perform one nonblocking service pass from Arduino loop(). */
  void run() {
    serviceBleButtonInterface();
    ble_.poll();
    handleBleConnectionEvents();
    bleAuth_.poll();
    handleBleAuthEvents();
    if (bleAuth_.disconnectRequired()) ble_.disconnect();
    cmdMgr_.poll();
    if (uploadModeActive_) webServer_.handleClient();
    delay(2);
  }

private:
  AppLoaderSerialCmdChannel serial_;       // Always-available USB command transport.
  AppLoaderBleCmdChannel ble_;             // Gesture-controlled BLE command transport.
  AppLoaderBleAuth bleAuth_;               // Authorization state for the BLE session.
  AppLoaderCmdMgr cmdMgr_;                 // Common dispatcher for both transports.
  AppLoaderIdentityMdl identity_;          // Hardware UID and configured unit number.
  AppLoaderBootCfgMdl bootCfg_;               // SD/NVS startup-application selection.
  AppLoaderFlashImageMgr flashImageMgr_; // Read-only on-flash inspection.
  AppLoaderProvisionMgr provisioningMgr_;
  AppLoaderProvisionState provisioningState_;
  AppLoaderProvisionState::Record provisioningRecord_;
  bool provisioningStateReady_ = false;
  AppLoaderBootMgr bootMgr_;               // Validated app1/app0 partition mapping.
  AppLoaderLogMdl log_;              // Event and installation audit writer.
  AppLoaderAppRepoMdl repo_;                  // SD application-image repository.
  AppLoaderAppValidator validator_;        // Source and flashed-image validator.
  AppLoaderAppInstaller installer_;        // OTA writer for the app0 partition.
  WebServer webServer_{80};          // Local addapp upload server.
  RTC_DS3231 rtc_;                   // DS3231 wall clock and BLE-trust time source.
  WTgBoardBringupMgr boardBringup_;     // WaterThing 0.5 peripheral bring-up tests.
  File uploadFile_;                  // Currently open incoming .part file.
  AppLoaderState state_ = AppLoaderState::Starting; // Current workflow phase.
  bool confirmationPending_ = false; // True after install until YES/cancel.
  bool uploadModeActive_ = false;    // True while local upload AP/server runs.
  bool uploadRoutesConfigured_ = false; // Web route lambdas installed once.
  bool uploadInProgress_ = false;    // Multipart body is currently being received.
  bool uploadSucceeded_ = false;     // Result returned by the latest upload.
  bool uploadFailed_ = false;        // Prevents further writes after upload error.
  WTgInteruptBtn bleButton_;         // ISR-captured GPIO4 gesture source.
  WTgTimerInterruptMgr bleUiTimer_;  // Base timer for deterministic LED feedback.
  WTgUiFeedbackMgr bleUiFeedback_;   // Immediate and result-pattern LED control.
  uint8_t bleHoldToggleRequests_ = 0; // Deferred 2-10 second hold toggles.
  bool rtcReady_ = false;            // Whether persistent absolute trust is usable.
  bool rtcLostPowerAtStartup_ = false; // DS3231 oscillator-stop state seen at boot.
  bool rtcInitializedFromBuild_ = false; // True when lost RTC was set to build time.
  uint32_t uploadBytes_ = 0;         // Bytes accepted in current Web upload.
  char uploadFilename_[MAX_APP_FILENAME] = {}; // Sanitized final image name.
  char uploadPartPath_[MAX_APP_FILENAME + 24] = {}; // Staging-file path.
  char uploadStatus_[220] = "No application upload attempted."; // UI result text.
  char uploadApSsid_[40] = {};       // Unit-specific temporary access-point SSID.
  char pendingFilename_[MAX_APP_FILENAME] = {}; // Image awaiting confirmation.
  char transactionPrevious_[MAX_APP_FILENAME] = {}; // Prior installed image.

  /** Drive all pump-control outputs low before enabling them as outputs. */
  static void forceOutputsSafe() {
    digitalWrite(CTL0_PIN, LOW);
    digitalWrite(CTL1_PIN, LOW);
    digitalWrite(CTL2_PIN, LOW);
    digitalWrite(CTL3_PIN, LOW);
    pinMode(CTL0_PIN, OUTPUT);
    pinMode(CTL1_PIN, OUTPUT);
    pinMode(CTL2_PIN, OUTPUT);
    pinMode(CTL3_PIN, OUTPUT);
    digitalWrite(CTL0_PIN, LOW);
    digitalWrite(CTL1_PIN, LOW);
    digitalWrite(CTL2_PIN, LOW);
    digitalWrite(CTL3_PIN, LOW);
  }

  static void onBleButtonPressThunk(
      const WTgInteruptBtn::PressEvent &event,
      void *context) {
    if (context != nullptr) {
      static_cast<AppLoaderMgr *>(context)->onBleButtonPress(event);
    }
  }

  static void ARDUINO_ISR_ATTR onBleButtonLevelThunk(
      bool active,
      void *context) {
    if (context != nullptr) {
      static_cast<WTgUiFeedbackMgr *>(context)
          ->setPhysicalPressLevelIsr(active);
    }
  }

  static void ARDUINO_ISR_ATTR onBleUiTimerTickThunk(void *context) {
    if (context != nullptr) {
      static_cast<WTgUiFeedbackMgr *>(context)->onTimerTickIsr();
    }
  }

  /** Configure GPIO gesture capture, feedback output, and feedback timer. */
  void initializeBleButtonInterface() {
    const bool feedbackReady = bleUiFeedback_.begin(
        BLE_FEEDBACK_LED_PIN,
        WTgUiFeedbackMgr::ActiveLevel::High,
        BLE_UI_TIMER_TICK_US,
        BLE_UI_FLASH_ON_US,
        BLE_UI_FLASH_OFF_US,
        BLE_UI_POST_RELEASE_QUIET_US);
    if (!feedbackReady) {
      serial_.write("ERROR: BLE button feedback initialization failed.\r\n");
    }

    const bool timerReady =
        feedbackReady &&
        bleUiTimer_.begin(BLE_UI_TIMER_TICK_US,
                          onBleUiTimerTickThunk,
                          &bleUiFeedback_);
    if (!timerReady) {
      serial_.write("ERROR: BLE button UI timer initialization failed.\r\n");
    }

    const bool buttonReady = bleButton_.begin(
        BLE_ENABLE_BUTTON_PIN,
        WTgInteruptBtn::ActiveLevel::Low,
        BLE_BUTTON_PRESS_PERIOD_MS,
        BLE_BUTTON_MAX_VALIDATION_TRANSITIONS,
        onBleButtonPressThunk,
        this,
        INPUT_PULLUP,
        BLE_BUTTON_RELEASE_IGNORE_US,
        feedbackReady ? onBleButtonLevelThunk : nullptr,
        feedbackReady ? static_cast<void *>(&bleUiFeedback_) : nullptr);
    if (!buttonReady) {
      serial_.write("ERROR: BLE interrupt button initialization failed.\r\n");
    }
  }

  /** Accept only a reed activation longer than 2 s and shorter than 10 s. */
  void onBleButtonPress(const WTgInteruptBtn::PressEvent &event) {
    if (event.durationUs > BLE_REED_TOGGLE_MIN_US &&
        event.durationUs < BLE_REED_TOGGLE_MAX_US) {
      if (bleHoldToggleRequests_ < UINT8_MAX) {
        ++bleHoldToggleRequests_;
      }
      serial_.write("BLE reed hold accepted: toggle requested.\r\n");
      return;
    }

    if (event.durationUs >= BLE_REED_TOGGLE_MAX_US) {
      serial_.write("BLE reed hold rejected: activation reached 10 seconds.\r\n");
      bleUiFeedback_.signal(WTgUiFeedbackMgr::Result::Failure);
      return;
    }
    serial_.write("BLE reed activation ignored: hold must exceed 2 seconds.\r\n");
  }

  /** Activate BLE service mode and record the transition. */
  bool enterBleMode() {
    if (ble_.enabled()) return true;
    if (!ble_.activate()) return false;
    if (!bleUiFeedback_.setModeIndicator(
            true, BLE_MODE_LED_ON_US, BLE_MODE_LED_OFF_US)) {
      serial_.write("WARNING: BLE mode LED indication could not be enabled.\r\n");
    }
    log_.event("BLE service mode entered by button");
    serial_.write("AppLoader BLE service mode active.\r\n");
    return true;
  }

  /** End BLE service mode and clear authorization session state. */
  void exitBleMode() {
    if (!ble_.enabled()) return;
    ble_.deactivate();
    bleUiFeedback_.setModeIndicator(false);
    bleAuth_.onDisconnect();
    log_.event("BLE service mode exited by button");
    serial_.write("AppLoader BLE service mode inactive.\r\n");
  }

  /** Consume ISR-captured gestures and perform their deferred mode changes. */
  void serviceBleButtonInterface() {
    bleButton_.service();

    if (bleHoldToggleRequests_ != 0U) {
      --bleHoldToggleRequests_;

      if (uploadInProgress_) {
        serial_.write(
            "BLE reed hold rejected: application upload is in progress.\r\n");
        bleUiFeedback_.signal(WTgUiFeedbackMgr::Result::Failure);
        return;
      }

      if (uploadModeActive_) {
        serial_.write(
            "BLE reed hold: closing application-upload Wi-Fi mode.\r\n");
        stopAddAppMode(serial_);
      }

      const bool wasEnabled = ble_.enabled();
      if (wasEnabled) {
        exitBleMode();
      } else {
        enterBleMode();
      }

      if (ble_.enabled() != wasEnabled) {
        bleUiFeedback_.signal(WTgUiFeedbackMgr::Result::LongSuccess);
      } else {
        bleUiFeedback_.signal(WTgUiFeedbackMgr::Result::Failure);
      }
      return;
    }
  }

  static void dispatchCommand(void *context, char *command, AppLoaderCmdChannel &channel) {
    if (context != nullptr) {
      static_cast<AppLoaderMgr *>(context)->processCommand(command, channel);
    }
  }

  static char *trim(char *text) {
    while (*text == ' ' || *text == '\t') ++text;
    size_t length = strlen(text);
    while (length > 0 && (text[length - 1] == ' ' || text[length - 1] == '\t')) text[--length] = '\0';
    return text;
  }

  void processCommand(char *raw, AppLoaderCmdChannel &channel) {
    char *command = trim(raw);
    if (*command == '\0') return;

    if (&channel == &ble_ && !bleAuth_.authorized()) {
      const bool accepted = bleAuth_.submitKey(command, currentUnixTime());
      channel.write(accepted
        ? "AUTH SUCCESS: This device is trusted for 24 hours.\r\n"
        : "AUTH FAILED: BLE connection will be closed.\r\n");
      return;
    }
    echoCommand(command, channel);

    if (confirmationPending_) {
      if (strcmp(command, "YES") == 0) {
        confirmationPending_ = false;
        performInstall(pendingFilename_, channel);
      } else {
        confirmationPending_ = false;
        pendingFilename_[0] = '\0';
        state_ = AppLoaderState::Idle;
        channel.write("Installation cancelled.\r\n");
      }
      return;
    }

    char *argument = strpbrk(command, " \t");
    if (argument != nullptr) {
      *argument++ = '\0';
      argument = trim(argument);
    }

    if (strcasecmp(command, "appLdr2App1") == 0) return provisionCommand(argument, channel);
    if (strcasecmp(command, "help?") == 0) return printHelp(channel);
    if (strcasecmp(command, "status?") == 0) return printStatus(channel);
    if (strcasecmp(command, "recovery?") == 0) {
      AppLoaderSerialVw::printRecovery(channel, digitalRead(BLE_ENABLE_BUTTON_PIN) == LOW,
          repo_.ready() && SD.exists(ESP_APPLOADER_RECOVERY_MARKER));
      return;
    }
    if (strcasecmp(command, "identity?") == 0) return printIdentity(channel);
    if (strcasecmp(command, "partitions?") == 0) return printPartitions(channel);
    if (strcasecmp(command, "apps?") == 0) return printApps(channel);
    if (strcasecmp(command, "boot?") == 0) return printBootConfig(channel);
    if (strcasecmp(command, "startup?") == 0) return printBootConfig(channel);
    if (strcasecmp(command, "wifi?") == 0) return printWifiStatus(channel);
    if (strcasecmp(command, "time") == 0 ||
        strcasecmp(command, "time?") == 0) return printRtc(channel);
    if (strcasecmp(command, "bringup?") == 0) return bringupCommand(channel);
    if (strcasecmp(command, "setrtc") == 0) return setRtcCommand(argument, channel);
    if (strcasecmp(command, "unixtime") == 0) return setRtcUnixCommand(argument, channel);
    if (strcasecmp(command, "addapp") == 0) return addAppCommand(argument, channel);
    if (strcasecmp(command, "exitaddapp") == 0) return stopAddAppMode(channel);
    if (strcasecmp(command, "startup") == 0) return startupCommand(argument, channel);
    if (strcasecmp(command, "verify") == 0) return verifyCommand(argument, channel);
    if (strcasecmp(command, "install") == 0) return installCommand(argument, channel);
    if (strcasecmp(command, "verifyflash") == 0) {
      inspectExistingApp(channel);
      return;
    }
    if (strcasecmp(command, "bootapp") == 0) {
      if (argument && strcasecmp(argument, "existing") == 0)
        return bootExistingApp(channel);
      if (argument && *argument) {
        channel.write("ERR usage: bootapp [existing]\r\n");
        return;
      }
      return bootApp(channel);
    }
    if (strcasecmp(command, "logs?") == 0) return printLogs(channel);
    if (strcasecmp(command, "reboot") == 0) {
      channel.write("Rebooting AppLoader...\r\n");
      delay(250);
      ESP.restart();
    }
    if (strcasecmp(command, "help") == 0 || strcasecmp(command, "status") == 0 ||
        strcasecmp(command, "identity") == 0 || strcasecmp(command, "partitions") == 0 ||
        strcasecmp(command, "apps") == 0 || strcasecmp(command, "boot") == 0 ||
        strcasecmp(command, "wifi") == 0 || strcasecmp(command, "logs") == 0) {
      channel.write("ERR read-only queries must end in ?. Type help? for commands.\r\n");
      return;
    }
    channel.write("ERR unknown command. Type help? for commands.\r\n");
  }

  static bool sensitiveCommand(const char *command) {
    return strncasecmp(command, "key", 3) == 0 ||
           strncasecmp(command, "password", 8) == 0 ||
           strncasecmp(command, "credential", 10) == 0 ||
           strncasecmp(command, "token", 5) == 0 ||
           strncasecmp(command, "auth", 4) == 0;
  }

  void echoCommand(const char *command, AppLoaderCmdChannel &channel) {
    const char *display = sensitiveCommand(command) ? "<redacted>" : command;
    char line[CMD_BUFFER_SIZE + 40];
    snprintf(line, sizeof(line), "CMD[%s]: %s\r\n", channel.name(), display);
    // Every command is echoed to USB serial even when it arrived through BLE.
    serial_.write(line);

    char event[CMD_BUFFER_SIZE + 40];
    snprintf(event, sizeof(event), "CMD[%s]: %s", channel.name(), display);
    log_.event(event);
  }

  uint32_t currentUnixTime() {
    if (!rtcReady_) return 0;
    const DateTime now = rtc_.now();
    if (now.year() < 2024 || now.year() > 2099) return 0;
    return now.unixtime();
  }

  /** Probe the DS3231 and establish a valid time after battery loss. */
  void initializeRtc() {
    rtcReady_ = rtc_.begin();
    if (!rtcReady_) return;

    rtcLostPowerAtStartup_ = rtc_.lostPower();
    if (rtcLostPowerAtStartup_) {
      // A stopped oscillator leaves no trustworthy time to preserve. The build
      // timestamp is a deterministic starting point until setrtc/unixtime or
      // the operational application performs a network-time synchronization.
      rtc_.adjust(DateTime(F(__DATE__), F(__TIME__)));
      rtcInitializedFromBuild_ = true;
      serial_.write(
          "RTC: DS3231 detected; lost-power flag was set. "
          "Initialized to firmware build time.\r\n");
    } else {
      serial_.write("RTC: DS3231 detected and running.\r\n");
    }
  }

  /** Report DS3231 availability, time, Unix time, temperature, and boot state. */
  void printRtc(AppLoaderCmdChannel &channel) {
    if (!rtcReady_) {
      channel.write("RTC: unavailable (DS3231 not detected at I2C address 0x68).\r\n");
      return;
    }

    const DateTime now = rtc_.now();
    char line[192];
    snprintf(line, sizeof(line),
             "RTC: DS3231 ready\r\n"
             "Time: %04u-%02u-%02u %02u:%02u:%02u\r\n"
             "Unix_Time: %lu\r\n"
             "Temperature_C: %.2f\r\n"
             "Lost_power_at_startup: %s\r\n"
             "Initialized_from_build_time: %s\r\n",
             static_cast<unsigned>(now.year()),
             static_cast<unsigned>(now.month()),
             static_cast<unsigned>(now.day()),
             static_cast<unsigned>(now.hour()),
             static_cast<unsigned>(now.minute()),
             static_cast<unsigned>(now.second()),
             static_cast<unsigned long>(now.unixtime()),
             static_cast<double>(rtc_.getTemperature()),
             rtcLostPowerAtStartup_ ? "yes" : "no",
             rtcInitializedFromBuild_ ? "yes" : "no");
    channel.write(line);
  }

  /** Set the DS3231 from an explicit YYYY-MM-DD HH:MM:SS value. */
  void setRtcCommand(const char *argument, AppLoaderCmdChannel &channel) {
    if (!rtcReady_) {
      channel.write("ERR RTC unavailable; DS3231 not detected.\r\n");
      return;
    }
    if (argument == nullptr || *argument == '\0') {
      channel.write("ERR usage: setrtc YYYY-MM-DD HH:MM:SS\r\n");
      return;
    }

    unsigned year, month, day, hour, minute, second;
    char trailing;
    const int fields = sscanf(argument, "%u-%u-%u %u:%u:%u %c",
                              &year, &month, &day,
                              &hour, &minute, &second, &trailing);
    if (fields != 6 || year < 2000 || year > 2099 ||
        month < 1 || month > 12 || day < 1 || day > 31 ||
        hour > 23 || minute > 59 || second > 59) {
      channel.write("ERR invalid RTC time; use YYYY-MM-DD HH:MM:SS (2000-2099).\r\n");
      return;
    }

    const DateTime requested(year, month, day, hour, minute, second);
    if (!requested.isValid() || requested.year() != year ||
        requested.month() != month || requested.day() != day) {
      channel.write("ERR invalid calendar date.\r\n");
      return;
    }

    rtc_.adjust(requested);
    rtcInitializedFromBuild_ = false;
    log_.event("RTC set with setrtc command");
    channel.write("RTC updated.\r\n");
    printRtc(channel);
  }

  /** Set the DS3231 from a Unix timestamp in seconds. */
  void setRtcUnixCommand(const char *argument, AppLoaderCmdChannel &channel) {
    if (!rtcReady_) {
      channel.write("ERR RTC unavailable; DS3231 not detected.\r\n");
      return;
    }
    if (argument == nullptr || *argument == '\0') {
      channel.write("ERR usage: unixtime UNIX_TIME\r\n");
      return;
    }

    char *end = nullptr;
    const unsigned long unixTime = strtoul(argument, &end, 10);
    while (end != nullptr && (*end == ' ' || *end == '\t')) ++end;
    const DateTime requested(static_cast<uint32_t>(unixTime));
    if (end == argument || end == nullptr || *end != '\0' ||
        requested.year() < 2024 || requested.year() > 2099) {
      channel.write("ERR invalid Unix time; expected seconds for a date from 2024 through 2099.\r\n");
      return;
    }

    rtc_.adjust(requested);
    rtcInitializedFromBuild_ = false;
    log_.event("RTC set with unixtime command");
    channel.write("RTC updated.\r\n");
    printRtc(channel);
  }

  static void formatUnixTime(uint32_t unixTime, char *out, size_t capacity) {
    if (capacity == 0) return;
    if (unixTime == 0) {
      strlcpy(out, "unavailable", capacity);
      return;
    }
    const DateTime value(unixTime);
    snprintf(out, capacity, "%04u-%02u-%02u %02u:%02u:%02u",
             static_cast<unsigned>(value.year()),
             static_cast<unsigned>(value.month()),
             static_cast<unsigned>(value.day()),
             static_cast<unsigned>(value.hour()),
             static_cast<unsigned>(value.minute()),
             static_cast<unsigned>(value.second()));
  }

  void handleBleConnectionEvents() {
    char peer[13];
    bool peerKnown = false;
    if (ble_.takeConnectionEvent(peer, sizeof(peer), peerKnown)) {
      const uint32_t now = currentUnixTime();
      bleAuth_.onConnect(peer, peerKnown, now);
      char timestamp[24], event[120];
      formatUnixTime(now, timestamp, sizeof(timestamp));
      snprintf(event, sizeof(event), "BLE_CONNECT peer=%s connected_at=%s",
               peerKnown ? peer : "unknown", timestamp);
      log_.event(event);
      if (bleAuth_.authorized()) {
        ble_.write("AUTH SUCCESS: Trusted device recognized.\r\n");
      }
    }
    if (ble_.takeDisconnectionEvent()) bleAuth_.onDisconnect();
    if (bleAuth_.promptDue()) {
      ble_.write("Enter the BLE Authorization Code\r\n");
      bleAuth_.markPromptSent();
    }
  }

  void handleBleAuthEvents() {
    char authEvent[40];
    if (!bleAuth_.takeEvent(authEvent, sizeof(authEvent))) return;
    char event[120];
    snprintf(event, sizeof(event), "%s peer=%s",
             authEvent, bleAuth_.currentPeer());
    log_.event(event);
    if (strcmp(authEvent, "BLE_AUTH_TIMEOUT") == 0) {
      ble_.write("AUTH TIMEOUT: BLE connection will be closed.\r\n");
    }
  }

  void printHelp(AppLoaderCmdChannel &channel) {
    channel.write(
      "Commands:\r\n"
      "  appLdr2App1 <0|1>      Copy loader to app1; 0=install SD default and boot app0, 1=stay app1\r\n"
      "  recovery?              Report GPIO4 and SD marker; no recovery action\r\n"
      "  status?                AppLoader and SD status\r\n"
      "  identity?              Module and software identity\r\n"
      "  partitions?            Running, boot and target partitions\r\n"
      "  apps?                  List /apps/*.bin images\r\n"
      "  boot?                  Show /boot.cfg and persistent boot selection\r\n"
      "  startup?               Show the selected startup application\r\n"
      "  wifi?                  Show local application-upload Wi-Fi status\r\n"
      "  time                   Show DS3231 time, Unix time and temperature\r\n"
      "  bringup?               Initialize and test SD, RTC, LoRa, barometer and ADS1115\r\n"
      "  setrtc YYYY-MM-DD HH:MM:SS  Set DS3231 calendar time\r\n"
      "  unixtime UNIX_TIME     Set DS3231 from Unix seconds\r\n"
      "  addapp                 Start local Wi-Fi application upload page\r\n"
      "  exitaddapp             Stop upload page and Wi-Fi access point\r\n"
      "  startup <file|number>  Transactionally select Default_App\r\n"
      "  verify <file|number>   Validate and identify an SD image\r\n"
      "  verify default         Validate Default_App from /boot.cfg\r\n"
      "  install <file|number>  Confirm and install image to operational target\r\n"
      "  install default        Install Default_App from /boot.cfg\r\n"
      "  verifyflash            Validate existing target image without SD\r\n"
      "  bootapp existing       Validate and boot target without SD records\r\n"
      "  bootapp                Boot the SD-managed operational image\r\n"
      "  logs?                  Show AppLoader event log\r\n"
      "  reboot                 Restart the AppLoader\r\n"
      "  help?                  Show this help\r\n");
  }

  void printStatus(AppLoaderCmdChannel &channel) {
    char line[180];
    snprintf(line, sizeof(line),
             "State: %s\r\nSD: %s\r\nHeap: %lu bytes\r\n"
             "BLE: %s\r\nBLE object tree: %s\r\n",
             stateName(state_), repo_.ready() ? "ready" : "NOT READY",
             static_cast<unsigned long>(ESP.getFreeHeap()),
             ble_.enabled() ? "active" : "inactive",
             ble_.initialized() ? "initialized/retained" : "not initialized");
    channel.write(line);
    AppLoaderSerialVw::printMode(channel, bootMgr_.canMutateFlash());
    channel.write(bootCfg_.valid() ? "Boot configuration: valid\r\n" : "Boot configuration: INVALID OR MISSING\r\n");
    channel.write(rtcReady_ ? "RTC: DS3231 ready\r\n" : "RTC: unavailable\r\n");
    channel.write(boardBringup_.allPassed() ? "Board bring-up: PASS\r\n" : "Board bring-up: FAIL\r\n");
    printWifiStatus(channel);
  }

  /** Re-run all non-destructive board peripheral initialization checks. */
  void bringupCommand(AppLoaderCmdChannel &channel) {
    boardBringup_.begin(repo_.ready(), rtcReady_);
    boardBringup_.printTo(channel);
    log_.event(boardBringup_.allPassed() ? "Board bring-up PASS" : "Board bring-up FAIL");
  }

  void printPartitions(AppLoaderCmdChannel &channel) {
    bootMgr_.inspect();
    bootMgr_.printTo(channel);
  }

  void printBootConfig(AppLoaderCmdChannel &channel) {
    // Reload so an SD-card edit is visible without restarting the AppLoader.
    bootCfg_.load();
    bootCfg_.printTo(channel);
    const esp_partition_t *selected = esp_ota_get_boot_partition();
    char line[120];
    snprintf(line, sizeof(line), "Persistent_Boot_Partition: %s\r\n",
             selected != nullptr ? selected->label : "unavailable");
    channel.write(line);
  }

  void printIdentity(AppLoaderCmdChannel &channel) {
    char line[220];
    snprintf(line, sizeof(line),
             "Device_UID: %s\r\nUnit_Num: %d\r\nAppLoader_Name: %s\r\nAppLoader_Version: %s\r\nAppLoader_Build_ID: %s\r\n",
             identity_.deviceUid(), identity_.unitNum(), ESP_APPLOADER_NAME,
             ESP_APPLOADER_VERSION, ESP_APPLOADER_BUILD_ID);
    channel.write(line);
  }

  void printApps(AppLoaderCmdChannel &channel) {
    state_ = AppLoaderState::ScanningApps;
    const size_t count = repo_.scan();
    if (count == 0) {
      channel.write("No .bin images found in /apps/.\r\n");
    } else {
      char line[150];
      snprintf(line, sizeof(line), "Applications in /apps/ (%u):\r\n", static_cast<unsigned>(count));
      channel.write(line);
      for (size_t i = 0; i < count; ++i) {
        snprintf(line, sizeof(line), "  %u. %s\r\n", static_cast<unsigned>(i + 1), repo_.filename(i));
        channel.write(line);
      }
    }
    state_ = AppLoaderState::Idle;
  }

  bool resolveArgument(const char *argument, char *filename, AppLoaderCmdChannel &channel) {
    if (argument == nullptr || *argument == '\0') {
      channel.write("ERR filename or app list number required.\r\n");
      return false;
    }
    if (strcasecmp(argument, "default") == 0) {
      if (!bootCfg_.load()) {
        channel.write("ERR /boot.cfg is missing or invalid.\r\n");
        return false;
      }
      argument = bootCfg_.defaultApp();
    }
    if (!repo_.resolve(argument, filename, MAX_APP_FILENAME)) {
      channel.write("ERR selected .bin was not found in /apps/. Use apps? to list images.\r\n");
      return false;
    }
    return true;
  }

  bool validate(const char *filename, AppLoaderAppImage &image, AppLoaderCmdChannel &channel) {
    state_ = AppLoaderState::ValidatingSource;
    channel.write("Validating source image and calculating SHA-256...\r\n");
    char error[160];
    if (!validator_.validateSource(repo_, filename, bootMgr_, image, error, sizeof(error))) {
      char line[200];
      snprintf(line, sizeof(line), "ERR validation failed: %s\r\n", error);
      channel.write(line);
      state_ = AppLoaderState::InstallationFailed;
      return false;
    }
    char hash[65], line[240];
    AppLoaderLogMdl::hashHex(image.sha256, image.hashAvailable, hash, sizeof(hash));
    snprintf(line, sizeof(line), "SOURCE CHECK PASSED: %s (header/size/hash; not a bootability test)\r\n", image.filename);
    channel.write(line);
    snprintf(line, sizeof(line), "Size: %lu bytes\r\n",
             static_cast<unsigned long>(image.fileSize));
    channel.write(line);
    snprintf(line, sizeof(line), "ESP framework project: %s\r\n",
             image.metadataAvailable ? image.appName : "unavailable");
    channel.write(line);
    snprintf(line, sizeof(line), "ESP framework version: %s\r\n",
             image.metadataAvailable ? image.appVersion : "unavailable");
    channel.write(line);
    snprintf(line, sizeof(line), "ESP framework build: %s\r\n",
             image.metadataAvailable ? image.buildId : "unavailable");
    channel.write(line);
    snprintf(line, sizeof(line), "Source SHA-256: %s\r\n", hash);
    channel.write(line);
    state_ = AppLoaderState::Idle;
    return true;
  }

  void verifyCommand(const char *argument, AppLoaderCmdChannel &channel) {
    char filename[MAX_APP_FILENAME];
    if (!resolveArgument(argument, filename, channel)) return;
    AppLoaderAppImage image;
    validate(filename, image, channel);
  }

  void addAppCommand(const char *argument, AppLoaderCmdChannel &channel) {
    // Repository directories are created only by an explicit upload request.
    if (repo_.ready() &&
        ((!SD.exists(APPS_DIR) && !SD.mkdir(APPS_DIR)) ||
         (!SD.exists(INCOMING_DIR) && !SD.mkdir(INCOMING_DIR)))) {
      channel.write("ERR could not create application upload directories.\r\n");
      return;
    }
    if (argument != nullptr && *argument != '\0') {
      channel.write("ERR addapp takes no argument. Type addapp and use the reported Web page.\r\n");
      return;
    }
    if (!repo_.ready() || !bootMgr_.targetAvailable()) {
      channel.write("ERR addapp unavailable: SD or operational target is unavailable.\r\n");
      return;
    }

    if (uploadModeActive_) {
      reportAddAppConnection(channel);
      return;
    }

    snprintf(uploadApSsid_, sizeof(uploadApSsid_), "%s-%d", ESP_APPLOADER_NAME, identity_.unitNum());
    IPAddress ip(192, 168, 4, 1);
    IPAddress gateway(192, 168, 4, 1);
    IPAddress subnet(255, 255, 255, 0);
    WiFi.mode(WIFI_AP);
    if (!WiFi.softAPConfig(ip, gateway, subnet) || !WiFi.softAP(uploadApSsid_)) {
      WiFi.softAPdisconnect(true);
      WiFi.mode(WIFI_OFF);
      channel.write("ERR could not start the application-upload Wi-Fi access point.\r\n");
      return;
    }
    configureUploadRoutes();
    webServer_.begin();
    uploadModeActive_ = true;
    uploadSucceeded_ = false;
    uploadFailed_ = false;
    strlcpy(uploadStatus_, "Waiting for an application image.", sizeof(uploadStatus_));
    state_ = AppLoaderState::UploadMode;
    log_.event("Local Wi-Fi addapp mode started");
    reportAddAppConnection(channel);
  }

  void reportAddAppConnection(AppLoaderCmdChannel &channel) {
    char line[220];
    snprintf(line, sizeof(line),
             "Application upload mode active.\r\nWi-Fi SSID: %s\r\nPassword: <none / open network>\r\nUpload URL: http://%s\r\n",
             uploadApSsid_[0] != '\0' ? uploadApSsid_ : "unavailable",
             APP_UPLOAD_IP_TEXT);
    channel.write(line);
  }

  void printWifiStatus(AppLoaderCmdChannel &channel) {
    char line[220];
    snprintf(line, sizeof(line),
             "Application upload Wi-Fi: %s\r\nSSID: %s\r\nIP: %s\r\nConnected clients: %u\r\nLast upload: %s\r\n",
             uploadModeActive_ ? "active" : "inactive",
             uploadModeActive_ ? uploadApSsid_ : "unavailable",
             uploadModeActive_ ? APP_UPLOAD_IP_TEXT : "unavailable",
             uploadModeActive_ ? static_cast<unsigned>(WiFi.softAPgetStationNum()) : 0U,
             uploadStatus_);
    channel.write(line);
  }

  void stopAddAppMode(AppLoaderCmdChannel &channel) {
    if (!uploadModeActive_) {
      channel.write("Application upload mode is not active.\r\n");
      return;
    }
    if (uploadInProgress_) {
      channel.write("ERR cannot stop addapp while an upload is in progress.\r\n");
      return;
    }
    webServer_.stop();
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_OFF);
    uploadModeActive_ = false;
    state_ = AppLoaderState::Idle;
    log_.event("Local Wi-Fi addapp mode stopped");
    channel.write("Application upload mode stopped. BLE and USB remain available.\r\n");
  }

  void configureUploadRoutes() {
    if (uploadRoutesConfigured_) return;
    webServer_.on("/", HTTP_GET, [this]() { handleUploadPage(); });
    webServer_.on("/upload", HTTP_POST,
                  [this]() { handleUploadDone(); },
                  [this]() { handleUploadData(); });
    webServer_.on("/exit", HTTP_GET, [this]() {
      webServer_.send(200, "text/plain", "Application upload mode is stopping.");
      delay(150);
      if (!uploadInProgress_) {
        webServer_.stop();
        WiFi.softAPdisconnect(true);
        WiFi.mode(WIFI_OFF);
        uploadModeActive_ = false;
        state_ = AppLoaderState::Idle;
        log_.event("Local Wi-Fi addapp mode stopped from Web page");
      }
    });
    webServer_.onNotFound([this]() { webServer_.send(404, "text/plain", "Not found"); });
    uploadRoutesConfigured_ = true;
  }

  void handleUploadPage() {
    webServer_.send(200, "text/html", AppLoaderWebVw::uploadPage(
        identity_.unitNum(), ESP_APPLOADER_VERSION, uploadStatus_, uploadInProgress_));
  }

  void handleUploadDone() {
    webServer_.send(uploadSucceeded_ ? 200 : 400, "text/html",
                    AppLoaderWebVw::resultPage(uploadStatus_));
  }

  void handleUploadData() {
    HTTPUpload &upload = webServer_.upload();
    if (upload.status == UPLOAD_FILE_START) {
      beginWebUpload(upload.filename.c_str());
      return;
    }
    if (upload.status == UPLOAD_FILE_WRITE) {
      if (!uploadInProgress_ || uploadFailed_) return;
      const esp_partition_t *target = bootMgr_.target();
      if (target == nullptr || uploadBytes_ + upload.currentSize > target->size ||
          uploadFile_.write(upload.buf, upload.currentSize) != upload.currentSize) {
        failWebUpload("Upload exceeded target partition size or SD write failed.");
        return;
      }
      uploadBytes_ += upload.currentSize;
      delay(0);
      return;
    }
    if (upload.status == UPLOAD_FILE_END) {
      finishWebUpload();
      return;
    }
    if (upload.status == UPLOAD_FILE_ABORTED) {
      failWebUpload("Upload was aborted.");
    }
  }

  // Stage incoming bytes outside /apps so a partial transfer is excluded
  // from the application listing. Existing images are kept on name conflicts.
  void beginWebUpload(const char *originalName) {
    uploadSucceeded_ = false;
    uploadFailed_ = false;
    uploadBytes_ = 0;
    uploadFilename_[0] = '\0';
    uploadPartPath_[0] = '\0';
    if (!safeBinFilename(originalName, uploadFilename_, sizeof(uploadFilename_))) {
      strlcpy(uploadStatus_, "Rejected: filename must be a safe .bin name.", sizeof(uploadStatus_));
      uploadFailed_ = true;
      return;
    }
    if (repo_.exists(uploadFilename_)) {
      snprintf(uploadStatus_, sizeof(uploadStatus_), "Rejected: %s already exists in /apps.", uploadFilename_);
      uploadFailed_ = true;
      return;
    }
    snprintf(uploadPartPath_, sizeof(uploadPartPath_), "%s/%s.part", INCOMING_DIR, uploadFilename_);
    if (SD.exists(uploadPartPath_)) SD.remove(uploadPartPath_);
    uploadFile_ = SD.open(uploadPartPath_, FILE_WRITE);
    if (!uploadFile_) {
      strlcpy(uploadStatus_, "Rejected: could not create the incoming .part file.", sizeof(uploadStatus_));
      uploadFailed_ = true;
      return;
    }
    uploadInProgress_ = true;
    state_ = AppLoaderState::ReceivingApp;
    snprintf(uploadStatus_, sizeof(uploadStatus_), "Receiving %s...", uploadFilename_);
    serial_.write("Web application upload started.\r\n");
    ble_.write("Web application upload started.\r\n");
  }

  void finishWebUpload() {
    if (!uploadInProgress_ || uploadFailed_) return;
    uploadFile_.flush();
    uploadFile_.close();
    uploadInProgress_ = false;
    if (uploadBytes_ == 0) {
      failWebUpload("Upload was empty.");
      return;
    }
    char finalPath[MAX_APP_FILENAME + 8];
    snprintf(finalPath, sizeof(finalPath), "%s/%s", APPS_DIR, uploadFilename_);
    if (!SD.rename(uploadPartPath_, finalPath)) {
      failWebUpload("Could not commit the staged file into /apps.");
      return;
    }
    // The validator opens images through the /apps repository. Remove the
    // promoted file if that validation fails.
    AppLoaderAppImage image;
    if (!validate(uploadFilename_, image, serial_)) {
      SD.remove(finalPath);
      uploadFailed_ = true;
      strlcpy(uploadStatus_, "Rejected: uploaded file failed ESP image validation.", sizeof(uploadStatus_));
      ble_.write("ERR uploaded file failed validation and was removed.\r\n");
      log_.event("Web addapp validation failed");
      state_ = AppLoaderState::UploadMode;
      return;
    }
    uploadSucceeded_ = true;
    uploadFailed_ = false;
    snprintf(uploadStatus_, sizeof(uploadStatus_),
             "SUCCESS: %s added to /apps (%lu bytes). Startup selection was not changed.",
             uploadFilename_, static_cast<unsigned long>(uploadBytes_));
    ble_.write("SUCCESS: application added to SD. Startup selection was not changed.\r\n");
    serial_.write("SUCCESS: Web application upload validated and added to /apps.\r\n");
    log_.event("Web addapp completed");
    state_ = AppLoaderState::UploadMode;
  }

  void failWebUpload(const char *message) {
    if (uploadFile_) uploadFile_.close();
    uploadInProgress_ = false;
    uploadSucceeded_ = false;
    uploadFailed_ = true;
    if (uploadPartPath_[0] != '\0' && SD.exists(uploadPartPath_)) SD.remove(uploadPartPath_);
    strlcpy(uploadStatus_, message != nullptr ? message : "Upload failed.", sizeof(uploadStatus_));
    serial_.write("ERR Web application upload failed.\r\n");
    ble_.write("ERR Web application upload failed.\r\n");
    log_.event("Web addapp failed");
    state_ = AppLoaderState::UploadMode;
  }

  static bool safeBinFilename(const char *input, char *output, size_t capacity) {
    if (input == nullptr || capacity == 0) return false;
    const char *base = strrchr(input, '/');
    base = base != nullptr ? base + 1 : input;
    const char *backslash = strrchr(base, '\\');
    if (backslash != nullptr) base = backslash + 1;
    const size_t length = strlen(base);
    if (length < 5 || length >= capacity || strcasecmp(base + length - 4, ".bin") != 0) return false;
    for (size_t i = 0; i < length; ++i) {
      const char c = base[i];
      if (!(isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '_' || c == '-')) return false;
    }
    strlcpy(output, base, capacity);
    return true;
  }

  static const AppLoaderProvisionIdentity &loaderIdentity() {
    // Referenced at runtime and retained as a contiguous record in the image.
    static const AppLoaderProvisionIdentity identity __attribute__((used)) = {
      "EspAppLoader.identity:1:8a7c6d21-4fa0-4e58-b2e5-7a2fc158d402",
      ESP_APPLOADER_VERSION, ESP_APPLOADER_BUILD_ID
    };
    return identity;
  }

  static void provisioningYield() { delay(0); }

  void provisioningError(AppLoaderCmdChannel &channel, const char *code) {
    char line[160];
    snprintf(line, sizeof(line), "ERR appLdr2App1 code=%s\r\n", code);
    channel.write(line);
  }

  bool clearProvisioningHold(AppLoaderCmdChannel &channel) {
    if (provisioningStateReady_ && provisioningRecord_.mode == AppLoaderProvisionState::None)
      return true;
    if (!provisioningState_.clear()) {
      provisioningError(channel, "state_write");
      return false;
    }
    provisioningRecord_ = AppLoaderProvisionState::Record{};
    provisioningStateReady_ = true;
    return true;
  }

  void provisionCommand(const char *argument, AppLoaderCmdChannel &channel) {
    const int option = AppLoaderProvisionMgr::bootOption(argument);
    if (option < 0) {
      provisioningError(channel, "usage_expected_0_or_1");
      return;
    }
    if (ESP_APPLOADER_TEST_MODE != 0) {
      provisioningError(channel, "test_mode");
      return;
    }
#if defined(CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE) && CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
    // Existing loader startup has no qualified first-boot health handshake.
    provisioningError(channel, "rollback_profile_unsupported");
    return;
#endif
    if (uploadModeActive_ || uploadInProgress_) {
      provisioningError(channel, "upload_active");
      return;
    }
    bootMgr_.inspect();
    const esp_partition_t *running = bootMgr_.running();
    const esp_partition_t *loader = bootMgr_.loader();
    const esp_partition_t *operational = bootMgr_.target();
    if (!running || !loader || !operational ||
        strcmp(running->label, "app0") || strcmp(loader->label, "app1") ||
        strcmp(operational->label, "app0") ||
        running->address != operational->address) {
      provisioningError(channel, "requires_loader_running_in_app0");
      return;
    }
    const esp_err_t identityResult = provisioningMgr_.inspectIdentity(running, loaderIdentity());
    if (identityResult != ESP_OK) {
      char error[180];
      snprintf(error, sizeof(error),
               "ERR appLdr2App1 code=source_not_apploader detail=%s\r\n",
               esp_err_to_name(identityResult));
      channel.write(error);
      return;
    }
    const AppLoaderProvisionIdentity &identity = provisioningMgr_.sourceIdentity();
    char identityLine[220];
    snprintf(identityLine, sizeof(identityLine),
             "PROVISION state=identity_verified source=app0 product=EspAppLoader version=%s build=%s\r\n",
             identity.version, identity.build);
    channel.write(identityLine);
    const AppLoaderProvisionIdentity &sourceIdentity = provisioningMgr_.sourceIdentity();
    char identityLine[200];
    snprintf(identityLine, sizeof(identityLine),
             "PROVISION state=identity_verified source=app0 product=EspAppLoader version=%.31s build=%.47s\r\n",
             sourceIdentity.version, sourceIdentity.build);
    channel.write(identityLine);
    AppLoaderProvisionState::Record request;
    request.mode = option == 0 ? AppLoaderProvisionState::Install
                               : AppLoaderProvisionState::Hold;
    if (request.mode == AppLoaderProvisionState::Install) {
      if (!resolveArgument("default", request.filename, channel)) {
        provisioningError(channel, "sd_default_unavailable");
        return;
      }
      AppLoaderAppImage image;
      if (!validate(request.filename, image, channel)) {
        provisioningError(channel, "sd_default_invalid");
        return;
      }
      request.size = image.fileSize;
      memcpy(request.digest, image.sha256, sizeof(request.digest));
    }
    channel.write("PROVISION state=copying source=app0 destination=app1\r\n");
    const esp_err_t result = provisioningMgr_.copy(false, running, loader,
                                                  operational, loaderIdentity(), provisioningYield);
    if (result != ESP_OK) {
      char line[180];
      snprintf(line, sizeof(line), "ERR appLdr2App1 code=copy_failed detail=%s\r\n",
               esp_err_to_name(result));
      channel.write(line);
      return;
    }
    if (!provisioningState_.save(request)) {
      provisioningError(channel, "state_write");
      return;
    }
    // This is a separate, narrow provisioning path. Normal installer policy
    // continues to reject writing app0 while app0 is running.
    const esp_err_t bootResult = esp_ota_set_boot_partition(loader);
    if (bootResult != ESP_OK) {
      provisioningError(channel, "boot_selection_failed");
      return;
    }
    channel.write("OK appLdr2App1 stage=copy_verified next_boot=app1\r\n");
    if (request.mode == AppLoaderProvisionState::Install)
      channel.write("PROVISION state=handoff: app1 will install the saved SD default and boot app0.\r\n");
    delay(350);
    ESP.restart();
  }

  void resumeProvisioning(bool recoveryRequested) {
    // Consume automatic installation BEFORE modifying app0. A reset or failure
    // thereafter holds app1 for inspection, rather than repeating flash writes.
    const AppLoaderProvisionState::Record request = provisioningRecord_;
    AppLoaderProvisionState::Record hold;
    hold.mode = AppLoaderProvisionState::Hold;
    if (!provisioningState_.save(hold)) {
      provisioningError(serial_, "state_write");
      return;
    }
    provisioningRecord_ = hold;
    bootMgr_.inspect();
    if (!bootMgr_.canMutateFlash() ||
        strcmp(bootMgr_.running()->label, "app1") ||
        !bootMgr_.boot() ||
        bootMgr_.boot()->address != bootMgr_.running()->address) {
      provisioningError(serial_, "handoff_requires_app1");
      return;
    }
    if (recoveryRequested) {
      provisioningError(serial_, "recovery_requested");
      return;
    }
    if (!repo_.ready()) {
      provisioningError(serial_, "sd_unavailable");
      return;
    }
    AppLoaderAppImage image;
    if (!validate(request.filename, image, serial_) ||
        image.fileSize != request.size ||
        memcmp(image.sha256, request.digest, sizeof(request.digest))) {
      provisioningError(serial_, "sd_source_changed_or_invalid");
      return;
    }
    // performInstall also revalidates and compares flashed bytes with the SD
    // source; it never writes the currently running partition.
    performInstall(request.filename, serial_, &request);
    if (state_ != AppLoaderState::ReadyToBoot) {
      provisioningError(serial_, "install_failed");
      return;
    }
    // Use the exact captured candidate, independent of later /boot.cfg edits.
    bootExistingApp(serial_, true);
  }

  void startupCommand(const char *argument, AppLoaderCmdChannel &channel) {
    char filename[MAX_APP_FILENAME];
    if (!resolveArgument(argument, filename, channel)) return;
    AppLoaderAppImage image;
    if (!validate(filename, image, channel)) return;
    char error[180];
    if (!bootCfg_.selectDefault(filename, error, sizeof(error))) {
      char line[220];
      snprintf(line, sizeof(line), "ERR startup selection failed: %s\r\n", error);
      channel.write(line);
      return;
    }
    char line[180];
    snprintf(line, sizeof(line),
             "SUCCESS: startup application selected: %s. No install or reboot was performed.\r\n",
             filename);
    channel.write(line);
    log_.event("Default startup application changed");
  }

  void installCommand(const char *argument, AppLoaderCmdChannel &channel) {
    if (!bootMgr_.canMutateFlash()) {
      channel.write("ERR installation disabled: test mode or invalid running/target roles.\r\n");
      return;
    }
    if (!resolveArgument(argument, pendingFilename_, channel)) return;
    AppLoaderAppImage image;
    if (!validate(pendingFilename_, image, channel)) return;
    char line[180];
    snprintf(line, sizeof(line),
             "Install %s into %s? Type YES exactly to continue; anything else cancels.\r\n",
             pendingFilename_, APP_TARGET_PARTITION_LABEL);
    channel.write(line);
    confirmationPending_ = true;
    state_ = AppLoaderState::AwaitingConfirmation;
  }

  // Installation and boot selection are separate operations. A successful
  // write and read-back leave the loader in control until a boot command.
  void performInstall(const char *filename, AppLoaderCmdChannel &channel,
                      const AppLoaderProvisionState::Record *expected = nullptr) {
    if (!bootMgr_.canMutateFlash()) {
      channel.write("ERR installation disabled by build/partition policy.\r\n");
      return;
    }
    // Revalidate immediately before destructive flash writing. The SD file may
    // have changed between the initial display and confirmation.
    AppLoaderAppImage image;
    if (!validate(filename, image, channel)) return;
    if (expected && (image.fileSize != expected->size ||
        memcmp(image.sha256, expected->digest, sizeof(expected->digest)))) {
      provisioningError(channel, "sd_source_changed");
      state_ = AppLoaderState::InstallationFailed;
      return;
    }
    state_ = AppLoaderState::Installing;
    transactionPrevious_[0] = '\0';
    readInstalledFilename(transactionPrevious_, sizeof(transactionPrevious_));
    writeInstallTransaction("installing", filename, "");
    log_.event("Installation started");
    channel.write("Installation started. Do not remove power.\r\n");
    char error[180];
    if (!installer_.install(repo_, image, bootMgr_, channel, error, sizeof(error))) {
      char line[220];
      snprintf(line, sizeof(line), "ERR installation failed: %s\r\n", error);
      channel.write(line);
      log_.installation(identity_, image, "SD:/apps",
                        APP_TARGET_PARTITION_LABEL, "FAILED", error);
      log_.event("Installation failed");
      writeInstallTransaction("failed", filename, error);
      state_ = AppLoaderState::InstallationFailed;
      return;
    }
    // ESP image validation checks bootability. Compare the flashed bytes
    // with the selected source independently before recording success.
    state_ = AppLoaderState::ValidatingTarget;
    char readbackMessage[160];
    snprintf(readbackMessage, sizeof(readbackMessage),
             "Reading %s back and comparing SHA-256 with the SD source...\r\n",
             APP_TARGET_PARTITION_LABEL);
    channel.write(readbackMessage);
    if (!validator_.validateFlashedImage(bootMgr_.target(), image, error, sizeof(error))) {
      char sourceHash[65], flashHash[65], line[220];
      AppLoaderLogMdl::hashHex(image.sha256, image.hashAvailable,
                               sourceHash, sizeof(sourceHash));
      AppLoaderLogMdl::hashHex(image.flashSha256, image.flashHashAvailable,
                               flashHash, sizeof(flashHash));
      snprintf(line, sizeof(line),
               "ERR flash read-back validation failed: %s\r\n", error);
      channel.write(line);
      snprintf(line, sizeof(line), "Source SHA-256: %s\r\n", sourceHash);
      channel.write(line);
      snprintf(line, sizeof(line), "Flash SHA-256:  %s\r\n", flashHash);
      channel.write(line);
      channel.write("Boot partition was NOT changed.\r\n");
      log_.installation(identity_, image, "SD:/apps",
                        APP_TARGET_PARTITION_LABEL, "FAILED", error);
      log_.event("Flash read-back SHA-256 validation failed");
      writeInstallTransaction("failed", filename, error);
      state_ = AppLoaderState::InstallationFailed;
      return;
    }

    char matchedHash[65], hashLine[180];
    AppLoaderLogMdl::hashHex(image.flashSha256, image.flashHashAvailable,
                             matchedHash, sizeof(matchedHash));
    snprintf(hashLine, sizeof(hashLine),
             "Flash read-back SHA-256 MATCH: %s\r\n", matchedHash);
    channel.write(hashLine);

    log_.installation(identity_, image, "SD:/apps",
                      APP_TARGET_PARTITION_LABEL, "SUCCESS", "");
    if (!writeInstalledRecord(image)) {
      channel.write("WARNING: image is installed, but /apploader/installed.cfg could not be written.\r\n");
      log_.event("Installed image record write failed");
    }
    writeInstallTransaction("pending_health", filename, "");
    log_.event("Installation complete; AppLoader remains selected for boot");
    state_ = AppLoaderState::ReadyToBoot;
    channel.write("SUCCESS: image installed and validated. AppLoader remains the persistent boot partition.\r\nType bootapp to select and start the operational application.\r\n");
  }

  bool inspectExistingApp(AppLoaderCmdChannel &channel) {
    bootMgr_.inspect();
    const esp_err_t result = flashImageMgr_.inspect(
        bootMgr_.target(), bootMgr_.running());
    if (result != ESP_OK) {
      char line[160];
      snprintf(line, sizeof(line), "ERR existing image validation: %s\r\n",
               esp_err_to_name(result));
      channel.write(line);
      return false;
    }
    char hash[65];
    AppLoaderLogMdl::hashHex(flashImageMgr_.digest(), true, hash, sizeof(hash));
    const esp_app_desc_t &description = flashImageMgr_.description();
    char line[300];
    snprintf(line, sizeof(line),
        "Validated %s: %lu bytes\r\nProject: %.32s\r\nVersion: %.32s\r\nSHA256: %s\r\n",
        bootMgr_.target()->label, static_cast<unsigned long>(flashImageMgr_.length()),
        description.project_name, description.version, hash);
    channel.write(line);
    channel.write("Image integrity passed; functional health is untested. Boot selection unchanged.\r\n");
    return true;
  }

  void bootExistingApp(AppLoaderCmdChannel &channel, bool provisioning = false) {
    bootMgr_.inspect();
    if (!bootMgr_.canMutateFlash()) {
      channel.write("ERR boot disabled by build/partition policy.\r\n");
      return;
    }
    // Always validate now, never trust a cached inspection or stale SD record.
    if (!inspectExistingApp(channel)) {
      if (provisioning) provisioningError(channel, "installed_image_invalid");
      return;
    }
    if (!clearProvisioningHold(channel)) return;
    const esp_err_t result = bootMgr_.selectOperationalApp();
    if (result != ESP_OK) {
      char line[160];
      snprintf(line, sizeof(line), "ERR selecting existing app: %s\r\n",
               esp_err_to_name(result));
      channel.write(line);
      if (provisioning) {
        provisioningRecord_.mode = AppLoaderProvisionState::Hold;
        provisioningState_.save(provisioningRecord_);
        provisioningError(channel, "boot_select_failed");
      }
      return;
    }
    if (provisioning) channel.write("OK appLdr2App1 stage=install_verified next_boot=app0\r\n");
    state_ = AppLoaderState::ReadyToBoot;
    if (repo_.ready()) log_.event("Boot of validated existing flash image");
    channel.write("Booting existing app; app0 remains selected after reset. Automatic return is not implemented.\r\n");
    delay(350);
    ESP.restart();
  }

  // Match the recorded installation to the configured default before
  // pointing the ESP bootloader at the operational slot.
  void bootApp(AppLoaderCmdChannel &channel) {
    if (!bootMgr_.canMutateFlash()) {
      channel.write("ERR bootapp disabled: test mode or invalid running/target roles.\r\n");
      return;
    }
    if (!bootCfg_.load()) {
      channel.write("ERR cannot boot default application: /boot.cfg is missing or invalid.\r\n");
      return;
    }
    char installedFilename[MAX_APP_FILENAME];
    if (!readInstalledFilename(installedFilename, sizeof(installedFilename))) {
      channel.write("ERR cannot confirm installed application: /apploader/installed.cfg is missing or invalid.\r\n");
      return;
    }
    if (strcmp(installedFilename, bootCfg_.defaultApp()) != 0) {
      char line[240];
      snprintf(line, sizeof(line),
               "ERR installed app %s does not match Default_App %s. Use install default first.\r\n",
               installedFilename, bootCfg_.defaultApp());
      channel.write(line);
      return;
    }
    if (!inspectExistingApp(channel) || !clearProvisioningHold(channel)) return;
    const esp_partition_t *target = bootMgr_.target();
    const esp_partition_t *verified = esp_ota_get_boot_partition();
    if (verified == nullptr || verified->address != target->address) {
      // Permit booting an already installed valid app, but only if the ESP OTA
      // subsystem accepts it as a boot target.
      const esp_err_t err = bootMgr_.selectOperationalApp();
      if (err != ESP_OK) {
        char line[160];
        snprintf(line, sizeof(line), "ERR app target is not bootable: %s\r\n", esp_err_to_name(err));
        channel.write(line);
        return;
      }
    }
    state_ = AppLoaderState::ReadyToBoot;
    log_.event("Booting operational application");
    channel.write("Booting operational application...\r\n");
    delay(350);
    ESP.restart();
  }

  bool writeInstalledRecord(const AppLoaderAppImage &image) {
    if (SD.exists(INSTALLED_TMP_PATH)) SD.remove(INSTALLED_TMP_PATH);
    File out = SD.open(INSTALLED_TMP_PATH, FILE_WRITE);
    if (!out) return false;
    char hash[65];
    AppLoaderLogMdl::hashHex(image.sha256, image.hashAvailable, hash, sizeof(hash));
    out.println("Installed_Record_Version=1");
    out.print("Image_File="); out.println(image.filename);
    out.print("Image_Size="); out.println(image.fileSize);
    out.print("Image_SHA256="); out.println(hash);
    out.print("AppLoader_Version="); out.println(ESP_APPLOADER_VERSION);
    out.println("Install_State=pending_health");
    out.close();
    if (SD.exists(INSTALLED_CFG_PATH) && !SD.remove(INSTALLED_CFG_PATH)) return false;
    return SD.rename(INSTALLED_TMP_PATH, INSTALLED_CFG_PATH);
  }

  bool writeInstallTransaction(const char *state, const char *filename, const char *error) {
    if (SD.exists(TRANSACTION_TMP_PATH)) SD.remove(TRANSACTION_TMP_PATH);
    File out = SD.open(TRANSACTION_TMP_PATH, FILE_WRITE);
    if (!out) return false;
    out.println("Install_Transaction_Version=1");
    out.print("State="); out.println(state != nullptr ? state : "unknown");
    out.print("Requested_Image="); out.println(filename != nullptr ? filename : "");
    out.print("Previous_Image="); out.println(transactionPrevious_);
    out.print("Target_Partition="); out.println(APP_TARGET_PARTITION_LABEL);
    out.print("Error="); out.println(error != nullptr ? error : "");
    out.flush();
    out.close();
    if (SD.exists(TRANSACTION_CFG_PATH) && !SD.remove(TRANSACTION_CFG_PATH)) return false;
    return SD.rename(TRANSACTION_TMP_PATH, TRANSACTION_CFG_PATH);
  }

  bool readInstalledFilename(char *filename, size_t capacity) {
    if (capacity == 0) return false;
    filename[0] = '\0';
    File in = SD.open(INSTALLED_CFG_PATH, FILE_READ);
    if (!in) return false;
    char line[160];
    while (in.available()) {
      const size_t count = in.readBytesUntil('\n', line, sizeof(line) - 1);
      line[count] = '\0';
      if (strncmp(line, "Image_File=", 11) == 0) {
        char *value = line + 11;
        const size_t length = strcspn(value, "\r\n");
        value[length] = '\0';
        strlcpy(filename, value, capacity);
        in.close();
        return filename[0] != '\0';
      }
    }
    in.close();
    return false;
  }

  void printLogs(AppLoaderCmdChannel &channel) {
    File log = SD.open(EVENT_LOG_PATH, FILE_READ);
    if (!log) {
      channel.write("No AppLoader event log available.\r\n");
      return;
    }
    char buffer[129];
    while (log.available()) {
      const size_t n = log.readBytes(buffer, sizeof(buffer) - 1);
      buffer[n] = '\0';
      channel.write(buffer);
      delay(0);
    }
    log.close();
    channel.write("\r\n");
  }
};


static AppLoaderMgr appLoader;
void appLoaderSetup() { appLoader.begin(); }
void appLoaderLoop() { appLoader.run(); }

} // namespace esp_apploader
