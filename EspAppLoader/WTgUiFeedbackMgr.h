/*
 * EspAppLoader
 * File: WTgUiFeedbackMgr.h
 * Purpose: Declares the timed visual feedback manager.
 */

/*
 * WaterThing User-Interface Feedback Manager
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
 * WTgUiFeedbackMgr
 * ================
 *
 * Generates standardized WTg user-feedback patterns on one digital output.
 *
 * This class does not own a hardware timer. The application owns and configures
 * WTgTimerInterruptMgr, then calls onTimerTickIsr() from the application's
 * timer callback. This permits other ISR-safe consumers to share the same base
 * timer frequency.
 *
 * Feedback standard
 * -----------------
 *   ShortSuccess  :  1 flash
 *   MediumSuccess :  2 flashes
 *   LongSuccess   :  3 flashes
 *   Failure       : 10 flashes
 *
 * A success pattern means both the press-duration command and its associated
 * application action completed successfully. Failure means the requested
 * action was not completed.
 *
 * Priority
 * --------
 * A new physical press cancels an active feedback pattern and immediately
 * drives the output active. Live physical input always has priority over an
 * older result pattern. Result patterns have priority over the optional mode
 * indicator. The mode indicator resumes automatically after a press or result
 * pattern finishes.
 */
class WTgUiFeedbackMgr {
public:
    enum class ActiveLevel : uint8_t {
        Low = LOW,
        High = HIGH
    };

    enum class Result : uint8_t {
        ShortSuccess,
        MediumSuccess,
        LongSuccess,
        Failure
    };

    /** Lifetime counters used to verify feedback behavior in field tests. */
    struct Diagnostics {
        uint32_t shortSuccessSignals;
        uint32_t mediumSuccessSignals;
        uint32_t longSuccessSignals;
        uint32_t failureSignals;
        uint32_t patternsCompleted;
        uint32_t patternsCancelledByPress;
        uint32_t physicalLevelCallbacks;
        uint32_t timerTicks;
    };

    WTgUiFeedbackMgr();
    ~WTgUiFeedbackMgr();

    WTgUiFeedbackMgr(const WTgUiFeedbackMgr&) = delete;
    WTgUiFeedbackMgr& operator=(const WTgUiFeedbackMgr&) = delete;

    /*
     * Configure the feedback output and timing.
     *
     * All durations must be nonzero. flashOnUs and flashOffUs must be exact
     * integer multiples of timerTickUs. postReleaseQuietUs establishes the
     * minimum LED-OFF interval between release of the physical button and the
     * first result flash. All resulting tick counts must fit in uint16_t.
     */
    bool begin(uint8_t outputPin,
               ActiveLevel activeLevel,
               uint32_t timerTickUs,
               uint32_t flashOnUs = 100000UL,
               uint32_t flashOffUs = 100000UL,
               uint32_t postReleaseQuietUs = 300000UL);

    /*
     * Stop using the output. The application must stop dispatching timer ticks
     * before calling end().
     */
    void end();

    /*
     * Report the immediate physical button level from the GPIO ISR.
     *
     * This function is ISR-safe. A transition to active cancels any feedback
     * pattern. The caller must pass the logical active state, not the raw GPIO
     * voltage level.
     */
    void ARDUINO_ISR_ATTR setPhysicalPressLevelIsr(bool active);

    /*
     * Schedule a result pattern from normal program context.
     *
     * A new result replaces any older pending pattern. If the physical button
     * is still held, pattern timing waits until it is released.
     */
    bool signal(Result result);

    /*
     * Enable or disable the continuous mode indicator.
     *
     * When enabled, the output is ON for indicatorOnUs and OFF for
     * indicatorOffUs. Durations must be nonzero exact multiples of the base
     * timer tick and must fit in uint16_t tick counts. The default application
     * use is BLE mode indication with 1 second ON and 1 second OFF.
     */
    bool setModeIndicator(bool enabled,
                          uint32_t indicatorOnUs = 1000000UL,
                          uint32_t indicatorOffUs = 1000000UL);

    // Called once for every application-owned base-timer tick.
    void ARDUINO_ISR_ATTR onTimerTickIsr();

    /** Return true after successful configuration and before end(). */
    bool isBegun() const;
    /** Return true while a result flash sequence remains in progress. */
    bool isPatternActive() const;
    /** Copy a coherent diagnostics snapshot into caller-owned storage. */
    void getDiagnostics(Diagnostics& output) const;

private:
    static uint8_t flashesForResult(Result result);
    uint8_t ARDUINO_ISR_ATTR outputLevel(bool active) const;
    void ARDUINO_ISR_ATTR writeOutput(bool active);

    mutable portMUX_TYPE mux_;        // Protects state shared by ISR and task code.
    uint8_t outputPin_;               // GPIO driven for immediate/result feedback.
    uint8_t activeLevel_;             // Electrical level that turns the output on.
    uint32_t timerTickUs_;            // Application-owned base timer period.
    uint16_t onTicks_;                // Timer ticks in each illuminated phase.
    uint16_t offTicks_;               // Timer ticks between flashes.
    uint16_t postReleaseQuietTicks_;  // Required dark ticks after physical release.
    bool begun_;                      // True while output configuration is active.

    volatile bool physicalPressActive_;       // Latest raw press state from GPIO ISR.
    volatile bool outputActive_;              // Last logical output state written.
    volatile bool patternActive_;             // Result-pattern state-machine enable.
    volatile uint8_t flashesRemaining_;       // ON phases not yet started/completed.
    volatile uint16_t phaseTicksRemaining_;   // Ticks left in current ON/OFF phase.
    volatile uint16_t releaseQuietTicksRemaining_; // Ticks before result may begin.
    volatile bool modeIndicatorActive_;      // Continuous lower-priority indication.
    volatile uint16_t indicatorOnTicks_;     // Ticks in indicator illuminated phase.
    volatile uint16_t indicatorOffTicks_;    // Ticks in indicator dark phase.
    volatile uint16_t indicatorTicksRemaining_; // Ticks left in current mode phase.

    Diagnostics diagnostics_;          // Accumulated behavior and timing counters.
};

} // namespace esp_apploader
