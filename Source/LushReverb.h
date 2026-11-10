#pragma once

#include <JuceHeader.h>
#include <array>
#include <cmath>
#include <vector>

namespace FanfareDSP {

// ==============================================================================
// LushReverb — Modulated Dattorro / Griesinger (Lexicon-224 lineage) stereo
// figure-8 tank reverberator.
//
// Architecture:
//   1. Stereo input -> Low-cut HPF (~115 Hz) + Bandwidth LPF
//   2. Pre-delay line (separates dry transient attack from wet bloom)
//   3. 4-stage series Schroeder allpass diffuser cascade
//   4. Cross-coupled Figure-8 stereo tank with 4 quadrature-LFO-modulated
//      allpass diffusers, 4 prime-ratio delay lines, 1-pole HF damping,
//      35 Hz sub-rumble/DC blockers, and soft-clip safety junctions
//   5. 14 decorrelated stereo output taps (7 Left, 7 Right)
// ==============================================================================
class LushReverb {
public:
  enum Character : int {
    Room = 0,
    Hall = 1,
    Plate = 2,
    StripMorph = 3
  };

  void prepare(double sampleRate) {
    sr = juce::jmax(8000.0, sampleRate);
    rateScale = sr / kReferenceRate;

    preDelayLine.allocate(scaledLen(2400) + 64);

    for (size_t i = 0; i < 4; ++i)
      inDiffusers[i].allocate(scaledLen(kInApBase[i]) + 32);

    modApL1.allocate(scaledLen((int)(kTankModApBase[0] * 1.25)) + 96);
    modApL2.allocate(scaledLen((int)(kTankModApBase[1] * 1.25)) + 96);
    modApR1.allocate(scaledLen((int)(kTankModApBase[2] * 1.25)) + 96);
    modApR2.allocate(scaledLen((int)(kTankModApBase[3] * 1.25)) + 96);

    delayL1.allocate(scaledLen((int)(kTankDelayBase[0] * 1.25)) + 96);
    delayL2.allocate(scaledLen((int)(kTankDelayBase[1] * 1.25)) + 96);
    delayR1.allocate(scaledLen((int)(kTankDelayBase[2] * 1.25)) + 96);
    delayR2.allocate(scaledLen((int)(kTankDelayBase[3] * 1.25)) + 96);

    const double dcAlpha =
        1.0 - std::exp(-juce::MathConstants<double>::twoPi * 32.0 / sr);
    dcCoef = (float)juce::jlimit(0.0005, 0.05, dcAlpha);

    const double smoothSamples = juce::jmax(1.0, sr * 0.012);
    mixSmoothCoef = (float)std::exp(-1.0 / smoothSamples);

    reset();
  }

  void reset() noexcept {
    preDelayLine.clear();
    for (auto &d : inDiffusers)
      d.clear();
    modApL1.clear();
    modApL2.clear();
    modApR1.clear();
    modApR2.clear();
    delayL1.clear();
    delayL2.clear();
    delayR1.clear();
    delayR2.clear();

    inHpState = 0.0f;
    inLpState = 0.0f;
    dampStateL = 0.0f;
    dampStateR = 0.0f;
    dcStateL = 0.0f;
    dcStateR = 0.0f;
    tankFeedL = 0.0f;
    tankFeedR = 0.0f;
    lfoPhase1 = 0.0;
    lfoPhase2 = 0.37;
  }

