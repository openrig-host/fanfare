#pragma once

#include <JuceHeader.h>
#include <cmath>
#include "../SynthConstants.h"

namespace FanfareSynth::Dsp {

struct AnalogAdsr {
  enum Stage { Idle = 0, Attack, Decay, Release };

  Stage stage = Idle;
  double level = 0.0;
  double aCoef = 0.0;
  double dCoef = 0.0;
  double rCoef = 0.0;
  double aTime = 0.008;
  double dTime = 0.30;
  double sLevel = 0.80;
  double rTime = 0.35;
  double sampleRate = DspConfig::kDefaultSampleRate;

  void updateCoefficients() noexcept {
    constexpr double kAttackLogRatio = -1.51982575354; // ln((1.28 - 1.0) / 1.28)
    aCoef = std::exp(kAttackLogRatio / juce::jmax(1.0, aTime * sampleRate));
    dCoef = std::exp(-DspConfig::kDecayTimeConstantTau /
                     juce::jmax(1.0, dTime * sampleRate));
    rCoef = std::exp(-DspConfig::kDecayTimeConstantTau /
                     juce::jmax(1.0, rTime * sampleRate));
  }

  void noteOn(bool resetToZero = false) noexcept {
    if (resetToZero)
      level = 0.0;
    stage = Attack;
    updateCoefficients();
  }

  void noteOff() noexcept {
    if (stage != Idle) {
      stage = Release;
      updateCoefficients();
    }
  }

  void hardKill() noexcept {
    stage = Idle;
    level = 0.0;
  }

  inline double next() noexcept {
    switch (stage) {
    case Attack:
      level = DspConfig::kAttackTargetOvershoot +
              (level - DspConfig::kAttackTargetOvershoot) * aCoef;
      if (level >= 1.0) {
        level = 1.0;
        stage = Decay;
      }
      break;
    case Decay:
      level = sLevel + (level - sLevel) * dCoef;
      break;
    case Release:
      level *= rCoef;
      if (level < DspConfig::kEnvSilenceFloor) {
        level = 0.0;
        stage = Idle;
      }
      break;
    case Idle:
    default:
      level = 0.0;
      break;
    }
    return level;
  }

  bool isIdle() const noexcept { return stage == Idle; }
  bool isReleasing() const noexcept { return stage == Release; }
};

struct GateEnvelope {
  bool gateOpen = false;
  double level = 0.0;
  double slewStep = 0.01;

  void prepare(double sampleRate) noexcept {
    slewStep = 1.0 / juce::jmax(1.0, DspConfig::kGateSlewedSeconds * sampleRate);
  }

  void noteOn(bool resetToZero = false) noexcept {
    if (resetToZero)
      level = 0.0;
    gateOpen = true;
  }

  void noteOff() noexcept { gateOpen = false; }

  void hardKill() noexcept {
    gateOpen = false;
    level = 0.0;
  }

  inline double next() noexcept {
    if (gateOpen) {
      level = juce::jmin(1.0, level + slewStep);
    } else {
      level = juce::jmax(0.0, level - slewStep);
    }
    return level;
  }

  bool isIdle() const noexcept { return !gateOpen && level <= 1e-6; }
};

} // namespace FanfareSynth::Dsp

