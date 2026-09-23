#pragma once

// WHADriftResampler — variable-ratio windowed-sinc resampler for Network Stream clock drift.
// Vocabulary: Network Stream, Master Clock.
// Runs on the network thread. Always delays by kHalf frames; at ratio 1.0 with an integer read
// position the output is a bit-exact copy, so switching compensation on never jumps the timeline.

#include <cmath>
#include <cstdint>
#include <vector>

namespace wha {

class WHADriftResampler {
 public:
  static constexpr int kTaps = 16;
  static constexpr int kHalf = kTaps / 2;
  static constexpr int kPhases = 256;

  WHADriftResampler() { buildTable(); }

  void reset(uint32_t channels) {
    channels_ = channels;
    hist_.assign(static_cast<size_t>(kTaps) * channels, 0.0f);  // silence primes the filter history
    histFrames_ = kTaps;
    frac_ = 0.0;
  }

  // ratio = output frames per input frame (1.0 = no correction). Appends interleaved output.
  void process(const float* in, uint32_t frames, double ratio, std::vector<float>& out) {
    const uint32_t ch = channels_;
    hist_.insert(hist_.end(), in, in + static_cast<size_t>(frames) * ch);
    histFrames_ += frames;
    const double step = 1.0 / ratio;
    // Read position: integer `pos` (frames into hist_) + fraction frac_ (carried across calls).
    size_t pos = kHalf - 1;
    while (pos + kHalf < histFrames_) {
      const size_t at = out.size();
      out.resize(at + ch);
      float* dst = out.data() + at;
      if (frac_ == 0.0) {
        for (uint32_t c = 0; c < ch; ++c) dst[c] = hist_[pos * ch + c];  // exact identity
      } else {
        const double p = frac_ * kPhases;
        const int ph = static_cast<int>(p);
        const float t = static_cast<float>(p - ph);
        const float* c0 = &table_[static_cast<size_t>(ph) * kTaps];
        const float* c1 = &table_[static_cast<size_t>(ph + 1) * kTaps];
        const size_t first = pos + 1 - kHalf;
        for (uint32_t c = 0; c < ch; ++c) {
          float acc = 0.0f;
          for (int k = 0; k < kTaps; ++k) {
            const float coef = c0[k] + t * (c1[k] - c0[k]);
            acc += coef * hist_[(first + k) * ch + c];
          }
          dst[c] = acc;
        }
      }
      frac_ += step;
      const double whole = std::floor(frac_);
      pos += static_cast<size_t>(whole);
      frac_ -= whole;
    }
    // Drop frames no future output can touch; the next call restarts at pos == kHalf - 1,
    // whose first tap is exactly `keepFrom`. The loop above waits for enough input, so a short
    // remainder is fine.
    const size_t keepFrom = pos + 1 - kHalf;
    hist_.erase(hist_.begin(), hist_.begin() + static_cast<ptrdiff_t>(keepFrom * ch));
    histFrames_ -= keepFrom;
  }

 private:
  void buildTable() {
    // table_[phase][k]: tap k for a read point `phase / kPhases` past sample (kHalf - 1).
    const double pi = 3.14159265358979323846;
    const double beta = 8.0;
    auto bessel0 = [](double x) {
      double sum = 1.0, term = 1.0;
      for (int k = 1; k < 25; ++k) {
        term *= (x / (2.0 * k)) * (x / (2.0 * k));
        sum += term;
      }
      return sum;
    };
    const double i0b = bessel0(beta);
    table_.assign(static_cast<size_t>(kPhases + 1) * kTaps, 0.0f);
    for (int ph = 0; ph <= kPhases; ++ph) {
      const double frac = static_cast<double>(ph) / kPhases;
      double sum = 0.0;
      for (int k = 0; k < kTaps; ++k) {
        const double x = (k - (kHalf - 1)) - frac;  // distance from read point
        const double sinc = x == 0.0 ? 1.0 : std::sin(pi * x) / (pi * x);
        const double r = x / kHalf;
        const double w = std::fabs(r) >= 1.0 ? 0.0 : bessel0(beta * std::sqrt(1.0 - r * r)) / i0b;
        table_[static_cast<size_t>(ph) * kTaps + k] = static_cast<float>(sinc * w);
        sum += sinc * w;
      }
      for (int k = 0; k < kTaps; ++k) table_[static_cast<size_t>(ph) * kTaps + k] /= static_cast<float>(sum);  // unity DC gain
    }
  }

  std::vector<float> table_;
  std::vector<float> hist_;
  size_t histFrames_ = 0;
  uint32_t channels_ = 0;
  double frac_ = 0.0;
};

}  // namespace wha
