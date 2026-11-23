/*
 * EspAppLoader
 * File: AppLoaderMdl.h
 * Purpose: Implements the loader models and services for identity, boot settings, storage, and installation.
 *
 * Copyright (c) 2026 Darryl Cornish
 */

// SPDX-License-Identifier: MPL-2.0

#pragma once
#include "AppLoaderChannels.h"
#include "AppLoaderPolicy.h"

namespace esp_apploader {

/** Builds the hardware UID and reads the configured WaterThing unit number. */
class AppLoaderIdentityMdl {
public:
  bool begin() {
    const uint64_t raw = ESP.getEfuseMac();
    snprintf(deviceUid_, sizeof(deviceUid_), "WTg-%04X%08X",
             static_cast<uint16_t>(raw >> 32), static_cast<uint32_t>(raw));
    unitNum_ = readUnitNum();
    return true;
  }

  const char *deviceUid() const { return deviceUid_; }
  int unitNum() const { return unitNum_; }

private:
  char deviceUid_[17] = "WTg-UNKNOWN";
  int unitNum_ = -1;

  int readUnitNum() {
    File cfg = SD.open(SETUP_CFG_PATH, FILE_READ);
    if (!cfg) return -1;
    char line[128];
    while (cfg.available()) {
      const size_t n = cfg.readBytesUntil('\n', line, sizeof(line) - 1);
      line[n] = '\0';
      char *p = line;
      while (*p == ' ' || *p == '\t') ++p;
      if (strncmp(p, "Unit_Num=", 9) == 0) {
        cfg.close();
        return atoi(p + 9);
      }
    }
    cfg.close();
    return -1;
  }
};

// Station-mode server pull is deliberately deferred. The initial addapp
// workflow hosts a local access point and browser upload page instead.

/** Loads, validates, caches, and transactionally updates /boot.cfg. */
// Prefer SD boot configuration when valid. NVS provides a cached copy
// for cases where the removable configuration cannot be read.
class AppLoaderBootCfgMdl {
public:
  bool load() {
    reset();
    if (loadFromSd()) {
      strlcpy(source_, "SD:/boot.cfg", sizeof(source_));
      cacheToNvs();
      return true;
    }
    reset();
    if (loadFromNvs()) {
      strlcpy(source_, "NVS fallback", sizeof(source_));
      return true;
    }
    reset();
    return false;
  }

  bool valid() const { return valid_; }
  int protocolVersion() const { return protocolVersion_; }
  const char *defaultApp() const { return defaultApp_; }
  const char *source() const { return source_; }

  bool selectDefault(const char *filename, char *error, size_t errorCapacity) {
    if (!validBinFilename(filename)) return fail(error, errorCapacity, "invalid application filename");
    if (SD.exists(BOOT_CFG_TMP_PATH)) SD.remove(BOOT_CFG_TMP_PATH);
    File out = SD.open(BOOT_CFG_TMP_PATH, FILE_WRITE);
    if (!out) return fail(error, errorCapacity, "could not create boot.cfg.new");

    bool wroteProtocol = false;
    File in = SD.open(BOOT_CFG_PATH, FILE_READ);
    char line[192];
    if (in) {
      while (in.available()) {
        const size_t count = in.readBytesUntil('\n', line, sizeof(line) - 1);
        line[count] = '\0';
        char probe[192];
        strlcpy(probe, line, sizeof(probe));
        char *text = trimInPlace(probe);
        if (strncmp(text, "Default_App=", 12) == 0) continue;
        if (strncmp(text, "Boot_Protocol_Version=", 22) == 0) wroteProtocol = true;
        out.println(line);
      }
      in.close();
    }
    if (!wroteProtocol) out.println("Boot_Protocol_Version=1");
    out.print("Default_App=");
    out.println(filename);
    out.flush();
    out.close();

    if (SD.exists(BOOT_CFG_BACKUP_PATH)) SD.remove(BOOT_CFG_BACKUP_PATH);
    if (SD.exists(BOOT_CFG_PATH) && !SD.rename(BOOT_CFG_PATH, BOOT_CFG_BACKUP_PATH))
      return fail(error, errorCapacity, "could not back up boot.cfg");
    if (!SD.rename(BOOT_CFG_TMP_PATH, BOOT_CFG_PATH)) {
      if (SD.exists(BOOT_CFG_BACKUP_PATH)) SD.rename(BOOT_CFG_BACKUP_PATH, BOOT_CFG_PATH);
      return fail(error, errorCapacity, "could not commit boot.cfg.new");
    }
    if (!load()) {
      SD.remove(BOOT_CFG_PATH);
      if (SD.exists(BOOT_CFG_BACKUP_PATH)) SD.rename(BOOT_CFG_BACKUP_PATH, BOOT_CFG_PATH);
      load();
      return fail(error, errorCapacity, "new boot.cfg failed validation; previous file restored");
    }
    if (SD.exists(BOOT_CFG_BACKUP_PATH)) SD.remove(BOOT_CFG_BACKUP_PATH);
    if (errorCapacity > 0) error[0] = '\0';
    return true;
  }