  void processStereo(float *left, float *right, int numSamples, int character,
                     float sizeNorm, float dampNorm, float wetMix,
                     float dryGain) noexcept {
    if (numSamples <= 0 || left == nullptr)
      return;

    if (delayL1.buf.empty())
      prepare(48000.0);

    targetWet = juce::jlimit(0.0f, 1.0f, wetMix);
    targetDry = juce::jlimit(0.0f, 1.0f, dryGain);

    if (targetWet < 1e-4f && currentWet < 1e-4f) {
      currentWet = 0.0f;
      currentDry = targetDry;
      return;
    }

    const float sz = juce::jlimit(0.0f, 1.0f, sizeNorm);
    const float dm = juce::jlimit(0.0f, 1.0f, dampNorm);
    const int mode = juce::jlimit(0, 3, character);

    float geoScale = 1.0f;
    float preDelayMs = 18.0f;
    float inDiff1 = 0.75f;
    float inDiff2 = 0.625f;
    float decay = 0.72f;
    float damp = 0.32f;
    float modAmount = 1.0f;
    double inputCutHz = 120.0;
    double bandwidthHz = 9500.0;

    if (mode == Room) {
      geoScale = 0.50f + 0.24f * sz;
      preDelayMs = 2.5f + 8.0f * sz;
      inDiff1 = 0.64f;
      inDiff2 = 0.52f;
      decay = 0.28f + 0.48f * std::pow(sz, 0.85f);
      damp = 0.30f + 0.52f * dm;
      modAmount = 0.38f;
      inputCutHz = 95.0;
      bandwidthHz = 8200.0 - 2800.0 * (double)dm;
    } else if (mode == Hall) {
      geoScale = 0.82f + 0.30f * sz;
      preDelayMs = 12.0f + 22.0f * sz;
      inDiff1 = 0.75f;
      inDiff2 = 0.625f;
      decay = 0.54f + 0.37f * std::pow(sz, 0.78f);
      damp = 0.16f + 0.56f * dm;
      modAmount = 1.05f;
      inputCutHz = 135.0;
      bandwidthHz = 9800.0 - 3800.0 * (double)dm;
    } else if (mode == Plate) {
      geoScale = 0.68f + 0.22f * sz;
      preDelayMs = 4.0f + 8.0f * sz;
      inDiff1 = 0.78f;
      inDiff2 = 0.66f;
      decay = 0.46f + 0.43f * std::pow(sz, 0.80f);
      damp = 0.10f + 0.48f * dm;
      modAmount = 0.78f;
      inputCutHz = 125.0;
      bandwidthHz = 12800.0 - 4500.0 * (double)dm;
    } else { // StripMorph: continuous Room -> Plate -> Hall sweep for Channel Strips
      geoScale = 0.52f + 0.56f * sz;
      preDelayMs = 3.0f + 22.0f * sz * sz;
      inDiff1 = 0.66f + 0.10f * sz;
      inDiff2 = 0.54f + 0.10f * sz;
      decay = 0.30f + 0.60f * std::pow(sz, 0.78f);
      damp = 0.44f - 0.22f * sz;
      modAmount = 0.42f + 0.60f * sz;
      inputCutHz = 115.0;
      bandwidthHz = 8800.0 + 2200.0 * (double)sz;
    }

    decay = juce::jlimit(0.15f, 0.915f, decay);
    damp = juce::jlimit(0.05f, 0.88f, damp);

    const float hpCoef = (float)(1.0 - std::exp(-juce::MathConstants<double>::twoPi *
                                                inputCutHz / sr));
    const float bwCoef = (float)(1.0 - std::exp(-juce::MathConstants<double>::twoPi *
                                                juce::jmin(bandwidthHz, sr * 0.42) / sr));

    const int preDelaySamp = juce::jlimit(
        1, preDelayLine.len - 4, (int)std::lround(preDelayMs * 0.001 * sr));

    const int apInSamp[4] = {
        scaledClamped(kInApBase[0], inDiffusers[0].len),
        scaledClamped(kInApBase[1], inDiffusers[1].len),
        scaledClamped(kInApBase[2], inDiffusers[2].len),
        scaledClamped(kInApBase[3], inDiffusers[3].len)};

    const double modBaseL1 = kTankModApBase[0] * rateScale * (double)geoScale;
    const double modBaseL2 = kTankModApBase[1] * rateScale * (double)geoScale;
    const double modBaseR1 = kTankModApBase[2] * rateScale * (double)geoScale;
    const double modBaseR2 = kTankModApBase[3] * rateScale * (double)geoScale;

    const double exc1 = 15.0 * rateScale * (double)modAmount;
    const double exc2 = 11.0 * rateScale * (double)modAmount;

    const int dlyLenL1 = scaledGeoClamped(kTankDelayBase[0], geoScale, delayL1.len);
    const int dlyLenL2 = scaledGeoClamped(kTankDelayBase[1], geoScale, delayL2.len);
    const int dlyLenR1 = scaledGeoClamped(kTankDelayBase[2], geoScale, delayR1.len);
    const int dlyLenR2 = scaledGeoClamped(kTankDelayBase[3], geoScale, delayR2.len);

    // Scale multi-tap readout offsets proportionally with tank geometry
    int tapL[7], tapR[7];
    const int tapMax[7] = {dlyLenR1, dlyLenR1, modApR2.len - 4, dlyLenR2,
                           dlyLenL1, modApL2.len - 4, dlyLenL2};
    const int tapMaxR[7] = {dlyLenL1, dlyLenL1, modApL2.len - 4, dlyLenL2,
                            dlyLenR1, modApR2.len - 4, dlyLenR2};
    for (int t = 0; t < 7; ++t) {
      tapL[t] = scaledGeoClamped(kOutTapsL[t], geoScale, tapMax[t]);
      tapR[t] = scaledGeoClamped(kOutTapsR[t], geoScale, tapMaxR[t]);
    }

    const double lfoInc1 = 0.83 / sr;
    const double lfoInc2 = 0.59 / sr;
    const float decayDiff2 = juce::jlimit(0.25f, 0.52f, decay + 0.12f);

    for (int i = 0; i < numSamples; ++i) {
      const float inL = left[i];
      const float inR = (right != nullptr) ? right[i] : inL;
      const float monoIn = 0.5f * (inL + inR);
      const float sideIn = 0.18f * (inL - inR);

      // 1. Input low-cut + high-frequency bandwidth conditioning
      inHpState += hpCoef * (monoIn - inHpState);
      const float highpassed = monoIn - inHpState;
      inLpState += bwCoef * (highpassed - inLpState);

      // 2. Pre-delay
      preDelayLine.write(inLpState);
      float diffused = preDelayLine.readInt(preDelaySamp);

      // 3. 4-stage input diffusion cascade
      diffused = inDiffusers[0].processAllpass(diffused, apInSamp[0], inDiff1);
      diffused = inDiffusers[1].processAllpass(diffused, apInSamp[1], inDiff1);
      diffused = inDiffusers[2].processAllpass(diffused, apInSamp[2], inDiff2);
      diffused = inDiffusers[3].processAllpass(diffused, apInSamp[3], inDiff2);

      // Quadrature dual-rate LFOs for tank modulation
      lfoPhase1 += lfoInc1;
      if (lfoPhase1 >= 1.0)
        lfoPhase1 -= 1.0;
      lfoPhase2 += lfoInc2;
      if (lfoPhase2 >= 1.0)
        lfoPhase2 -= 1.0;

      const double ang1 = lfoPhase1 * juce::MathConstants<double>::twoPi;
      const double ang2 = lfoPhase2 * juce::MathConstants<double>::twoPi;
      const double sin1 = std::sin(ang1);
      const double cos1 = std::cos(ang1);
      const double sin2 = std::sin(ang2);
      const double cos2 = std::cos(ang2);

      // 4A. Left half of Figure-8 tank (receives cross-coupled right tail)
      float sigL = softLimit(diffused + sideIn + decay * tankFeedR);
      sigL = modApL1.processModAllpass(sigL, modBaseL1 + exc1 * sin1, -0.70f);
      delayL1.write(sigL);
      sigL = delayL1.readInt(dlyLenL1);

      dampStateL = sigL * (1.0f - damp) + dampStateL * damp;
      dcStateL += dcCoef * (dampStateL - dcStateL);
      sigL = (dampStateL - dcStateL) * decay;

      sigL = modApL2.processModAllpass(sigL, modBaseL2 + exc2 * cos2, decayDiff2);
      delayL2.write(sigL);
      const float nextFeedL = delayL2.readInt(dlyLenL2);

      // 4B. Right half of Figure-8 tank (receives cross-coupled left tail)
      float sigR = softLimit(diffused - sideIn + decay * tankFeedL);
      sigR = modApR1.processModAllpass(sigR, modBaseR1 + exc1 * cos1, -0.70f);
      delayR1.write(sigR);
      sigR = delayR1.readInt(dlyLenR1);

      dampStateR = sigR * (1.0f - damp) + dampStateR * damp;
      dcStateR += dcCoef * (dampStateR - dcStateR);
      sigR = (dampStateR - dcStateR) * decay;

      sigR = modApR2.processModAllpass(sigR, modBaseR2 - exc2 * sin2, decayDiff2);
      delayR2.write(sigR);
      const float nextFeedR = delayR2.readInt(dlyLenR2);

      tankFeedL = nextFeedL;
      tankFeedR = nextFeedR;

      // 5. 7-tap decorrelated stereo accumulator (Dattorro tap matrix)
      const float wetL =
          0.86f * (delayR1.readInt(tapL[0]) + delayR1.readInt(tapL[1]) -
                   modApR2.readInt(tapL[2]) + delayR2.readInt(tapL[3]) -
                   delayL1.readInt(tapL[4]) - modApL2.readInt(tapL[5]) -
                   delayL2.readInt(tapL[6]));

      const float wetR =
          0.86f * (delayL1.readInt(tapR[0]) + delayL1.readInt(tapR[1]) -
                   modApL2.readInt(tapR[2]) + delayL2.readInt(tapR[3]) -
                   delayR1.readInt(tapR[4]) - modApR2.readInt(tapR[5]) -
                   delayR2.readInt(tapR[6]));

      currentWet = targetWet + mixSmoothCoef * (currentWet - targetWet);
      currentDry = targetDry + mixSmoothCoef * (currentDry - targetDry);

      left[i] = inL * currentDry + wetL * currentWet;
      if (right != nullptr)
        right[i] = inR * currentDry + wetR * currentWet;
      else
        left[i] = inL * currentDry + 0.5f * (wetL + wetR) * currentWet;
    }
  }

