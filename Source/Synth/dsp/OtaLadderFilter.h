#pragma once

#include <JuceHeader.h>
#include <cmath>
#include "SynthDspMath.h"

namespace FanfareSynth::Dsp {

struct OtaLadderFilter {
  double s1 = 0.0;
  double s2 = 0.0;
  double s3 = 0.0;
  double s4 = 0.0;
  double G = 0.5;
  double alpha = 0.5;
  double resK = 0.0;

  void reset() noexcept {
    s1 = 0.0;
    s2 = 0.0;
    s3 = 0.0;
    s4 = 0.0;
  }

  static inline double computeG(double cutoffHz, double sampleRate) noexcept {
    const double maxHz =
        juce::jmin(DspConfig::kMaxCutoffHz, sampleRate * DspConfig::kMaxNyquistRatio);
    const double fc = juce::jlimit(DspConfig::kMinCutoffHz, maxHz, cutoffHz);
    const double g = fastTanNormalized(fc / sampleRate);
    return g / (1.0 + g);
  }

  inline void setResonance(double resonanceNorm) noexcept {
    const double r = juce::jlimit(0.0, 1.0, resonanceNorm);
    const double shaped = r * (0.65 + 0.35 * r);
    resK = shaped * DspConfig::kLadderMaxResonanceK;
  }

  inline void setG(double newG) noexcept {
    G = newG;
    alpha = 1.0 - newG;
  }

  inline void setCoefficients(double cutoffHz, double sampleRate,
                              double resonanceNorm) noexcept {
    setG(computeG(cutoffHz, sampleRate));
    setResonance(resonanceNorm);
  }

  inline double process(double x, bool slope24dB) noexcept {
    const double G2 = G * G;
    const double G3 = G2 * G;
    const double G4 = G2 * G2;

    const double sigma = alpha * (G3 * s1 + G2 * s2 + G * s3 + s4);
    const double bassComp = 1.0 + resK * DspConfig::kLadderBassCompAmount;
    const double linearU =
        ((x + 1e-9) * bassComp - resK * sigma) / (1.0 + resK * G4);
    const double u = fastTanh(linearU * 0.80) * 1.25;

    const double v1 = (u - s1) * G;
    const double y1 = v1 + s1;
    s1 = y1 + v1;

    const double v2 = (y1 - s2) * G;
    const double y2 = v2 + s2;
    s2 = y2 + v2;

    const double v3 = (y2 - s3) * G;
    const double y3 = v3 + s3;
    s3 = y3 + v3;

    const double v4 = (y3 - s4) * G;
    const double y4 = v4 + s4;
    s4 = y4 + v4;

    return slope24dB ? y4 : y2;
  }
};

struct JunoHpf {
  OnePoleTpt lowShelfLp;
  OnePoleTpt highPass;
  double boostGain = 0.0;
  bool boostActive = false;
  bool hpfActive = false;

  void reset() noexcept {
    lowShelfLp.reset();
    highPass.reset();
  }

  void update(float hpfParam, double sampleRate) noexcept {
    constexpr float shelfEnd = (float)DspConfig::kHpfBoostShelfEnd;
    if (hpfParam < shelfEnd - 0.005f) {
      boostActive = true;
      hpfActive = false;
      const double amount = 1.0 - (double)(hpfParam / shelfEnd);
      const double maxLinearGain =
          std::pow(10.0, DspConfig::kHpfBoostMaxDb / 20.0) - 1.0;
      boostGain = maxLinearGain * amount;
      lowShelfLp.setCutoff(105.0, sampleRate);
    } else if (hpfParam > shelfEnd + 0.005f) {
      boostActive = false;
      hpfActive = true;
      const double t =
          (double)(hpfParam - shelfEnd) / (1.0 - (double)shelfEnd);
      const double hz =
          DspConfig::kHpfSweepMinHz *
          std::pow(DspConfig::kHpfSweepMaxHz / DspConfig::kHpfSweepMinHz,
                   juce::jlimit(0.0, 1.0, t));
      highPass.setCutoff(hz, sampleRate);
    } else {
      boostActive = false;
      hpfActive = false;
    }
  }

  inline double process(double x) noexcept {
    if (boostActive)
      return x + boostGain * lowShelfLp.processLowpass(x);
    if (hpfActive)
      return highPass.processHighpass(x);
    return x;
  }
};

} // namespace FanfareSynth::Dsp
