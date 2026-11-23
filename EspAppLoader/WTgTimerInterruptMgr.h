/*
 * EspAppLoader
 * File: WTgTimerInterruptMgr.h
 * Purpose: Declares the hardware timer interrupt manager.
 */

/*
 * WaterThing Hardware-Timer Interrupt Manager
 *
 * Designed by Darryl Cornish.
 * Written by Darryl Cornish with assistance from various AI systems.
 *
 * Copyright (c) 2026 Darryl Cornish
 * SPDX-License-Identifier: MPL-2.0
 *
 * See LICENSE in the repository root.
 */

#pragma once

#include <Arduino.h>

namespace esp_apploader {

/*
 * WTgTimerInterruptMgr
 * ====================
 *
 * Owns and manages one periodic ESP32 hardware-timer interrupt.
 *
 * Multiple WTgTimerInterruptMgr objects may be created. Each object can use a
 * different interrupt interval and callback. Hardware-timer allocation remains
 * the responsibility of the Arduino-ESP32 timer API.
 *
 * ISR contract
 * ------------
 * The callback executes in interrupt context. It must be short and bounded:
 *
 *   - no Serial output
 *   - no delay()
 *   - no dynamic allocation
 *   - no blocking operations
 *   - no ordinary application callbacks unless they are explicitly ISR-safe
 *
 * The intended pattern is for the callback to update volatile state or perform
 * a small deterministic GPIO operation. Normal program code later consumes
 * any resulting state.
 */
class WTgTimerInterruptMgr {
public:
    /** ISR-safe callback invoked once per configured timer interval. */
    using InterruptHandler = void (*)(void* context);

    WTgTimerInterruptMgr();
    ~WTgTimerInterruptMgr();

    WTgTimerInterruptMgr(const WTgTimerInterruptMgr&) = delete;
    WTgTimerInterruptMgr& operator=(const WTgTimerInterruptMgr&) = delete;

    /*
     * Start a periodic timer.
     *
     * intervalUs: Period between interrupts in microseconds; must be nonzero.
     * handler:    ISR-safe callback; must not be null.
     * context:    Optional caller-owned value passed to handler.
     *
     * Returns false if the arguments are invalid, this object is already
     * running, or the ESP32 hardware timer cannot be allocated.
     */
    bool begin(uint32_t intervalUs,
               InterruptHandler handler,
               void* context = nullptr);

    // Stop the interrupt and release this object's hardware timer.
    void end();

    /** Return true while this object owns an active hardware timer. */
    bool isRunning() const;
    /** Return the configured interrupt period, or zero before begin(). */
    uint32_t intervalUs() const;

private:
    static void ARDUINO_ISR_ATTR interruptThunk(void* argument);
    void ARDUINO_ISR_ATTR handleInterrupt();

    hw_timer_t* timer_;              // Arduino-ESP32 timer owned by this object.
    InterruptHandler handler_;       // Caller callback executed in ISR context.
    void* handlerContext_;           // Opaque caller state passed to handler_.
    uint32_t intervalUs_;            // Configured interrupt period in microseconds.
    bool running_;                   // True after successful begin() until end().
};

} // namespace esp_apploader