  void processMono(float *mono, int numSamples, int character, float sizeNorm,
                   float dampNorm, float wetMix, float dryGain) noexcept {
    processStereo(mono, nullptr, numSamples, character, sizeNorm, dampNorm,
                  wetMix, dryGain);
  }

private:
  struct DelayBuffer {
    std::vector<float> buf;
    int len = 0;
    int wpos = 0;

    void allocate(int capacity) {
      len = juce::jmax(32, capacity);
      buf.assign((size_t)len, 0.0f);
      wpos = 0;
    }

    void clear() noexcept {
      std::fill(buf.begin(), buf.end(), 0.0f);
      wpos = 0;
    }

    inline void write(float x) noexcept {
      buf[(size_t)wpos] = x;
      if (++wpos >= len)
        wpos = 0;
    }

    inline float readInt(int delaySamples) const noexcept {
      int r = wpos - delaySamples;
      while (r < 0)
        r += len;
      return buf[(size_t)(r % len)];
    }

    inline float processAllpass(float input, int delaySamples,
                                float g) noexcept {
      int r = wpos - delaySamples;
      while (r < 0)
        r += len;
      const float delayed = buf[(size_t)r];
      const float v = input + g * delayed;
      buf[(size_t)wpos] = v;
      if (++wpos >= len)
        wpos = 0;
      return delayed - g * v;
    }