  void printTo(AppLoaderCmdChannel &channel) const {
    char line[180];
    snprintf(line, sizeof(line), "Boot_Config: %s\r\n", valid_ ? "VALID" : "INVALID OR MISSING");
    channel.write(line);
    snprintf(line, sizeof(line), "Boot_Config_Source: %s\r\n",
             source_[0] != '\0' ? source_ : "compiled recovery default");
    channel.write(line);
    snprintf(line, sizeof(line), "Boot_Protocol_Version: %d\r\n", protocolVersion_);
    channel.write(line);
    snprintf(line, sizeof(line), "Default_App: %s\r\n",
             defaultApp_[0] != '\0' ? defaultApp_ : "unavailable");
    channel.write(line);
  }

private:
  bool valid_ = false;
  int protocolVersion_ = 0;
  char defaultApp_[MAX_APP_FILENAME] = {};
  char source_[32] = {};

  void reset() {
    valid_ = false;
    protocolVersion_ = 0;
    defaultApp_[0] = '\0';
    source_[0] = '\0';
  }

  bool loadFromSd() {
    File cfg = SD.open(BOOT_CFG_PATH, FILE_READ);
    if (!cfg) return false;

    bool sawVersion = false;
    bool sawDefault = false;
    char line[160];
    while (cfg.available()) {
      const size_t count = cfg.readBytesUntil('\n', line, sizeof(line) - 1);
      line[count] = '\0';
      char *text = trimInPlace(line);
      if (*text == '\0' || *text == '#') continue;
      char *equals = strchr(text, '=');
      if (equals == nullptr) continue;
      *equals++ = '\0';
      char *key = trimInPlace(text);
      char *value = trimInPlace(equals);
      if (strcmp(key, "Boot_Protocol_Version") == 0) {
        protocolVersion_ = atoi(value);
        sawVersion = true;
      } else if (strcmp(key, "Default_App") == 0) {
        strlcpy(defaultApp_, value, sizeof(defaultApp_));
        sawDefault = true;
      }
    }
    cfg.close();

    valid_ = sawVersion && sawDefault && protocolVersion_ == 1 &&
             validBinFilename(defaultApp_);
    return valid_;
  }

  bool loadFromNvs() {
    Preferences preferences;
    if (!preferences.begin("wtgboot", true)) return false;
    protocolVersion_ = static_cast<int>(preferences.getUInt("protocol", 0));
    const size_t count = preferences.getString("default", defaultApp_, sizeof(defaultApp_));
    preferences.end();
    valid_ = count > 0 && protocolVersion_ == 1 && validBinFilename(defaultApp_);
    return valid_;
  }

