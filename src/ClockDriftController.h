#pragma once
//==============================================================================
// ClockDriftController.h - Soft-sync controller for one output device.
//
// Each output device has its own hardware clock, which drifts relative to the
// capture device's clock. Left alone, a device slowly accumulates or drains
// buffered audio until it overflows or starves. This controller nudges the
// device's sample rate (via WASAPI's rate-adjust facility) to hold the ring
// buffer near its target fill level.
//
// Deliberately has NO Windows dependency: it is pure arithmetic over
// (current_fill, target_fill) and can be unit-tested on any platform, in
// isolation from WASAPI/COM.
//
// Design note: update() returns everything about the step it just took in
// one value. An earlier version split this into update() (returned only
// whether the rate changed) plus a separate state() getter callers had to
// remember to call afterward — that's temporal coupling: correctness depended
// on calling the two methods in the right order, and nothing enforced it.
// Returning one immutable result per call removes the ordering requirement
// entirely.
//==============================================================================

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace mux {

struct DriftControlParams {
    // Fraction of base rate the controller may deviate by. This is a
    // CORRECTION RANGE, not an expected hardware drift figure: real endpoint
    // clocks are typically off by tens of ppm, while 0.0025 == 2500 ppm. The
    // headroom exists so recovery from a large fill excursion is quick.
    float max_drift_ratio = 0.0025f;

    // Normalised error magnitude tolerated before any correction is applied.
    // With a 50 ms control range, 0.05 == roughly +/-2.5 ms of slack.
    float deadband = 0.05f;

    // EMA smoothing on the fill error. Lower == steadier but slower to react.
    float ema_alpha = 0.05f;

    // Minimum rate change worth applying. Single source of truth: an earlier
    // version gated at 0.5 Hz internally and again at 0.1 Hz in the caller,
    // so the caller's gate could never fire. One threshold now.
    float rate_hysteresis_hz = 0.5f;
};

// Everything update() learned/decided on this call. rate_changed is the only
// field callers strictly need to act on; the rest is diagnostic (telemetry,
// tuning) and safe to ignore.
struct DriftUpdateResult {
    float applied_rate_hz;
    float ema_error;
    float correction;
    bool  clamped;         // true if the target rate hit +/- max_drift_hz this step
    bool  rate_changed;    // true if applied_rate_hz moved past the hysteresis threshold
};

class ClockDriftController {
public:
    ClockDriftController(uint32_t baseSampleRate, const DriftControlParams& params)
        : params_(params),
          base_sample_rate_(static_cast<float>(baseSampleRate)),
          applied_rate_(static_cast<float>(baseSampleRate)) {
        max_drift_hz_ = base_sample_rate_ * params_.max_drift_ratio;
    }

    // Advances the controller by one control-period tick and returns the
    // full outcome. Safe to call at any cadence; the EMA smoothing is what
    // makes the result cadence-appropriate, not the caller.
    DriftUpdateResult update(size_t current_fill, size_t target_fill, size_t control_range_bytes) {
        DriftUpdateResult result{applied_rate_, ema_error_, 0.0f, false, false};
        if (control_range_bytes == 0) return result;

        const float error = static_cast<float>(current_fill) - static_cast<float>(target_fill);
        const float normalized_error = error / static_cast<float>(control_range_bytes);

        ema_error_ = (params_.ema_alpha * normalized_error) +
                     ((1.0f - params_.ema_alpha) * ema_error_);

        const float correction = applyDeadband(ema_error_, params_.deadband);

        float target_rate = base_sample_rate_ + (correction * max_drift_hz_);
        const float lo = base_sample_rate_ - max_drift_hz_;
        const float hi = base_sample_rate_ + max_drift_hz_;
        const bool clamped = (target_rate <= lo || target_rate >= hi);
        target_rate = std::clamp(target_rate, lo, hi);

        const bool changed = std::fabs(target_rate - applied_rate_) >= params_.rate_hysteresis_hz;
        if (changed) applied_rate_ = target_rate;

        result.applied_rate_hz = applied_rate_;
        result.ema_error = ema_error_;
        result.correction = correction;
        result.clamped = clamped;
        result.rate_changed = changed;
        return result;
    }

    float applied_rate() const { return applied_rate_; }
    float base_rate() const { return base_sample_rate_; }
    float max_drift_hz() const { return max_drift_hz_; }

private:
    // Pure function, no member access: maps a raw error past a symmetric
    // dead zone onto a [-1, 1] correction. Pulled out on its own because it's
    // the one piece of this controller worth testing with hand-picked inputs
    // independent of the EMA/rate-hysteresis machinery around it.
    static float applyDeadband(float value, float deadband) {
        float correction = 0.0f;
        if (value > deadband) {
            correction = (value - deadband) / (1.0f - deadband);
        } else if (value < -deadband) {
            correction = (value + deadband) / (1.0f - deadband);
        }
        return std::clamp(correction, -1.0f, 1.0f);
    }

    DriftControlParams params_;
    float base_sample_rate_;
    float applied_rate_;
    float max_drift_hz_ = 0.0f;
    float ema_error_ = 0.0f;
};

}  // namespace mux