    inline float processModAllpass(float input, double delaySamples,
                                   float g) noexcept {
      const double clamped =
          juce::jlimit(2.0, (double)(len - 4), delaySamples);
      double rpos = (double)wpos - clamped;
      while (rpos < 0.0)
        rpos += (double)len;
      const int i0 = (int)rpos % len;
      const int i1 = (i0 + 1) % len;
      const float frac = (float)(rpos - std::floor(rpos));
      const float delayed =
          buf[(size_t)i0] * (1.0f - frac) + buf[(size_t)i1] * frac;

      const float v = input + g * delayed;
      buf[(size_t)wpos] = v;
      if (++wpos >= len)
        wpos = 0;
      return delayed - g * v;
    }
  };

  static inline float softLimit(float x) noexcept {
    if (x > 4.0f)
      return 1.0f;
    if (x < -4.0f)
      return -1.0f;
    const float x2 = x * x;
    return x * (27.0f + x2) / (27.0f + 9.0f * x2);
  }

  int scaledLen(int refSamples) const noexcept {
    return juce::jmax(16, (int)std::ceil((double)refSamples * rateScale));
  }

  int scaledClamped(int refSamples, int maxLen) const noexcept {
    return juce::jlimit(2, juce::jmax(2, maxLen - 3),
                        (int)std::lround((double)refSamples * rateScale));
  }

  int scaledGeoClamped(int refSamples, float geo, int maxLen) const noexcept {
    return juce::jlimit(
        2, juce::jmax(2, maxLen - 3),
        (int)std::lround((double)refSamples * rateScale * (double)geo));
  }

  static constexpr double kReferenceRate = 29761.0;
  static constexpr int kInApBase[4] = {142, 107, 379, 277};
  static constexpr int kTankModApBase[4] = {672, 1800, 908, 2656};
  static constexpr int kTankDelayBase[4] = {4453, 3720, 4217, 3163};
  static constexpr int kOutTapsL[7] = {266, 2974, 1913, 1996, 1990, 187, 1066};
  static constexpr int kOutTapsR[7] = {353, 3627, 1228, 2673, 2111, 335, 121};

  double sr = 48000.0;
  double rateScale = 48000.0 / kReferenceRate;

  DelayBuffer preDelayLine;
  std::array<DelayBuffer, 4> inDiffusers;
  DelayBuffer modApL1, modApL2, modApR1, modApR2;
  DelayBuffer delayL1, delayL2, delayR1, delayR2;

  float inHpState = 0.0f;
  float inLpState = 0.0f;
  float dampStateL = 0.0f;
  float dampStateR = 0.0f;
  float dcStateL = 0.0f;
  float dcStateR = 0.0f;
  float dcCoef = 0.004f;

  float tankFeedL = 0.0f;
  float tankFeedR = 0.0f;
  double lfoPhase1 = 0.0;
  double lfoPhase2 = 0.37;

  float currentWet = 0.0f;
  float targetWet = 0.0f;
  float currentDry = 1.0f;
  float targetDry = 1.0f;
  float mixSmoothCoef = 0.99f;
};

} // namespace FanfareDSP