  bool cacheToNvs() const {
    char cachedApp[MAX_APP_FILENAME] = {};
    uint32_t cachedProtocol = 0;
    Preferences current;
    if (current.begin("wtgboot", true)) {
      cachedProtocol = current.getUInt("protocol", 0);
      current.getString("default", cachedApp, sizeof(cachedApp));
      current.end();
    }
    if (cachedProtocol == static_cast<uint32_t>(protocolVersion_) &&
        strcmp(cachedApp, defaultApp_) == 0) return true;

    Preferences preferences;
    if (!preferences.begin("wtgboot", false)) return false;
    const size_t versionBytes = preferences.putUInt("protocol", protocolVersion_);
    const size_t appBytes = preferences.putString("default", defaultApp_);
    preferences.end();
    return versionBytes > 0 && appBytes > 0;
  }

  static char *trimInPlace(char *text) {
    while (*text == ' ' || *text == '\t' || *text == '\r') ++text;
    size_t length = strlen(text);
    while (length > 0 &&
           (text[length - 1] == ' ' || text[length - 1] == '\t' || text[length - 1] == '\r')) {
      text[--length] = '\0';
    }
    return text;
  }

  static bool validBinFilename(const char *filename) {
    if (filename == nullptr || *filename == '\0' || strchr(filename, '/') != nullptr ||
        strchr(filename, '\\') != nullptr) return false;
    const size_t length = strlen(filename);
    return length >= 5 && strcasecmp(filename + length - 4, ".bin") == 0;
  }

  static bool fail(char *error, size_t capacity, const char *message) {
    strlcpy(error, message, capacity);
    return false;
  }
};

/** Discovers configured roles and checks whether the running image is the loader. */
// Resolve partition roles from ESP metadata and apply the mutation policy
// before a caller writes flash or selects a boot target.
class AppLoaderBootMgr {
public:
  bool inspect() {
    running_ = esp_ota_get_running_partition();
    boot_ = esp_ota_get_boot_partition();
    loader_ = esp_partition_find_first(
      ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, APPLOADER_PARTITION_LABEL);
    target_ = esp_partition_find_first(
      ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, APP_TARGET_PARTITION_LABEL);

    layoutValid_ = running_ != nullptr && loader_ != nullptr && target_ != nullptr &&
                   running_->address == loader_->address &&
                   running_->address != target_->address;
    return layoutValid_;
  }

  bool layoutValid() const { return layoutValid_; }
  bool targetAvailable() const { return target_ != nullptr; }
  bool canMutateFlash() const {
    return AppLoaderPolicy::mayMutate(ESP_APPLOADER_TEST_MODE != 0,
        running_ != nullptr, loader_ != nullptr, target_ != nullptr,
        running_ != nullptr && loader_ != nullptr && running_->address == loader_->address,
        running_ != nullptr && target_ != nullptr && running_->address != target_->address);
  }
  const esp_partition_t *running() const { return running_; }
  const esp_partition_t *boot() const { return boot_; }
  const esp_partition_t *target() const { return target_; }

  bool imageFits(uint32_t size) const {
    return target_ != nullptr && size > 0 && size <= target_->size;
  }

  esp_err_t selectAppLoader() {
    if (!canMutateFlash()) return ESP_ERR_INVALID_STATE;
    return esp_ota_set_boot_partition(loader_);
  }

  esp_err_t selectOperationalApp() {
    if (!canMutateFlash()) return ESP_ERR_INVALID_STATE;
    return esp_ota_set_boot_partition(target_);
  }

  void printTo(AppLoaderCmdChannel &channel) const {
    char line[180];
    printOne(channel, "Running", running_, line, sizeof(line));
    printOne(channel, "Boot", boot_, line, sizeof(line));
    printOne(channel, "AppLoader", loader_, line, sizeof(line));
    printOne(channel, "App target", target_, line, sizeof(line));
    snprintf(line, sizeof(line), "Layout: %s (current roles %s -> %s)\r\n",
             layoutValid_ ? "VALID" : "INVALID",
             APPLOADER_PARTITION_LABEL, APP_TARGET_PARTITION_LABEL);
    channel.write(line);
  }

private:
  const esp_partition_t *running_ = nullptr;
  const esp_partition_t *boot_ = nullptr;
  const esp_partition_t *loader_ = nullptr;
  const esp_partition_t *target_ = nullptr;
  bool layoutValid_ = false;

