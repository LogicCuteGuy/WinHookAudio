#pragma once

// WHADriftControl — PI loop from a FIFO's fill to a resample ratio, for a source on its own clock
// feeding a consumer on the Master Clock. Vocabulary: Master Clock, Worker.
//
// Engaging needs the smoothed error beyond the deadband for engageSec without a break, so a
// transient (a stalled tick, a late packet burst) is not mistaken for drift.
// Fill is smoothed by two cascaded EMAs: packets arrive a device period at a time, so the fill at a
// read is a sawtooth, and one stage leaves enough of it to frequency-modulate the ratio audibly
// (sidebands ~-45 dB at 512-frame packets); two stages square the attenuation. Correction engages only once the smoothed fill leaves the deadband
// around the setpoint, so equal clocks never resample (the resampler stays a bit-exact copy).
// Once engaged it stays engaged until reset(). The plant is an integrator (fill' = drift - rate *
// correction), so with kp = 2, ki = 1 the closed loop is critically damped with a ~1 s time scale.

#include <algorithm>
#include <cmath>

namespace wha {

struct WHADriftParams {
  double fillTauSec = 0.125;  // fill smoothing, per stage (two stages)
  double kp = 2.0;           // x 1/rate per frame of error
  double ki = 1.0;           // x 1/rate per frame*second
  double maxPpm = 10000.0;   // correction clamp
  double deadband = 96.0;    // frames around the setpoint before engaging
  double engageSec = 0.25;   // ... held that long
};

class WHADriftControl {
 public:
  void reset(double setpoint, double rate, const WHADriftParams& params) {
    setpoint_ = setpoint;
    rate_ = rate;
    p_ = params;
    ema_ = ema2_ = setpoint;
    integ_ = 0.0;
    ratio_ = 1.0;
    ratioAvg_ = 1.0;
    engaged_ = false;
    outsideSec_ = 0.0;
  }

  // New setpoint/deadband (e.g. a larger jitter target) keeping the drift estimate and engagement.
  void retarget(double setpoint, double deadband) {
    setpoint_ = setpoint;
    p_.deadband = deadband;
    ema_ = ema2_ = setpoint;  // re-priming restarts the fill at the new target
  }

  // New loop gains keeping the correction it has built up (the integral's share of it).
  void setGains(double kp, double ki) {
    if (ki > 0.0 && p_.ki > 0.0) integ_ *= p_.ki / ki;
    p_.kp = kp;
    p_.ki = ki;
  }

  // One observation of the fill `dt` seconds after the previous one. Returns the ratio (output
  // frames per input frame) for the next input.
  double update(double fill, double dt) {
    const double a = dt < p_.fillTauSec ? dt / p_.fillTauSec : 1.0;
    ema_ += (fill - ema_) * a;
    ema2_ += (ema_ - ema2_) * a;
    const double err = ema2_ - setpoint_;
    if (!engaged_) {
      outsideSec_ = std::fabs(err) < p_.deadband ? 0.0 : outsideSec_ + dt;
      if (outsideSec_ < p_.engageSec) return ratio_;
      engaged_ = true;
    }
    const double maxCorr = p_.maxPpm * 1e-6;
    double corr = -(p_.kp * err + p_.ki * (integ_ + err * dt)) / rate_;
    if (corr > -maxCorr && corr < maxCorr) integ_ += err * dt;  // anti-windup: freeze while clamped
    corr = std::clamp(corr, -maxCorr, maxCorr);
    ratio_ = 1.0 + corr;
    ratioAvg_ += (ratio_ - ratioAvg_) * (dt < kEstimateTauSec ? dt / kEstimateTauSec : 1.0);
    return ratio_;
  }

  double ratio() const { return ratio_; }
  bool engaged() const { return engaged_; }
  double smoothedFill() const { return ema2_; }
  // Source clock relative to the consumer, ppm: positive = source runs fast (we drop frames).
  // From the ratio averaged over kEstimateTauSec: the instantaneous ratio also carries the
  // proportional reaction to fill ripple.
  double sourcePpm() const { return (1.0 - ratioAvg_) * 1e6; }

 private:
  WHADriftParams p_;
  double setpoint_ = 0.0;
  double rate_ = 48000.0;
  double ema_ = 0.0;
  double ema2_ = 0.0;
  double integ_ = 0.0;
  double ratio_ = 1.0;
  double ratioAvg_ = 1.0;
  bool engaged_ = false;
  double outsideSec_ = 0.0;
  static constexpr double kEstimateTauSec = 5.0;
};

}  // namespace wha
