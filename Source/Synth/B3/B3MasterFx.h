#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace B3 {

// Post-Cabinet Studio FX Strip for B3 Organ:
// 1. Behind-the-scenes 10-Band Strip Voicing EQ (matching Slot 3 curve + trim)
// 2. 3-Band Console / Cab EQ (135 Hz Low Shelf, 1.1 kHz Mid Bell, 4.8 kHz High Shelf)
// 3. Optical / Bus Compressor (glues drawbars + percussion, soft 3:1 knee + makeup)
// 4. Stereo Spring / Studio Room Reverb (warm damped 4-comb + 2-allpass tank)
// 5. Warm Ping-Pong Delay (301 ms, 26% feedback, 600 Hz LPF, adjustable mix)
class MasterFx {
public:
  void prepare(double sampleRate) {
    sr = sampleRate > 8000.0 ? sampleRate : 44100.0;
    const float srF = (float)sr;

    // Optical compressor ballistics
    compAtkCoef = 1.0f - std::exp(-1.0f / (0.012f * srF));
    compRelCoef = 1.0f - std::exp(-1.0f / (0.140f * srF));

    // Stereo Schroeder/Spring-Room Reverb scaled to sampleRate
    const double scale = sr / 44100.0;
    static constexpr int kCombBaseL[4] = {1116, 1188, 1277, 1356};
    static constexpr int kCombBaseR[4] = {1139, 1211, 1300, 1379};
    static constexpr int kApBaseL[2]   = {556, 441};
    static constexpr int kApBaseR[2]   = {579, 464};

    for (int i = 0; i < 4; ++i) {
      combLenL[i] = std::max(32, (int)std::lround((double)kCombBaseL[i] * scale));
      combLenR[i] = std::max(32, (int)std::lround((double)kCombBaseR[i] * scale));
      combBufL[i].assign((size_t)combLenL[i], 0.0f);
      combBufR[i].assign((size_t)combLenR[i], 0.0f);
    }
    for (int i = 0; i < 2; ++i) {
      apLenL[i] = std::max(16, (int)std::lround((double)kApBaseL[i] * scale));
      apLenR[i] = std::max(16, (int)std::lround((double)kApBaseR[i] * scale));
      apBufL[i].assign((size_t)apLenL[i], 0.0f);
      apBufR[i].assign((size_t)apLenR[i], 0.0f);
    }

    constexpr float kTwoPi = 6.28318530717958647692f;
    revDampCoef = 1.0f - std::exp(-kTwoPi * std::min(4200.0f, srF * 0.40f) / srF);
    revHpCoef = 1.0f - std::exp(-kTwoPi * 160.0f / srF);

    // 10-Band Strip Voicing EQ (restored low-end warmth + airy high-drawbar shine)
    static constexpr double kVoiceFreqs[10] = {31.25, 62.5, 125.0, 250.0, 500.0,
                                               1000.0, 2000.0, 4000.0, 8000.0, 16000.0};
    static constexpr double kVoiceGainsDb[10] = {0.2, 1.4, 2.0, 1.4, 1.4,
                                                 4.4, 4.7, 6.4, 7.2, 7.6};
    for (int i = 0; i < 10; ++i)
      setPeaking(voiceEq[(size_t)i], kVoiceFreqs[i], 1.414, kVoiceGainsDb[i]);
    voiceTrimGain = std::pow(10.0f, -6.8f / 20.0f);

    // Ping-Pong Delay (301 ms, 26% feedback, 600 Hz LPF)
    delaySamples = std::max(8, (int)std::lround(0.301 * sr));
    delayBufLen = std::max(512, delaySamples + 16);
    delayBufL.assign((size_t)delayBufLen, 0.0f);
    delayBufR.assign((size_t)delayBufLen, 0.0f);
    delayPos = 0;
    delayLpCoef = 1.0f - std::exp(-kTwoPi * std::min(600.0f, srF * 0.42f) / srF);
    delayHpCoef = 1.0f - std::exp(-kTwoPi * 65.0f / srF);

    eqDirty = true;
    reset();
  }

