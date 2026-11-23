/*
 * EspAppLoader
 * File: WTgInteruptBtn.cpp
 * Purpose: Captures and processes button and reed-switch input transitions.
 */

/*
 * WaterThing Interrupt-Driven Button Implementation
 *
 * Designed by Darryl Cornish.
 * Written by Darryl Cornish with assistance from various AI systems.
 *
 * Copyright (c) 2026 Darryl Cornish
 * SPDX-License-Identifier: MPL-2.0
 *
 * See LICENSE in the repository root.
 */

#include "WTgInteruptBtn.h"

#include <cstring>

namespace esp_apploader {

WTgInteruptBtn::WTgInteruptBtn()
    : writeSequence_(0),
      readSequence_(0),
      eventStartLatched_(false),
      eventStartTimeUs_(0),
      eventStartSequence_(0),
      queueMux_(portMUX_INITIALIZER_UNLOCKED),
      pin_(0),
      activeLevel_(LOW),
      currentLevel_(HIGH),
      begun_(false),
      pressPeriodUs_(0),
      releaseIgnoreUs_(DefaultReleaseIgnoreUs),
      maxValidationTransitions_(0),
      handler_(nullptr),
      handlerContext_(nullptr),
      isrLevelHandler_(nullptr),
      isrLevelContext_(nullptr),
      state_(State::Armed),
      candidateStartUs_(0),
      validationDeadlineUs_(0),
      candidateTransitions_(0),
      validPressAwaitingRelease_(false),
      pendingReleaseTimeUs_(0),
      releaseIgnoreDeadlineUs_(0),
      lastReleaseTailTransitionUs_(0),
      releaseTailTransitionCount_(0),
      releaseTailOpen_(false),
      releaseTailExceededRecorded_(false),
      lastServiceUs_(0) {
    std::memset(queue_, 0, sizeof(queue_));
    std::memset(&diagnostics_, 0, sizeof(diagnostics_));
}

WTgInteruptBtn::~WTgInteruptBtn() {
    end();
}

bool WTgInteruptBtn::begin(uint8_t pin,
                           ActiveLevel activeLevel,
                           uint32_t pressPeriodMs,
                           uint16_t maxValidationTransitions,
                           PressHandler handler,
                           void* context,
                           uint8_t pinModeValue,
                           uint32_t releaseIgnoreUs,
                           IsrLevelHandler isrLevelHandler,
                           void* isrLevelContext) {
    if (begun_ || handler == nullptr || pressPeriodMs == 0U ||
        releaseIgnoreUs == 0U) {
        return false;
    }

    pin_ = pin;
    activeLevel_ = static_cast<uint8_t>(activeLevel);
    pressPeriodUs_ = pressPeriodMs * 1000UL;
    releaseIgnoreUs_ = releaseIgnoreUs;
    maxValidationTransitions_ = maxValidationTransitions;
    handler_ = handler;
    handlerContext_ = context;
    isrLevelHandler_ = isrLevelHandler;
    isrLevelContext_ = isrLevelContext;

    pinMode(pin_, pinModeValue);
    currentLevel_ = static_cast<uint8_t>(digitalRead(pin_));

    portENTER_CRITICAL(&queueMux_);
    writeSequence_ = 0;
    readSequence_ = 0;
    eventStartLatched_ = false;
    eventStartTimeUs_ = 0;
    eventStartSequence_ = 0;
    portEXIT_CRITICAL(&queueMux_);

    std::memset(&diagnostics_, 0, sizeof(diagnostics_));
    state_ = State::Armed;
    candidateTransitions_ = 0;
    validPressAwaitingRelease_ = false;
    pendingReleaseTimeUs_ = 0;
    releaseIgnoreDeadlineUs_ = 0;
    lastReleaseTailTransitionUs_ = 0;
    releaseTailTransitionCount_ = 0;
    releaseTailOpen_ = false;
    releaseTailExceededRecorded_ = false;
    lastServiceUs_ = micros();

    attachInterruptArg(digitalPinToInterrupt(pin_), isrThunk, this, CHANGE);
    begun_ = true;
    return true;
}

void WTgInteruptBtn::end() {
    if (!begun_) {
        return;
    }
    detachInterrupt(digitalPinToInterrupt(pin_));
    begun_ = false;
}

void WTgInteruptBtn::reset() {
    if (!begun_) {
        return;
    }

    portENTER_CRITICAL(&queueMux_);
    readSequence_ = writeSequence_;
    eventStartLatched_ = false;
    portEXIT_CRITICAL(&queueMux_);

    currentLevel_ = static_cast<uint8_t>(digitalRead(pin_));
    state_ = State::Armed;
    candidateTransitions_ = 0;
    validPressAwaitingRelease_ = false;
    pendingReleaseTimeUs_ = 0;
    releaseIgnoreDeadlineUs_ = 0;
    lastReleaseTailTransitionUs_ = 0;
    releaseTailTransitionCount_ = 0;
    releaseTailOpen_ = false;
    releaseTailExceededRecorded_ = false;
    lastServiceUs_ = micros();
}

bool WTgInteruptBtn::setReleaseIgnoreUs(uint32_t releaseIgnoreUs) {
    if (releaseIgnoreUs == 0U || state_ == State::ReleaseIgnore) {
        return false;
    }
    releaseIgnoreUs_ = releaseIgnoreUs;
    return true;
}

uint32_t WTgInteruptBtn::releaseIgnoreUs() const {
    return releaseIgnoreUs_;
}

void WTgInteruptBtn::service() {
    if (!begun_) {
        return;
    }

    const uint32_t serviceNow = micros();
    const uint32_t serviceGap = serviceNow - lastServiceUs_;
    lastServiceUs_ = serviceNow;
    if (serviceGap > diagnostics_.maxServiceGapUs) {
        diagnostics_.maxServiceGapUs = serviceGap;
    }

    Transition transition{};
    bool historyWasOverwritten = false;
    while (popTransition(transition, historyWasOverwritten)) {
        if (historyWasOverwritten) {
            resynchronizeBefore(transition);
        }
        advanceStateTo(transition.timeUs);
        processTransition(transition);
    }
    advanceStateTo(micros());
}

bool WTgInteruptBtn::isActive() const {
    return levelIsActive(currentLevel_);
}

bool WTgInteruptBtn::isBegun() const {
    return begun_;
}

const WTgInteruptBtn::Diagnostics& WTgInteruptBtn::diagnostics() const {
    return diagnostics_;
}

void ARDUINO_ISR_ATTR WTgInteruptBtn::isrThunk(void* argument) {
    static_cast<WTgInteruptBtn*>(argument)->captureTransition();
}

void ARDUINO_ISR_ATTR WTgInteruptBtn::captureTransition() {
    const Transition transition = {
        micros(),
        static_cast<uint8_t>(digitalRead(pin_))
    };

    portENTER_CRITICAL_ISR(&queueMux_);
    const uint32_t sequence = writeSequence_;

    // Capture only. Validation and behavior never run in interrupt context.
    if (!eventStartLatched_ && levelIsActive(transition.level)) {
        eventStartLatched_ = true;
        eventStartTimeUs_ = transition.timeUs;
        eventStartSequence_ = sequence;
    }

    queue_[sequence & QueueMask] = transition;
    writeSequence_ = sequence + 1U;
    portEXIT_CRITICAL_ISR(&queueMux_);

    // Optional immediate observation only. The callback receives the captured
    // physical level but does not participate in press validation.
    if (isrLevelHandler_ != nullptr) {
        isrLevelHandler_(levelIsActive(transition.level), isrLevelContext_);
    }
}

bool WTgInteruptBtn::popTransition(Transition& transition,
                                   bool& historyWasOverwritten) {
    historyWasOverwritten = false;
    portENTER_CRITICAL(&queueMux_);

    const uint32_t writeSnapshot = writeSequence_;
    uint32_t available = writeSnapshot - readSequence_;
    if (available == 0U) {
        portEXIT_CRITICAL(&queueMux_);
        return false;
    }

    if (available > QueueSize) {
        const uint32_t overwritten = available - QueueSize;
        diagnostics_.overwrittenTransitions += overwritten;
        ++diagnostics_.overwriteEvents;
        readSequence_ = writeSnapshot - QueueSize;
        available = QueueSize;
        historyWasOverwritten = true;
    }

    if (available > diagnostics_.maxQueueDepth) {
        diagnostics_.maxQueueDepth = available;
    }

    transition = queue_[readSequence_ & QueueMask];
    ++readSequence_;
    portEXIT_CRITICAL(&queueMux_);
    return true;
}

void WTgInteruptBtn::resynchronizeBefore(const Transition& transition) {
    currentLevel_ = transition.level == LOW ? HIGH : LOW;
    candidateTransitions_ = 0;
    if (eventStartLatched_) {
        startCandidateFromLatchedEvent();
    } else {
        state_ = State::Armed;
    }
}

void WTgInteruptBtn::startCandidateFromLatchedEvent() {
    uint32_t startTimeUs;
    portENTER_CRITICAL(&queueMux_);
    startTimeUs = eventStartTimeUs_;
    portEXIT_CRITICAL(&queueMux_);

    candidateStartUs_ = startTimeUs;
    validationDeadlineUs_ = candidateStartUs_ + pressPeriodUs_;
    state_ = State::Validating;
}

void WTgInteruptBtn::releaseStartLatch() {
    portENTER_CRITICAL(&queueMux_);
    eventStartLatched_ = false;
    portEXIT_CRITICAL(&queueMux_);
}

void WTgInteruptBtn::advanceStateTo(uint32_t timeUs) {
    if (state_ == State::ReleaseIgnore &&
        timeReached(timeUs, releaseIgnoreDeadlineUs_)) {
        if (!levelIsActive(currentLevel_)) {
            finishRelease();
        } else {
            // The signal was still active when the ignore period expired.
            // Continue treating subsequent edges as the same release tail and
            // wait for another inactive edge.
            if (!releaseTailExceededRecorded_) {
                ++diagnostics_.releaseTailsOverIgnore;
                releaseTailExceededRecorded_ = true;
            }
            state_ = State::WaitRelease;
        }
        return;
    }

    if (state_ != State::Validating ||
        !timeReached(timeUs, validationDeadlineUs_)) {
        return;
    }

    if (levelIsActive(currentLevel_) &&
        candidateTransitions_ <= maxValidationTransitions_) {
        completeValidPress();
    } else {
        completeRejectedPress();
    }
}

void WTgInteruptBtn::processTransition(const Transition& transition) {
    currentLevel_ = transition.level;

    switch (state_) {
        case State::Armed:
            if (levelIsActive(currentLevel_)) {
                if (eventStartLatched_) {
                    startCandidateFromLatchedEvent();
                } else {
                    candidateStartUs_ = transition.timeUs;
                    validationDeadlineUs_ =
                        candidateStartUs_ + pressPeriodUs_;
                    state_ = State::Validating;
                }
                candidateTransitions_ = 0;
            }
            break;

        case State::Validating:
            if (candidateTransitions_ != UINT16_MAX) {
                ++candidateTransitions_;
            }
            break;

        case State::WaitRelease:
            if (!levelIsActive(currentLevel_)) {
                if (validPressAwaitingRelease_) {
                    startReleaseIgnore(transition.timeUs);
                } else {
                    state_ = State::Armed;
                    releaseStartLatch();
                }
            }
            break;

        case State::ReleaseIgnore:
            if (releaseTailTransitionCount_ != UINT16_MAX) {
                ++releaseTailTransitionCount_;
            }
            lastReleaseTailTransitionUs_ = transition.timeUs;
            if (!levelIsActive(currentLevel_)) {
                // Require a complete quiet interval after the most recent
                // inactive edge while retaining the first release timestamp.
                releaseIgnoreDeadlineUs_ =
                    transition.timeUs + releaseIgnoreUs_;
            }
            break;
    }
}

void WTgInteruptBtn::startReleaseIgnore(uint32_t releaseTimeUs) {
    if (!releaseTailOpen_) {
        pendingReleaseTimeUs_ = releaseTimeUs;
        lastReleaseTailTransitionUs_ = releaseTimeUs;
        releaseTailTransitionCount_ = 0;
        releaseTailExceededRecorded_ = false;
        releaseTailOpen_ = true;
    } else {
        if (releaseTailTransitionCount_ != UINT16_MAX) {
            ++releaseTailTransitionCount_;
        }
        lastReleaseTailTransitionUs_ = releaseTimeUs;
    }

    releaseIgnoreDeadlineUs_ = releaseTimeUs + releaseIgnoreUs_;
    state_ = State::ReleaseIgnore;
}

void WTgInteruptBtn::finishRelease() {
    recordReleaseTail();
    deliverReleasedPress(pendingReleaseTimeUs_);
    validPressAwaitingRelease_ = false;
    releaseTailOpen_ = false;
    state_ = State::Armed;
    releaseStartLatch();
}

void WTgInteruptBtn::recordReleaseTail() {
    const uint32_t tailUs =
        lastReleaseTailTransitionUs_ - pendingReleaseTimeUs_;

    ++diagnostics_.releaseEvents;
    diagnostics_.releaseTailTransitions += releaseTailTransitionCount_;
    if (releaseTailTransitionCount_ >
        diagnostics_.maxReleaseTailTransitions) {
        diagnostics_.maxReleaseTailTransitions =
            releaseTailTransitionCount_;
    }
    if (tailUs > diagnostics_.maxReleaseTailUs) {
        diagnostics_.maxReleaseTailUs = tailUs;
    }

    if (releaseTailTransitionCount_ == 0U) {
        ++diagnostics_.cleanReleases;
    } else {
        ++diagnostics_.bouncedReleases;
        if (tailUs <= 50U) {
            ++diagnostics_.releaseTail1To50Us;
        } else if (tailUs <= 100U) {
            ++diagnostics_.releaseTail51To100Us;
        } else if (tailUs <= 250U) {
            ++diagnostics_.releaseTail101To250Us;
        } else if (tailUs <= 500U) {
            ++diagnostics_.releaseTail251To500Us;
        } else if (tailUs <= 1000U) {
            ++diagnostics_.releaseTail501To1000Us;
        } else {
            ++diagnostics_.releaseTailOver1000Us;
        }
    }
}

void WTgInteruptBtn::completeValidPress() {
    diagnostics_.lastPressTransitions = candidateTransitions_;
    if (candidateTransitions_ > diagnostics_.maxPressTransitions) {
        diagnostics_.maxPressTransitions = candidateTransitions_;
    }
    ++diagnostics_.validPresses;
    state_ = State::WaitRelease;
    validPressAwaitingRelease_ = true;
}

void WTgInteruptBtn::completeRejectedPress() {
    diagnostics_.lastPressTransitions = candidateTransitions_;
    if (candidateTransitions_ > diagnostics_.maxPressTransitions) {
        diagnostics_.maxPressTransitions = candidateTransitions_;
    }
    ++diagnostics_.rejectedPresses;
    validPressAwaitingRelease_ = false;

    state_ = levelIsActive(currentLevel_)
                 ? State::WaitRelease
                 : State::Armed;
    if (state_ == State::Armed) {
        releaseStartLatch();
    }
}

void WTgInteruptBtn::deliverReleasedPress(uint32_t releaseTimeUs) {
    uint32_t startTimeUs;

    portENTER_CRITICAL(&queueMux_);
    startTimeUs = eventStartTimeUs_;
    portEXIT_CRITICAL(&queueMux_);

    const uint32_t durationUs = releaseTimeUs - startTimeUs;
    const PressEvent event = {
        startTimeUs,
        releaseTimeUs,
        durationUs,
        durationUs / 1000UL,
        candidateTransitions_
    };

    // Duration calculation and application behavior occur only in service().
    handler_(event, handlerContext_);
}

bool WTgInteruptBtn::timeReached(uint32_t now, uint32_t deadline) {
    return static_cast<int32_t>(now - deadline) >= 0;
}

bool ARDUINO_ISR_ATTR WTgInteruptBtn::levelIsActive(uint8_t level) const {
    return level == activeLevel_;
}

} // namespace esp_apploader
