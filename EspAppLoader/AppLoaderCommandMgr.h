/*
 * EspAppLoader
 * File: AppLoaderCommandMgr.h
 * Purpose: Dispatches commands received through the loader command channels.
 *
 * Copyright (c) 2026 Darryl Cornish
 */

// SPDX-License-Identifier: MPL-2.0

#pragma once
#include "AppLoaderChannels.h"

namespace esp_apploader {

/** Polls each command transport and dispatches complete lines to one handler. */
// Route complete command lines through one dispatcher so serial and BLE
// share the command implementation while replies return to the caller.
class AppLoaderCmdMgr {
public:
  using CommandHandler = void (*)(void *context, char *command, AppLoaderCmdChannel &channel);

  void configure(AppLoaderCmdChannel &first, AppLoaderCmdChannel &second,
                 void *context, CommandHandler handler) {
    channels_[0] = &first;
    channels_[1] = &second;
    context_ = context;
    handler_ = handler;
  }

  void poll() {
    for (AppLoaderCmdChannel *channel : channels_) {
      if (channel == nullptr) continue;
      channel->poll();
      char command[CMD_BUFFER_SIZE];
      if (channel->getLine(command, sizeof(command)) && handler_ != nullptr) {
        handler_(context_, command, *channel);
      }
    }
  }

private:
  AppLoaderCmdChannel *channels_[2] = {nullptr, nullptr};
  void *context_ = nullptr;
  CommandHandler handler_ = nullptr;
};

} // namespace esp_apploader