  static void printOne(AppLoaderCmdChannel &channel, const char *role,
                       const esp_partition_t *partition, char *line, size_t capacity) {
    if (partition == nullptr) {
      snprintf(line, capacity, "%s: NOT FOUND\r\n", role);
    } else {
      snprintf(line, capacity, "%s: %s address=0x%06lX size=0x%06lX (%lu bytes)\r\n",
               role, partition->label,
               static_cast<unsigned long>(partition->address),
               static_cast<unsigned long>(partition->size),
               static_cast<unsigned long>(partition->size));
    }
    channel.write(line);
  }
};

/** Writes human-readable events and auditable installation-history records. */
class AppLoaderLogMdl {
public:
  bool begin() {
    if (!SD.exists(APPLOADER_DIR) && !SD.mkdir(APPLOADER_DIR)) return false;
    if (!SD.exists(INSTALL_LOG_PATH)) {
      File out = SD.open(INSTALL_LOG_PATH, FILE_WRITE);
      if (out) {
        out.println("uptime_ms,device_uid,unit_num,apploader_version,source_filename,image_size,source_sha256,flash_sha256,hash_match,source_location,target_partition,result,error");
        out.close();
      }
    }
    return true;
  }

  void event(const char *message) {
    File out = SD.open(EVENT_LOG_PATH, FILE_APPEND);
    if (!out) return;
    out.print(millis());
    out.print(',');
    out.println(message != nullptr ? message : "");
    out.close();
  }

  void installation(const AppLoaderIdentityMdl &identity, const AppLoaderAppImage &image,
                    const char *sourceLocation, const char *targetPartition,
                    const char *result, const char *error) {
    File out = SD.open(INSTALL_LOG_PATH, FILE_APPEND);
    if (!out) return;
    char sourceHash[65];
    char flashHash[65];
    hashHex(image.sha256, image.hashAvailable, sourceHash, sizeof(sourceHash));
    hashHex(image.flashSha256, image.flashHashAvailable, flashHash, sizeof(flashHash));
    out.print(millis()); out.print(',');
    out.print(identity.deviceUid()); out.print(',');
    out.print(identity.unitNum()); out.print(',');
    out.print(ESP_APPLOADER_VERSION); out.print(',');
    out.print(image.filename); out.print(',');
    out.print(image.fileSize); out.print(',');
    out.print(sourceHash); out.print(',');
    out.print(flashHash); out.print(',');
    out.print(image.flashHashAvailable ? (image.flashHashMatches ? "1" : "0") : "unknown"); out.print(',');
    out.print(sourceLocation); out.print(',');
    out.print(targetPartition); out.print(',');
    out.print(result); out.print(',');
    out.println(error != nullptr ? error : "");
    out.close();
  }

  static void hashHex(const uint8_t hash[32], bool available, char *out, size_t capacity) {
    if (!available) {
      strlcpy(out, "unavailable", capacity);
      return;
    }
    if (capacity < 65) {
      if (capacity > 0) out[0] = '\0';
      return;
    }
    for (size_t i = 0; i < 32; ++i) snprintf(out + i * 2, 3, "%02x", hash[i]);
    out[64] = '\0';
  }
};

/** Owns the SD /apps repository and its bounded in-memory file index. */
// Own the SD application directory and resolve command selectors to files.
// Keep image validation and flash writing outside the repository model.
class AppLoaderAppRepoMdl {
public:
  bool begin() {
    ready_ = SD.begin(SD_CS_PIN);
    // Explicit acquisition creates /apps and its staging directory.
    if (ready_) cleanupIncoming();
    return ready_;
  }