  void reset() {
    compEnv = 0.0f;
    lowEq.reset();
    midEq.reset();
    highEq.reset();
    for (auto &q : voiceEq)
      q.reset();
    std::fill(delayBufL.begin(), delayBufL.end(), 0.0f);
    std::fill(delayBufR.begin(), delayBufR.end(), 0.0f);
    delayPos = 0;
    delayLpL = delayLpR = 0.0f;
    delayHpL = delayHpR = 0.0f;
    for (int i = 0; i < 4; ++i) {
      std::fill(combBufL[i].begin(), combBufL[i].end(), 0.0f);
      std::fill(combBufR[i].begin(), combBufR[i].end(), 0.0f);
      combPosL[i] = combPosR[i] = 0;
      combLpL[i] = combLpR[i] = 0.0f;
    }
    for (int i = 0; i < 2; ++i) {
      std::fill(apBufL[i].begin(), apBufL[i].end(), 0.0f);
      std::fill(apBufR[i].begin(), apBufR[i].end(), 0.0f);
      apPosL[i] = apPosR[i] = 0;
    }
    revHpL = revHpR = 0.0f;
  }

  void setEq(float lowDbIn, float midDbIn, float highDbIn) {
    const float l = std::clamp(lowDbIn, -9.0f, 9.0f);
    const float m = std::clamp(midDbIn, -9.0f, 9.0f);
    const float h = std::clamp(highDbIn, -9.0f, 9.0f);
    if (std::abs(l - lowDb) > 0.01f || std::abs(m - midDb) > 0.01f ||
        std::abs(h - highDb) > 0.01f) {
      lowDb = l;
      midDb = m;
      highDb = h;
      eqDirty = true;
    }
  }

  void setComp(float v) { compAmt = std::clamp(v, 0.0f, 1.0f); }
  void setReverb(float v) { revMix = std::clamp(v, 0.0f, 1.0f); }
  void setDelayMix(float v) { delayMix = std::clamp(v, 0.0f, 1.0f); }

