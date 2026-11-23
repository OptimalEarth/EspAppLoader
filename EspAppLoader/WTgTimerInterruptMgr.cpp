/*
 * EspAppLoader
 * File: WTgTimerInterruptMgr.cpp
 * Purpose: Manages the hardware timer used to dispatch periodic interrupts.
 */

/*
 * WaterThing Hardware-Timer Interrupt Manager Implementation
 *
 * Designed by Darryl Cornish.
 * Written by Darryl Cornish with assistance from various AI systems.
 *
 * Copyright (c) 2026 Darryl Cornish
 * SPDX-License-Identifier: MPL-2.0
 *
 * See LICENSE in the repository root.
 */

#include "WTgTimerInterruptMgr.h"

namespace esp_apploader {

namespace {

// A 1 MHz timer frequency gives one hardware-timer count per microsecond.
constexpr uint32_t TimerFrequencyHz = 1000000UL;

}  // namespace

WTgTimerInterruptMgr::WTgTimerInterruptMgr()
    : timer_(nullptr),
      handler_(nullptr),
      handlerContext_(nullptr),
      intervalUs_(0),
      running_(false) {
}

WTgTimerInterruptMgr::~WTgTimerInterruptMgr() {
    end();
}

bool WTgTimerInterruptMgr::begin(uint32_t intervalUs,
                                 InterruptHandler handler,
                                 void* context) {
    if (running_ || intervalUs == 0U || handler == nullptr) {
        return false;
    }

    hw_timer_t* newTimer = timerBegin(TimerFrequencyHz);
    if (newTimer == nullptr) {
        return false;
    }

    timer_ = newTimer;
    handler_ = handler;
    handlerContext_ = context;
    intervalUs_ = intervalUs;

    timerAttachInterruptArg(timer_, interruptThunk, this);
    timerAlarm(timer_, intervalUs_, true, 0);
    running_ = true;
    return true;
}

void WTgTimerInterruptMgr::end() {
    if (timer_ != nullptr) {
        timerEnd(timer_);
    }

    timer_ = nullptr;
    handler_ = nullptr;
    handlerContext_ = nullptr;
    intervalUs_ = 0;
    running_ = false;
}

bool WTgTimerInterruptMgr::isRunning() const {
    return running_;
}

uint32_t WTgTimerInterruptMgr::intervalUs() const {
    return intervalUs_;
}

void ARDUINO_ISR_ATTR
WTgTimerInterruptMgr::interruptThunk(void* argument) {
    static_cast<WTgTimerInterruptMgr*>(argument)->handleInterrupt();
}

void ARDUINO_ISR_ATTR WTgTimerInterruptMgr::handleInterrupt() {
    handler_(handlerContext_);
}

} // namespace esp_apploader