  bool ready() const { return ready_; }

  size_t scan() {
    count_ = 0;
    if (!ready_) return 0;
    File directory = SD.open(APPS_DIR);
    if (!directory || !directory.isDirectory()) return 0;
    File entry = directory.openNextFile();
    while (entry && count_ < MAX_APP_FILES) {
      if (!entry.isDirectory()) {
        const char *name = entry.name();
        if (hasBinExtension(name)) {
          const char *base = strrchr(name, '/');
          strlcpy(files_[count_], base != nullptr ? base + 1 : name, MAX_APP_FILENAME);
          ++count_;
        }
      }
      entry.close();
      entry = directory.openNextFile();
    }
    directory.close();
    return count_;
  }

  size_t count() const { return count_; }
  const char *filename(size_t index) const { return index < count_ ? files_[index] : nullptr; }

  bool resolve(const char *selector, char *filenameOut, size_t capacity) {
    if (selector == nullptr || selector[0] == '\0') return false;
    scan();
    char *end = nullptr;
    const long listNumber = strtol(selector, &end, 10);
    if (end != selector && *end == '\0' && listNumber >= 1 &&
        static_cast<size_t>(listNumber) <= count_) {
      strlcpy(filenameOut, files_[listNumber - 1], capacity);
      return true;
    }
    for (size_t i = 0; i < count_; ++i) {
      if (strcasecmp(selector, files_[i]) == 0) {
        strlcpy(filenameOut, files_[i], capacity);
        return true;
      }
    }
    return false;
  }

  File open(const char *filename) {
    char path[MAX_APP_FILENAME + 8];
    snprintf(path, sizeof(path), "%s/%s", APPS_DIR, filename);
    return SD.open(path, FILE_READ);
  }

  bool exists(const char *filename) {
    char path[MAX_APP_FILENAME + 8];
    snprintf(path, sizeof(path), "%s/%s", APPS_DIR, filename);
    return SD.exists(path);
  }

  void cleanupIncoming() {
    if (!ready_ || !SD.exists(INCOMING_DIR)) return;
    File directory = SD.open(INCOMING_DIR);
    if (!directory || !directory.isDirectory()) return;
    File entry = directory.openNextFile();
    while (entry) {
      char path[MAX_APP_FILENAME + 24];
      const char *name = entry.name();
      const char *base = strrchr(name, '/');
      snprintf(path, sizeof(path), "%s/%s", INCOMING_DIR, base != nullptr ? base + 1 : name);
      const bool isDirectory = entry.isDirectory();
      entry.close();
      if (!isDirectory) SD.remove(path);
      entry = directory.openNextFile();
    }
    directory.close();
  }

private:
  bool ready_ = false;
  size_t count_ = 0;
  char files_[MAX_APP_FILES][MAX_APP_FILENAME] = {};

