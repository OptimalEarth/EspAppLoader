/*
 * EspAppLoader
 * File: WTgInteruptBtn.h
 * Purpose: Declares the interrupt-driven button and reed-switch input manager.
 */

/*
 * WaterThing Interrupt-Driven Button Interface
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
 * WTgInteruptBtn button-event state machine
 * =========================================
 *
 * The class separates time-critical observation from deferred interpretation.
 *
 * ISR responsibilities
 * --------------------
 * A CHANGE interrupt records each transition's timestamp and logic level in a
 * fixed-size circular buffer. The first transition to the active level also
 * latches the event's original start timestamp in separate storage. The ISR
 * performs no press validation, diagnostics, or normal application behavior.
 * If configured, it invokes one explicitly ISR-safe level observer after the
 * transition record has been committed.
 *
 * Circular-buffer behavior
 * ------------------------
 * The queue retains the most recent QueueSize transitions. If more transitions
 * occur before service() can consume them, new records overwrite the oldest
 * records. The separately latched event-start timestamp is not in the circular
 * buffer and is therefore not overwritten.
 *
 * A physical button push may contain more transitions than QueueSize. This is
 * acceptable: early noisy transitions roll out of the queue while the original
 * start time remains available. At the end of the push, the transitions still
 * retained in the queue are the ValidationTransitions used to judge whether
 * the signal settled sufficiently. MaxValidationTransitions limits these
 * retained validation transitions; it does not limit the total number of
 * physical transitions that may have occurred during the complete push.
 *
 * Deferred service
 * ----------------
 * service() runs in normal program context. For every retained transition, it
 * advances logical time to the transition timestamp before applying the new
 * level. This ordering allows a press that crossed its validation deadline
 * while loop() was blocked to be validated before a later release is applied.
 * After consuming the queue, service() advances the state machine to the
 * current time so a button that remains held can also complete validation.
 *
 * Optional immediate level observer
 * ---------------------------------
 * begin() may receive an IsrLevelHandler. It is invoked directly from the GPIO
 * ISR after every captured transition and receives the reconstructed active
 * state. This hook is intended only for short, bounded physical feedback such
 * as mirroring the button level to an LED or oscilloscope output. It does not
 * validate the press and cannot invoke the normal PressHandler.
 *
 * States
 * ------
 * Armed:
 *   The class is waiting for an active transition. The first active edge starts
 *   a candidate using the immutable event-start timestamp.
 *
 * Validating:
 *   The candidate must remain active until:
 *
 *       eventStart + pressPeriod
 *
 *   At that deadline, the candidate is valid when the retained validation
 *   history is within MaxValidationTransitions and the reconstructed current
 *   level is active. A valid press increments diagnostics and invokes the
 *   candidate is marked valid. Its handler is deferred until release so the
 *   complete press duration can be reported. A failed candidate is recorded
 *   as rejected.
 *
 * WaitRelease:
 *   After a valid press, the class waits for the first inactive transition.
 *   That ISR-captured transition is retained as the press release timestamp.
 *
 * ReleaseIgnore:
 *   Transitions following the first release are observed for an adjustable
 *   release-ignore interval, but cannot start another press or change the
 *   retained press start/release timestamps. When the input remains inactive
 *   through the interval, service() delivers the completed press and records
 *   release-tail statistics.
 *
 * Delayed service can delay notification, but it does not move the captured
 * start time. Queue overwrite is explicitly diagnosed so field measurements
 * can be used to confirm that QueueSize is appropriate.
 */
class WTgInteruptBtn {
public:
    enum class ActiveLevel : uint8_t {
        Low = LOW,
        High = HIGH
    };

    /** Immutable description delivered after a validated press is released. */
    struct PressEvent {
        uint32_t startTimeUs;              // ISR-captured first active edge.
        uint32_t releaseTimeUs;            // ISR-captured first inactive edge.
        uint32_t durationUs;               // Wrap-safe release-minus-start duration.
        uint32_t durationMs;               // Truncated millisecond duration convenience.
        uint16_t validationTransitions;    // Retained transitions used for validation.
    };

    using PressHandler = void (*)(const PressEvent& event, void* context);

    /*
     * Optional raw-level observer invoked directly from the GPIO ISR.
     *
     * active is true when the newly captured input level equals ActiveLevel.
     * The handler must obey normal ISR restrictions: short, bounded, no
     * logging, no delay, no allocation, and no blocking operations.
     */
    using IsrLevelHandler = void (*)(bool active, void* context);

    /** Lifetime counters for queue sizing, validation, and release-bounce analysis. */
    struct Diagnostics {
        uint32_t validPresses;
        uint32_t rejectedPresses;
        uint32_t overwrittenTransitions;
        uint32_t overwriteEvents;
        uint32_t maxQueueDepth;
        uint32_t maxServiceGapUs;
        uint16_t lastPressTransitions;
        uint16_t maxPressTransitions;
        uint32_t releaseEvents;
        uint32_t cleanReleases;
        uint32_t bouncedReleases;
        uint32_t releaseTailTransitions;
        uint16_t maxReleaseTailTransitions;
        uint32_t maxReleaseTailUs;
        uint32_t releaseTailsOverIgnore;
        uint32_t releaseTail1To50Us;
        uint32_t releaseTail51To100Us;
        uint32_t releaseTail101To250Us;
        uint32_t releaseTail251To500Us;
        uint32_t releaseTail501To1000Us;
        uint32_t releaseTailOver1000Us;
    };