  void process(float *left, float *right, int n) {
    if (n <= 0)
      return;

    if (eqDirty) {
      updateEqCoefs();
      eqDirty = false;
    }

    const bool doEq = (std::abs(lowDb) > 0.05f || std::abs(midDb) > 0.05f ||
                       std::abs(highDb) > 0.05f);
    const bool doComp = (compAmt > 0.005f);
    const bool doRev = (revMix > 0.005f);
    const bool doDelay = (delayMix > 0.003f && delayBufLen > delaySamples);

    const float compThresh = 0.36f - 0.22f * compAmt;
    const float compRatioInv = 1.0f / (1.0f + 2.5f * compAmt); // up to ~3.5:1
    const float compMakeup = 1.0f + 0.42f * compAmt;

    const float revFeedback = 0.72f + 0.14f * std::min(1.0f, revMix * 1.4f);
    const float wetGain = revMix * 0.42f;
    const float dryGain = 1.0f - 0.14f * revMix;

    const float dlyDryGain = (delayMix <= 0.5f) ? 1.0f : (2.0f * (1.0f - delayMix));
    const float dlyWetGain = std::min(1.0f, delayMix * 1.6f);

    for (int s = 0; s < n; ++s) {
      float l = left[s];
      float r = right[s];

      // 0. Behind-the-scenes 10-Band Strip Voicing EQ
      for (int b = 0; b < 10; ++b)
        voiceEq[(size_t)b].process(l, r);
      l *= voiceTrimGain;
      r *= voiceTrimGain;

      // 1. 3-Band Console / Cab EQ
      if (doEq) {
        if (std::abs(lowDb) > 0.05f)  lowEq.process(l, r);
        if (std::abs(midDb) > 0.05f)  midEq.process(l, r);
        if (std::abs(highDb) > 0.05f) highEq.process(l, r);
      }

      // 2. Optical / Studio Bus Compressor (stereo-linked)
      if (doComp) {
        const float peak = std::max(std::abs(l), std::abs(r));
        compEnv += (peak > compEnv ? compAtkCoef : compRelCoef) * (peak - compEnv);
        float gain = 1.0f;
        if (compEnv > compThresh) {
          const float over = compEnv - compThresh;
          const float target = compThresh + over * compRatioInv;
          gain = target / std::max(1.0e-5f, compEnv);
        }
        const float g = gain * compMakeup;
        l *= g;
        r *= g;
      }

      // 3. Ping-Pong Stereo Delay (301 ms, 26% feedback, 600 Hz LPF)
      if (doDelay) {
        int rIdx = delayPos - delaySamples;
        if (rIdx < 0)
          rIdx += delayBufLen;
        const float delL = delayBufL[(size_t)rIdx];
        const float delR = delayBufR[(size_t)rIdx];

        delayLpL += (delL - delayLpL) * delayLpCoef;
        delayLpR += (delR - delayLpR) * delayLpCoef;
        delayHpL += (delayLpL - delayHpL) * delayHpCoef;
        delayHpR += (delayLpR - delayHpR) * delayHpCoef;

        const float filtL = std::tanh(delayLpL - delayHpL);
        const float filtR = std::tanh(delayLpR - delayHpR);
        const float monoIn = 0.5f * (l + r);

        delayBufL[(size_t)delayPos] = monoIn + filtR * 0.26f;
        delayBufR[(size_t)delayPos] = filtL * 0.26f;
        if (++delayPos >= delayBufLen)
          delayPos = 0;

        l = l * dlyDryGain + delL * dlyWetGain;
        r = r * dlyDryGain + delR * dlyWetGain;
      }

      // 4. Stereo Spring / Studio Room Reverb
      if (doRev) {
        // Gentle 160 Hz HPF on reverb send so 16' bass doesn't muddy the room
        const float monoIn = 0.5f * (l + r);
        revHpL += revHpCoef * (monoIn - revHpL);
        const float send = (monoIn - revHpL) * 0.25f;

        float accL = 0.0f, accR = 0.0f;
        for (int c = 0; c < 4; ++c) {
          const int pL = combPosL[c];
          const float outC_L = combBufL[c][(size_t)pL];
          combLpL[c] += revDampCoef * (outC_L - combLpL[c]);
          combBufL[c][(size_t)pL] = send + combLpL[c] * revFeedback;
          combPosL[c] = (pL + 1 >= combLenL[c]) ? 0 : (pL + 1);
          accL += outC_L;

          const int pR = combPosR[c];
          const float outC_R = combBufR[c][(size_t)pR];
          combLpR[c] += revDampCoef * (outC_R - combLpR[c]);
          combBufR[c][(size_t)pR] = send + combLpR[c] * revFeedback;
          combPosR[c] = (pR + 1 >= combLenR[c]) ? 0 : (pR + 1);
          accR += outC_R;
        }

        for (int a = 0; a < 2; ++a) {
          const int pL = apPosL[a];
          const float bufL = apBufL[a][(size_t)pL];
          const float outL_ap = -accL + bufL;
          apBufL[a][(size_t)pL] = accL + bufL * 0.5f;
          apPosL[a] = (pL + 1 >= apLenL[a]) ? 0 : (pL + 1);
          accL = outL_ap;

          const int pR = apPosR[a];
          const float bufR = apBufR[a][(size_t)pR];
          const float outR_ap = -accR + bufR;
          apBufR[a][(size_t)pR] = accR + bufR * 0.5f;
          apPosR[a] = (pR + 1 >= apLenR[a]) ? 0 : (pR + 1);
          accR = outR_ap;
        }

        l = l * dryGain + accL * wetGain;
        r = r * dryGain + accR * wetGain;
      }

      left[s] = l;
      right[s] = r;
    }
  }

private:
  struct StereoBiquad {
    float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    float z1L = 0, z2L = 0, z1R = 0, z2R = 0;

    void reset() { z1L = z2L = z1R = z2R = 0.0f; }

    inline void process(float &l, float &r) noexcept {
      const float yl = b0 * l + z1L;
      z1L = b1 * l - a1 * yl + z2L;
      z2L = b2 * l - a2 * yl;
      l = yl;

      const float yr = b0 * r + z1R;
      z1R = b1 * r - a1 * yr + z2R;
      z2R = b2 * r - a2 * yr;
      r = yr;
    }
  };

  void setLowShelf(StereoBiquad &q, double fc, double gainDb) {
    constexpr double kTwoPi = 6.28318530717958647692;
    const double A = std::pow(10.0, gainDb / 40.0);
    const double w0 = kTwoPi * std::min(fc, sr * 0.42) / sr;
    const double cs = std::cos(w0);
    const double sn = std::sin(w0);
    const double alpha = sn * 0.5 * std::sqrt(2.0);
    const double twoSqrtAAlpha = 2.0 * std::sqrt(A) * alpha;

    const double b0 = A * ((A + 1.0) - (A - 1.0) * cs + twoSqrtAAlpha);
    const double b1 = 2.0 * A * ((A - 1.0) - (A + 1.0) * cs);
    const double b2 = A * ((A + 1.0) - (A - 1.0) * cs - twoSqrtAAlpha);
    const double a0 = (A + 1.0) + (A - 1.0) * cs + twoSqrtAAlpha;
    const double a1 = -2.0 * ((A - 1.0) + (A + 1.0) * cs);
    const double a2 = (A + 1.0) + (A - 1.0) * cs - twoSqrtAAlpha;

    q.b0 = (float)(b0 / a0); q.b1 = (float)(b1 / a0); q.b2 = (float)(b2 / a0);
    q.a1 = (float)(a1 / a0); q.a2 = (float)(a2 / a0);
  }