  static bool hasBinExtension(const char *name) {
    if (name == nullptr) return false;
    const size_t length = strlen(name);
    return length >= 4 && strcasecmp(name + length - 4, ".bin") == 0;
  }
};


/** Validates ESP image structure, metadata, size, and source/read-back hashes. */
// Source checks and flash read-back answer different questions: whether the
// SD image can be installed and whether the bytes written match that source.
class AppLoaderAppValidator {
public:
  bool validateSource(AppLoaderAppRepoMdl &repo, const char *filename,
                      const AppLoaderBootMgr &partitions, AppLoaderAppImage &image,
                      char *error, size_t errorCapacity) {
    memset(&image, 0, sizeof(image));
    strlcpy(image.filename, filename, sizeof(image.filename));
    File source = repo.open(filename);
    if (!source) return fail(error, errorCapacity, "could not open image");
    image.fileSize = source.size();
    if (!partitions.imageFits(image.fileSize)) {
      source.close();
      return fail(error, errorCapacity, "image is empty or does not fit target partition");
    }

    uint8_t magic = 0;
    if (source.read(&magic, 1) != 1 || magic != ESP_IMAGE_MAGIC) {
      source.close();
      return fail(error, errorCapacity, "invalid ESP32 image magic");
    }

    source.seek(0x20);
    AppLoaderEmbeddedAppDesc desc = {};
    if (source.read(reinterpret_cast<uint8_t *>(&desc), sizeof(desc)) == sizeof(desc) &&
        desc.magicWord == ESP_APP_DESC_MAGIC) {
      copyDescriptorText(image.appName, sizeof(image.appName), desc.projectName, sizeof(desc.projectName));
      copyDescriptorText(image.appVersion, sizeof(image.appVersion), desc.version, sizeof(desc.version));
      char date[17], time[17];
      copyDescriptorText(date, sizeof(date), desc.buildDate, sizeof(desc.buildDate));
      copyDescriptorText(time, sizeof(time), desc.buildTime, sizeof(desc.buildTime));
      snprintf(image.buildId, sizeof(image.buildId), "%s %s", date, time);
      image.metadataAvailable = true;
    }

    source.seek(0);
    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    if (wtgSha256Start(&sha) != 0) {
      mbedtls_sha256_free(&sha);
      source.close();
      return fail(error, errorCapacity, "SHA-256 initialization failed");
    }
    while (source.available()) {
      const size_t n = source.read(buffer_, sizeof(buffer_));
      if (n == 0 || wtgSha256Update(&sha, buffer_, n) != 0) {
        mbedtls_sha256_free(&sha);
        source.close();
        return fail(error, errorCapacity, "SD read or SHA-256 update failed");
      }
      delay(0);
    }
    image.hashAvailable = wtgSha256Finish(&sha, image.sha256) == 0;
    mbedtls_sha256_free(&sha);
    source.close();
    if (!image.hashAvailable) return fail(error, errorCapacity, "SHA-256 finalization failed");
    if (errorCapacity > 0) error[0] = '\0';
    return true;
  }

  bool validateFlashedImage(const esp_partition_t *partition,
                            AppLoaderAppImage &image,
                            char *error, size_t errorCapacity) {
    image.flashHashAvailable = false;
    image.flashHashMatches = false;
    memset(image.flashSha256, 0, sizeof(image.flashSha256));
    if (partition == nullptr)
      return fail(error, errorCapacity, "flash target partition is unavailable");
    if (!image.hashAvailable || image.fileSize == 0 || image.fileSize > partition->size)
      return fail(error, errorCapacity, "source hash or image size is invalid");

    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    if (wtgSha256Start(&sha) != 0) {
      mbedtls_sha256_free(&sha);
      return fail(error, errorCapacity, "flash SHA-256 initialization failed");
    }

    uint32_t offset = 0;
    while (offset < image.fileSize) {
      const size_t length = min(sizeof(buffer_),
                                static_cast<size_t>(image.fileSize - offset));
      const esp_err_t readErr = esp_partition_read(partition, offset, buffer_, length);
      if (readErr != ESP_OK) {
        mbedtls_sha256_free(&sha);
        snprintf(error, errorCapacity, "esp_partition_read at %lu: %s",
                 static_cast<unsigned long>(offset), esp_err_to_name(readErr));
        return false;
      }
      if (wtgSha256Update(&sha, buffer_, length) != 0) {
        mbedtls_sha256_free(&sha);
        return fail(error, errorCapacity, "flash SHA-256 update failed");
      }
      offset += length;
      delay(0);
    }

    image.flashHashAvailable = wtgSha256Finish(&sha, image.flashSha256) == 0;
    mbedtls_sha256_free(&sha);
    if (!image.flashHashAvailable)
      return fail(error, errorCapacity, "flash SHA-256 finalization failed");

    image.flashHashMatches = memcmp(image.sha256, image.flashSha256, 32) == 0;
    if (!image.flashHashMatches)
      return fail(error, errorCapacity, "source and flashed SHA-256 hashes differ");

    if (errorCapacity > 0) error[0] = '\0';
    return true;
  }

private:
  // Fixed class storage keeps the 4 KiB work area off loopTask's limited
  // call stack. Validation operations are synchronous, so one shared buffer
  // safely serves both SD hashing and flash read-back hashing.
  uint8_t buffer_[FLASH_BUFFER_SIZE] = {};

