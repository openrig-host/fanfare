#pragma once

#include <JuceHeader.h>
#include <cmath>
#include "../SynthConstants.h"

namespace FanfareSynth::Dsp {

enum OscWave : int {
  WaveSaw = 0,
  WavePulse = 1,
  WaveTriangle = 2,
  WaveSawPulse = 3
};

enum SubMode : int {
  SubOct1Square = 0,
  SubOct2Square = 1,
  SubOct2Pulse = 2
};

struct DcoOscillator {
  double phase = 0.0;
  int subCounter = 0;

  void reset(double initialPhase = 0.0) noexcept {
    phase = initialPhase - std::floor(initialPhase);
    subCounter = 0;
  }

  static inline double polyBlep(double t, double dt) noexcept {
    if (dt <= 1e-9)
      return 0.0;
    if (t < dt) {
      const double u = t / dt;
      return u + u - u * u - 1.0;
    }
    if (t > 1.0 - dt) {
      const double u = (t - 1.0) / dt;
      return u * u + u + u + 1.0;
    }
    return 0.0;
  }

  static inline double cornerBlamp(double distFromCorner, double dt) noexcept {
    if (dt <= 1e-9 || distFromCorner >= dt)
      return 0.0;
    const double oneMinusD = 1.0 - (distFromCorner / dt);
    return 2.0 * dt * oneMinusD * oneMinusD;
  }

  inline double renderSaw(double t, double dt) const noexcept {
    return (2.0 * t - 1.0) - polyBlep(t, dt);
  }

  inline double renderPulse(double t, double dt, double width) const noexcept {
    const double s1 = (2.0 * t - 1.0) - polyBlep(t, dt);
    double t2 = t + width;
    if (t2 >= 1.0)
      t2 -= 1.0;
    const double s2 = (2.0 * t2 - 1.0) - polyBlep(t2, dt);
    return (s1 - s2) * 0.88;
  }

  inline double renderTriangle(double t, double dt) const noexcept {
    double y = (t < 0.5) ? (4.0 * t - 1.0) : (3.0 - 4.0 * t);
    const double distZero = (t < 0.5) ? t : (1.0 - t);
    const double distHalf = std::abs(t - 0.5);
    y += cornerBlamp(distZero, dt);
    y -= cornerBlamp(distHalf, dt);
    return y;
  }

  struct Output {
    double mainSample;
    double subSample;
  };

  inline Output nextWithSub(double dt, int wave, double pulseWidth,
                            int subMode, bool computeSub) noexcept {
    dt = juce::jlimit(1e-7, DspConfig::kMaxNyquistRatio, dt);
    const double t = phase;

    double mainOut = 0.0;
    switch (wave) {
    case WaveSaw:
      mainOut = renderSaw(t, dt);
      break;
    case WavePulse:
      mainOut = renderPulse(t, dt, pulseWidth);
      break;
    case WaveTriangle:
      mainOut = renderTriangle(t, dt);
      break;
    case WaveSawPulse:
    default:
      mainOut = (renderSaw(t, dt) + renderPulse(t, dt, pulseWidth)) * 0.68;
      break;
    }

    double subOut = 0.0;
    if (computeSub) {
      if (subMode == SubOct1Square) {
        const double subPhase = 0.5 * ((double)(subCounter & 1) + t);
        const double subDt = 0.5 * dt;
        subOut = renderPulse(subPhase, subDt, 0.5);
      } else if (subMode == SubOct2Square) {
        const double subPhase = 0.25 * ((double)(subCounter & 3) + t);
        const double subDt = 0.25 * dt;
        subOut = renderPulse(subPhase, subDt, 0.5);
      } else {
        const double subPhase = 0.25 * ((double)(subCounter & 3) + t);
        const double subDt = 0.25 * dt;
        subOut = renderPulse(subPhase, subDt, 0.25);
      }
    }

    phase += dt;
    if (phase >= 1.0) {
      phase -= 1.0;
      subCounter = (subCounter + 1) & 3;
    }

    return {mainOut, subOut};
  }

  inline double nextMainOnly(double dt, int wave, double pulseWidth) noexcept {
    return nextWithSub(dt, wave, pulseWidth, SubOct1Square, false).mainSample;
  }
};

struct AnalogNoiseGenerator {
  uint32_t seed = 0x12345678u;
  double pinkState = 0.0;

  void setSeed(uint32_t s) noexcept { seed = (s != 0u) ? s : 0x6D2B79F5u; }

  inline double next() noexcept {
    seed ^= seed << 13;
    seed ^= seed >> 17;
    seed ^= seed << 5;
    const double white = (double)((int32_t)seed) * (1.0 / 2147483648.0);
    pinkState = 0.82 * pinkState + 0.18 * white;
    return 0.65 * white + 0.55 * pinkState;
  }
};

} // namespace FanfareSynth::Dsp