  void setHighShelf(StereoBiquad &q, double fc, double gainDb) {
    constexpr double kTwoPi = 6.28318530717958647692;
    const double A = std::pow(10.0, gainDb / 40.0);
    const double w0 = kTwoPi * std::min(fc, sr * 0.42) / sr;
    const double cs = std::cos(w0);
    const double sn = std::sin(w0);
    const double alpha = sn * 0.5 * std::sqrt(2.0);
    const double twoSqrtAAlpha = 2.0 * std::sqrt(A) * alpha;

    const double b0 = A * ((A + 1.0) + (A - 1.0) * cs + twoSqrtAAlpha);
    const double b1 = -2.0 * A * ((A - 1.0) + (A + 1.0) * cs);
    const double b2 = A * ((A + 1.0) + (A - 1.0) * cs - twoSqrtAAlpha);
    const double a0 = (A + 1.0) - (A - 1.0) * cs + twoSqrtAAlpha;
    const double a1 = 2.0 * ((A - 1.0) - (A + 1.0) * cs);
    const double a2 = (A + 1.0) - (A - 1.0) * cs - twoSqrtAAlpha;

    q.b0 = (float)(b0 / a0); q.b1 = (float)(b1 / a0); q.b2 = (float)(b2 / a0);
    q.a1 = (float)(a1 / a0); q.a2 = (float)(a2 / a0);
  }

  void setPeaking(StereoBiquad &q, double fc, double Q, double gainDb) {
    constexpr double kTwoPi = 6.28318530717958647692;
    const double A = std::pow(10.0, gainDb / 40.0);
    const double w0 = kTwoPi * std::min(fc, sr * 0.42) / sr;
    const double cs = std::cos(w0);
    const double sn = std::sin(w0);
    const double alpha = sn / (2.0 * Q);

    const double b0 = 1.0 + alpha * A;
    const double b1 = -2.0 * cs;
    const double b2 = 1.0 - alpha * A;
    const double a0 = 1.0 + alpha / A;
    const double a1 = -2.0 * cs;
    const double a2 = 1.0 - alpha / A;

    q.b0 = (float)(b0 / a0); q.b1 = (float)(b1 / a0); q.b2 = (float)(b2 / a0);
    q.a1 = (float)(a1 / a0); q.a2 = (float)(a2 / a0);
  }

  void updateEqCoefs() {
    setLowShelf(lowEq, 135.0, (double)lowDb);
    setPeaking(midEq, 1100.0, 0.85, (double)midDb);
    setHighShelf(highEq, 4800.0, (double)highDb);
  }

  double sr = 44100.0;
  float lowDb = 0.0f, midDb = 0.0f, highDb = 0.0f;
  bool eqDirty = true;
  StereoBiquad lowEq{}, midEq{}, highEq{};
  std::array<StereoBiquad, 10> voiceEq{};
  float voiceTrimGain = 1.0f;

  float delayMix = 0.22f;
  std::vector<float> delayBufL, delayBufR;
  int delayBufLen = 0, delaySamples = 0, delayPos = 0;
  float delayLpCoef = 0.08f, delayHpCoef = 0.008f;
  float delayLpL = 0.0f, delayLpR = 0.0f, delayHpL = 0.0f, delayHpR = 0.0f;

  float compAmt = 0.25f;
  float compEnv = 0.0f;
  float compAtkCoef = 0.002f, compRelCoef = 0.0002f;

  float revMix = 0.18f;
  std::vector<float> combBufL[4], combBufR[4], apBufL[2], apBufR[2];
  int combLenL[4]{}, combLenR[4]{}, apLenL[2]{}, apLenR[2]{};
  int combPosL[4]{}, combPosR[4]{}, apPosL[2]{}, apPosR[2]{};
  float combLpL[4]{}, combLpR[4]{};
  float revDampCoef = 0.4f, revHpCoef = 0.02f, revHpL = 0.0f, revHpR = 0.0f;
};

} // namespace B3
