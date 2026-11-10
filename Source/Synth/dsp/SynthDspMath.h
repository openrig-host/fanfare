#pragma once

#include <JuceHeader.h>
#include <cmath>
#include "../SynthConstants.h"

namespace FanfareSynth::Dsp {

inline double fastTanh(double x) noexcept {
  if (x > 4.5)
    return 1.0;
  if (x < -4.5)
    return -1.0;
  const double x2 = x * x;
  return x * (27.0 + x2) / (27.0 + 9.0 * x2);
}

inline float fastTanhf(float x) noexcept {
  if (x > 4.5f)
    return 1.0f;
  if (x < -4.5f)
    return -1.0f;
  const float x2 = x * x;
  return x * (27.0f + x2) / (27.0f + 9.0f * x2);
}

// Accurate Pade approximation for tan(pi * f / sr) across [0 .. 0.45 * sr].
inline double fastTanNormalized(double normalizedFreq) noexcept {
  const double w = juce::jlimit(0.00005, DspConfig::kMaxNyquistRatio, normalizedFreq) *
                   juce::MathConstants<double>::pi;
  if (w > 1.15)
    return std::tan(w);
  const double w2 = w * w;
  return w * (15.0 - w2) / (15.0 - 6.0 * w2);
}

struct OnePoleSmoother {
  float current = 0.0f;
  float target = 0.0f;
  float coeff = 0.99f;

  void prepare(double sampleRate, double timeSeconds, float initialValue) noexcept {
    current = initialValue;
    target = initialValue;
    setTime(sampleRate, timeSeconds);
  }

  void setTime(double sampleRate, double timeSeconds) noexcept {
    const double samples = juce::jmax(1.0, sampleRate * timeSeconds);
    coeff = (float)std::exp(-1.0 / samples);
  }

  void reset(float value) noexcept {
    current = value;
    target = value;
  }

  void setTarget(float value) noexcept { target = value; }

  inline float next() noexcept {
    current = target + coeff * (current - target);
    return current;
  }

  inline float advanceBlock(int numSamples) noexcept {
    if (std::abs(current - target) < 1e-6f) {
      current = target;
      return current;
    }
    const float blockCoeff = std::pow(coeff, (float)numSamples);
    current = target + blockCoeff * (current - target);
    return current;
  }
};

struct OnePoleTpt {
  double state = 0.0;
  double g = 0.0;

  void reset() noexcept { state = 0.0; }

  void setCutoff(double cutoffHz, double sampleRate) noexcept {
    const double w = fastTanNormalized(cutoffHz / sampleRate);
    g = w / (1.0 + w);
  }

  inline double processLowpass(double x) noexcept {
    const double v = (x - state) * g;
    const double lp = v + state;
    state = lp + v;
    return lp;
  }

  inline double processHighpass(double x) noexcept {
    return x - processLowpass(x);
  }
};

} // namespace FanfareSynth::Dsp

