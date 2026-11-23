/*
 * EspAppLoader
 * File: WTgUiFeedbackMgr.cpp
 * Purpose: Drives timed visual feedback on the board output.
 */

/*
 * WaterThing User-Interface Feedback Manager Implementation
 *
 * Designed by Darryl Cornish.
 * Written by Darryl Cornish with assistance from various AI systems.
 *
 * Copyright (c) 2026 Darryl Cornish
 * SPDX-License-Identifier: MPL-2.0
 *
 * See LICENSE in the repository root.
 */

#include "WTgUiFeedbackMgr.h"

#include <cstring>

namespace esp_apploader {

WTgUiFeedbackMgr::WTgUiFeedbackMgr()
    : mux_(portMUX_INITIALIZER_UNLOCKED),
      outputPin_(0),
      activeLevel_(HIGH),
      timerTickUs_(0),
      onTicks_(0),
      offTicks_(0),
      postReleaseQuietTicks_(0),
      begun_(false),
      physicalPressActive_(false),
      outputActive_(false),
      patternActive_(false),
      flashesRemaining_(0),
      phaseTicksRemaining_(0),
      releaseQuietTicksRemaining_(0),
      modeIndicatorActive_(false),
      indicatorOnTicks_(0),
      indicatorOffTicks_(0),
      indicatorTicksRemaining_(0) {
    std::memset(&diagnostics_, 0, sizeof(diagnostics_));
}

WTgUiFeedbackMgr::~WTgUiFeedbackMgr() {
    end();
}

bool WTgUiFeedbackMgr::begin(uint8_t outputPin,
                             ActiveLevel activeLevel,
                             uint32_t timerTickUs,
                             uint32_t flashOnUs,
                             uint32_t flashOffUs,
                             uint32_t postReleaseQuietUs) {
    if (begun_ || timerTickUs == 0U || flashOnUs == 0U ||
        flashOffUs == 0U || postReleaseQuietUs == 0U ||
        (flashOnUs % timerTickUs) != 0U ||
        (flashOffUs % timerTickUs) != 0U ||
        (postReleaseQuietUs % timerTickUs) != 0U) {
        return false;
    }

    const uint32_t calculatedOnTicks = flashOnUs / timerTickUs;
    const uint32_t calculatedOffTicks = flashOffUs / timerTickUs;
    const uint32_t calculatedQuietTicks =
        postReleaseQuietUs / timerTickUs;
    if (calculatedOnTicks == 0U || calculatedOnTicks > UINT16_MAX ||
        calculatedOffTicks == 0U || calculatedOffTicks > UINT16_MAX ||
        calculatedQuietTicks == 0U ||
        calculatedQuietTicks > UINT16_MAX) {
        return false;
    }

    outputPin_ = outputPin;
    activeLevel_ = static_cast<uint8_t>(activeLevel);
    timerTickUs_ = timerTickUs;
    onTicks_ = static_cast<uint16_t>(calculatedOnTicks);
    offTicks_ = static_cast<uint16_t>(calculatedOffTicks);
    postReleaseQuietTicks_ =
        static_cast<uint16_t>(calculatedQuietTicks);

    pinMode(outputPin_, OUTPUT);
    digitalWrite(outputPin_, outputLevel(false));

    portENTER_CRITICAL(&mux_);
    physicalPressActive_ = false;
    outputActive_ = false;
    patternActive_ = false;
    flashesRemaining_ = 0;
    phaseTicksRemaining_ = 0;
    releaseQuietTicksRemaining_ = 0;
    modeIndicatorActive_ = false;
    indicatorOnTicks_ = 0;
    indicatorOffTicks_ = 0;
    indicatorTicksRemaining_ = 0;
    std::memset(&diagnostics_, 0, sizeof(diagnostics_));
    begun_ = true;
    portEXIT_CRITICAL(&mux_);
    return true;
}

void WTgUiFeedbackMgr::end() {
    if (!begun_) {
        return;
    }

    portENTER_CRITICAL(&mux_);
    physicalPressActive_ = false;
    outputActive_ = false;
    patternActive_ = false;
    flashesRemaining_ = 0;
    phaseTicksRemaining_ = 0;
    releaseQuietTicksRemaining_ = 0;
    modeIndicatorActive_ = false;
    timerTickUs_ = 0;
    indicatorOnTicks_ = 0;
    indicatorOffTicks_ = 0;
    indicatorTicksRemaining_ = 0;
    begun_ = false;
    digitalWrite(outputPin_, outputLevel(false));
    portEXIT_CRITICAL(&mux_);
}

void ARDUINO_ISR_ATTR
WTgUiFeedbackMgr::setPhysicalPressLevelIsr(bool active) {
    portENTER_CRITICAL_ISR(&mux_);
    if (begun_) {
        ++diagnostics_.physicalLevelCallbacks;

        if (active) {
            if (patternActive_) {
                ++diagnostics_.patternsCancelledByPress;
            }
            patternActive_ = false;
            flashesRemaining_ = 0;
            phaseTicksRemaining_ = 0;
            releaseQuietTicksRemaining_ = 0;
        } else {
            // Start a deterministic dark interval at the actual release edge.
            // Timer ticks consume this interval even if the application has
            // not yet reported the result.
            releaseQuietTicksRemaining_ = postReleaseQuietTicks_;
        }

        physicalPressActive_ = active;
        writeOutput(active);
    }
    portEXIT_CRITICAL_ISR(&mux_);
}

bool WTgUiFeedbackMgr::signal(Result result) {
    if (!begun_) {
        return false;
    }

    const uint8_t requestedFlashes = flashesForResult(result);
    if (requestedFlashes == 0U) {
        return false;
    }

    portENTER_CRITICAL(&mux_);
    switch (result) {
        case Result::ShortSuccess:
            ++diagnostics_.shortSuccessSignals;
            break;
        case Result::MediumSuccess:
            ++diagnostics_.mediumSuccessSignals;
            break;
        case Result::LongSuccess:
            ++diagnostics_.longSuccessSignals;
            break;
        case Result::Failure:
            ++diagnostics_.failureSignals;
            break;
    }

    patternActive_ = true;
    flashesRemaining_ = requestedFlashes;
    phaseTicksRemaining_ = 1;
    if (!physicalPressActive_) {
        writeOutput(false);
    }
    portEXIT_CRITICAL(&mux_);
    return true;
}

bool WTgUiFeedbackMgr::setModeIndicator(bool enabled,
                                        uint32_t indicatorOnUs,
                                        uint32_t indicatorOffUs) {
    if (!begun_) {
        return false;
    }

    uint32_t calculatedOnTicks = 0U;
    uint32_t calculatedOffTicks = 0U;
    if (enabled) {
        if (timerTickUs_ == 0U || indicatorOnUs == 0U || indicatorOffUs == 0U ||
            (indicatorOnUs % timerTickUs_) != 0U ||
            (indicatorOffUs % timerTickUs_) != 0U) {
            return false;
        }
        calculatedOnTicks = indicatorOnUs / timerTickUs_;
        calculatedOffTicks = indicatorOffUs / timerTickUs_;
        if (calculatedOnTicks == 0U || calculatedOnTicks > UINT16_MAX ||
            calculatedOffTicks == 0U || calculatedOffTicks > UINT16_MAX) {
            return false;
        }
    }

    portENTER_CRITICAL(&mux_);
    modeIndicatorActive_ = enabled;
    indicatorOnTicks_ = static_cast<uint16_t>(calculatedOnTicks);
    indicatorOffTicks_ = static_cast<uint16_t>(calculatedOffTicks);
    indicatorTicksRemaining_ = enabled ? 1U : 0U;
    if (!physicalPressActive_ && !patternActive_) {
        writeOutput(false);
    }
    portEXIT_CRITICAL(&mux_);
    return true;
}

void ARDUINO_ISR_ATTR WTgUiFeedbackMgr::onTimerTickIsr() {
    portENTER_CRITICAL_ISR(&mux_);
    if (!begun_) {
        portEXIT_CRITICAL_ISR(&mux_);
        return;
    }

    ++diagnostics_.timerTicks;

    if (physicalPressActive_) {
        portEXIT_CRITICAL_ISR(&mux_);
        return;
    }

    // Maintain the minimum dark interval from the physical release edge.
    // This countdown runs independently of signal(), so application processing
    // latency cannot accidentally shorten the visual separation.
    if (releaseQuietTicksRemaining_ > 0U) {
        --releaseQuietTicksRemaining_;
        portEXIT_CRITICAL_ISR(&mux_);
        return;
    }

    if (patternActive_) {
        if (phaseTicksRemaining_ > 1U) {
            --phaseTicksRemaining_;
        } else {
            if (!outputActive_) {
                writeOutput(true);
                phaseTicksRemaining_ = onTicks_;
            } else {
                writeOutput(false);
                --flashesRemaining_;

                if (flashesRemaining_ == 0U) {
                    patternActive_ = false;
                    phaseTicksRemaining_ = 0;
                    indicatorTicksRemaining_ = modeIndicatorActive_ ? 1U : 0U;
                    ++diagnostics_.patternsCompleted;
                } else {
                    phaseTicksRemaining_ = offTicks_;
                }
            }
        }
        portEXIT_CRITICAL_ISR(&mux_);
        return;
    }

    if (modeIndicatorActive_) {
        if (indicatorTicksRemaining_ > 1U) {
            --indicatorTicksRemaining_;
        } else if (!outputActive_) {
            writeOutput(true);
            indicatorTicksRemaining_ = indicatorOnTicks_;
        } else {
            writeOutput(false);
            indicatorTicksRemaining_ = indicatorOffTicks_;
        }
    }

    portEXIT_CRITICAL_ISR(&mux_);
}

bool WTgUiFeedbackMgr::isBegun() const {
    return begun_;
}

bool WTgUiFeedbackMgr::isPatternActive() const {
    bool active;
    portENTER_CRITICAL(&mux_);
    active = patternActive_;
    portEXIT_CRITICAL(&mux_);
    return active;
}

void WTgUiFeedbackMgr::getDiagnostics(Diagnostics& output) const {
    portENTER_CRITICAL(&mux_);
    output = diagnostics_;
    portEXIT_CRITICAL(&mux_);
}

uint8_t WTgUiFeedbackMgr::flashesForResult(Result result) {
    switch (result) {
        case Result::ShortSuccess:
            return 1;
        case Result::MediumSuccess:
            return 2;
        case Result::LongSuccess:
            return 3;
        case Result::Failure:
            return 10;
    }
    return 0;
}

uint8_t ARDUINO_ISR_ATTR
WTgUiFeedbackMgr::outputLevel(bool active) const {
    if (active) {
        return activeLevel_;
    }
    return activeLevel_ == HIGH ? LOW : HIGH;
}

void ARDUINO_ISR_ATTR WTgUiFeedbackMgr::writeOutput(bool active) {
    outputActive_ = active;
    digitalWrite(outputPin_, outputLevel(active));
}

} // namespace esp_apploader