  static bool fail(char *error, size_t capacity, const char *message) {
    strlcpy(error, message, capacity);
    return false;
  }

  static void copyDescriptorText(char *destination, size_t destinationCapacity,
                                 const char *source, size_t sourceCapacity) {
    if (destinationCapacity == 0) return;
    size_t length = 0;
    while (length < sourceCapacity && source[length] != '\0') ++length;
    length = min(length, destinationCapacity - 1);
    memcpy(destination, source, length);
    destination[length] = '\0';
  }
};

/** Streams a validated SD image into app0 using the ESP OTA write API. */
// Stream the selected image using a reusable buffer so RAM use stays
// bounded by the buffer size even for images that fill the target partition.
class AppLoaderAppInstaller {
public:
  bool install(AppLoaderAppRepoMdl &repo, const AppLoaderAppImage &image,
               AppLoaderBootMgr &partitions, AppLoaderCmdChannel &channel,
               char *error, size_t errorCapacity) {
    const esp_partition_t *target = partitions.target();
    const esp_partition_t *running = partitions.running();
    if (!partitions.canMutateFlash() || target == nullptr || running == nullptr)
      return fail(error, errorCapacity, "invalid partition layout");
    if (target->address == running->address)
      return fail(error, errorCapacity, "refusing to overwrite running partition");
    if (!partitions.imageFits(image.fileSize))
      return fail(error, errorCapacity, "image does not fit target partition");

    File source = repo.open(image.filename);
    if (!source) return fail(error, errorCapacity, "could not reopen source image");

    esp_ota_handle_t handle = 0;
    esp_err_t err = esp_ota_begin(target, image.fileSize, &handle);
    if (err != ESP_OK) {
      source.close();
      snprintf(error, errorCapacity, "esp_ota_begin: %s", esp_err_to_name(err));
      return false;
    }

    uint32_t written = 0;
    uint8_t lastPercent = 255;
    while (written < image.fileSize) {
      const size_t wanted = min(sizeof(buffer_), static_cast<size_t>(image.fileSize - written));
      const size_t n = source.read(buffer_, wanted);
      if (n == 0) {
        esp_ota_abort(handle);
        source.close();
        return fail(error, errorCapacity, "unexpected end of source image");
      }
      err = esp_ota_write(handle, buffer_, n);
      if (err != ESP_OK) {
        esp_ota_abort(handle);
        source.close();
        snprintf(error, errorCapacity, "esp_ota_write: %s", esp_err_to_name(err));
        return false;
      }
      written += n;
      const uint8_t percent = static_cast<uint8_t>((static_cast<uint64_t>(written) * 100U) / image.fileSize);
      if (percent / 5 != lastPercent / 5) {
        char progress[48];
        snprintf(progress, sizeof(progress), "Install progress: %u%%\r\n", percent);
        channel.write(progress);
        lastPercent = percent;
      }
      delay(0);
    }
    source.close();

    // esp_ota_end validates the flashed ESP image before it can be selected.
    err = esp_ota_end(handle);
    if (err != ESP_OK) {
      snprintf(error, errorCapacity, "esp_ota_end/image validation: %s", esp_err_to_name(err));
      return false;
    }

    if (errorCapacity > 0) error[0] = '\0';
    return true;
  }

private:
  // Flash streaming also uses fixed object storage rather than consuming
  // half of loopTask's stack in a nested install command.
  uint8_t buffer_[FLASH_BUFFER_SIZE] = {};

  static bool fail(char *error, size_t capacity, const char *message) {
    strlcpy(error, message, capacity);
    return false;
  }
};

} // namespace esp_apploader
