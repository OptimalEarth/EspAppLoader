/*
 * EspAppLoader
 * File: AppLoaderWebVw.h
 * Purpose: Renders the Wi-Fi upload page and its result page.
 *
 * Copyright (c) 2026 Darryl Cornish
 */

// SPDX-License-Identifier: MPL-2.0

#pragma once
#include <Arduino.h>

namespace esp_apploader {

class AppLoaderWebVw {
public:
  static String escape(const char *value) {
    String out;
    if (!value) return out;
    while (*value) {
      switch (*value) {
        case '&': out += "&amp;"; break;
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        case '"': out += "&quot;"; break;
        case '\'': out += "&#39;"; break;
        default: out += *value;
      }
      ++value;
    }
    return out;
  }
  static String uploadPage(int unit, const char *version, const char *status, bool busy) {
    String p = "<!doctype html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'><title>AppLoader</title></head><body><h1>AppLoader</h1><p>Unit: ";
    p += String(unit); p += " | Version: "; p += escape(version);
    p += "</p><p>"; p += escape(status); p += "</p>";
    if (!busy) p += "<h2>Add firmware to SD</h2><p>Uploading does not install or start firmware.</p><form method='POST' action='/upload' enctype='multipart/form-data'><input type='file' name='application' accept='.bin' required><button type='submit'>Upload</button></form>";
    p += "<p><a href='/'>Refresh</a> | <a href='/exit'>Exit upload mode</a></p></body></html>";
    return p;
  }
  static String resultPage(const char *status) {
    String p = "<!doctype html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'></head><body><h1>Upload result</h1><p>";
    p += escape(status); p += "</p><a href='/'>Return</a></body></html>"; return p;
  }
};

} // namespace esp_apploader