    static constexpr uint32_t QueueSize = 32;
    static constexpr uint32_t DefaultReleaseIgnoreUs = 1000;

    WTgInteruptBtn();
    ~WTgInteruptBtn();

    WTgInteruptBtn(const WTgInteruptBtn&) = delete;
    WTgInteruptBtn& operator=(const WTgInteruptBtn&) = delete;

    bool begin(uint8_t pin,
               ActiveLevel activeLevel,
               uint32_t pressPeriodMs,
               uint16_t maxValidationTransitions,
               PressHandler handler,
               void* context = nullptr,
               uint8_t pinModeValue = INPUT_PULLUP,
               uint32_t releaseIgnoreUs = DefaultReleaseIgnoreUs,
               IsrLevelHandler isrLevelHandler = nullptr,
               void* isrLevelContext = nullptr);

    /** Detach the GPIO interrupt and return the object to its unconfigured state. */
    void end();
    /** Consume captured transitions and advance validation in normal task context. */
    void service();
    /** Discard an in-progress candidate and clear lifetime diagnostics. */
    void reset();
    bool setReleaseIgnoreUs(uint32_t releaseIgnoreUs);
    uint32_t releaseIgnoreUs() const;

    /** Return the reconstructed logical level last processed by service(). */
    bool isActive() const;
    /** Return true while the GPIO interrupt is configured. */
    bool isBegun() const;
    /** Return the accumulated diagnostic counters. */
    const Diagnostics& diagnostics() const;

private:
    static_assert((QueueSize & (QueueSize - 1U)) == 0,
                  "QueueSize must be a power of two");
    static constexpr uint32_t QueueMask = QueueSize - 1U;

    /** Minimal record written by the GPIO ISR into the fixed circular queue. */
    struct Transition {
        uint32_t timeUs; // micros() timestamp captured at the transition.
        uint8_t level;   // Raw digital level observed at that timestamp.
    };

    enum class State : uint8_t {
        Armed,
        Validating,
        WaitRelease,
        ReleaseIgnore
    };

    static void ARDUINO_ISR_ATTR isrThunk(void* argument);
    void ARDUINO_ISR_ATTR captureTransition();

    bool popTransition(Transition& transition, bool& historyWasOverwritten);
    void resynchronizeBefore(const Transition& transition);
    void startCandidateFromLatchedEvent();
    void releaseStartLatch();
    void advanceStateTo(uint32_t timeUs);
    void processTransition(const Transition& transition);
    void completeValidPress();
    void completeRejectedPress();
    void startReleaseIgnore(uint32_t releaseTimeUs);
    void finishRelease();
    void recordReleaseTail();
    void deliverReleasedPress(uint32_t releaseTimeUs);

    static bool timeReached(uint32_t now, uint32_t deadline);
    bool ARDUINO_ISR_ATTR levelIsActive(uint8_t level) const;

    Transition queue_[QueueSize];       // Fixed ISR-to-service transition ring.
    volatile uint32_t writeSequence_;   // Monotonic next-write sequence from ISR.
    uint32_t readSequence_;             // Monotonic next-read sequence for service().
    volatile bool eventStartLatched_;
    volatile uint32_t eventStartTimeUs_;
    volatile uint32_t eventStartSequence_;
    portMUX_TYPE queueMux_;

    uint8_t pin_;                       // GPIO observed for button transitions.
    uint8_t activeLevel_;               // Raw level interpreted as physically active.
    uint8_t currentLevel_;              // Last raw level reconstructed by service().
    bool begun_;                        // True while interrupt capture is configured.

    uint32_t pressPeriodUs_;
    uint32_t releaseIgnoreUs_;
    // Maximum retained queue transitions used to validate a candidate push.
    // This is not the total transition count of the complete physical push.
    uint16_t maxValidationTransitions_;
    PressHandler handler_;              // Normal-context completed-press callback.
    void* handlerContext_;              // Opaque state passed to handler_.
    IsrLevelHandler isrLevelHandler_;   // Optional immediate ISR-level observer.
    void* isrLevelContext_;             // Opaque state for the ISR observer.

    State state_;
    uint32_t candidateStartUs_;
    uint32_t validationDeadlineUs_;
    uint16_t candidateTransitions_;
    bool validPressAwaitingRelease_;
    uint32_t pendingReleaseTimeUs_;
    uint32_t releaseIgnoreDeadlineUs_;
    uint32_t lastReleaseTailTransitionUs_;
    uint16_t releaseTailTransitionCount_;
    bool releaseTailOpen_;
    bool releaseTailExceededRecorded_;
    uint32_t lastServiceUs_;
    Diagnostics diagnostics_;
};

} // namespace esp_apploader
