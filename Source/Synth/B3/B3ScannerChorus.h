#pragma once
#include <algorithm>
#include <array>
#include <cmath>

namespace B3 {

// Silky Hammond B3 LC Line-Box + Capacitive Scanner Vibrato/Chorus:
// - 8-stage (16-pole) dispersive LC allpass + inductor-loss ladder
// - Gentle Q (0.55..0.62) & user-controllable `depth` (0..1) so C1-C3 can be
//   dialed from subtle creamy harmonic thickening (zero warble) up to full
//   vintage swirling chorus!
class ScannerChorus {
public:
  enum Mode : int { Off = 0, V1, V2, V3, C1, C2, C3 };
  static constexpr int kNumSections = 8;
  static constexpr int kNumTaps = kNumSections + 1;

  void prepare(double sampleRate) {
    sr = sampleRate > 8000.0 ? sampleRate : 44100.0;
    constexpr double kTwoPi = 6.28318530717958647692;

    static constexpr double kSecFreqHz[kNumSections] = {
        680.0, 840.0, 1020.0, 1240.0, 1500.0, 1800.0, 2150.0, 2550.0};
    static constexpr double kSecQ[kNumSections] = {
        0.55, 0.57, 0.59, 0.61, 0.62, 0.60, 0.58, 0.56};

    for (int i = 0; i < kNumSections; ++i) {
      const double fc = std::min(kSecFreqHz[i], sr * 0.40);
      const double w0 = kTwoPi * fc / sr;
      const double cs = std::cos(w0);
      const double sn = std::sin(w0);
      const double alpha = sn / (2.0 * kSecQ[i]);
      const double a0 = 1.0 + alpha;

      sec[(size_t)i].a1 = (float)((-2.0 * cs) / a0);
      sec[(size_t)i].a2 = (float)((1.0 - alpha) / a0);

      const double lpHz = std::min(8200.0 - (double)i * 480.0, sr * 0.43);
      sec[(size_t)i].lpCoef =
          (float)(1.0 - std::exp(-kTwoPi * lpHz / sr));
    }

    const double dryHz = std::min(6400.0, sr * 0.44);
    dryToneCoef = (float)(1.0 - std::exp(-kTwoPi * dryHz / sr));

    // Slightly relaxed 6.25 Hz synchronous scanner for creamy, non-seasick motion
    scannerInc = 6.25f / (float)sr;
    driftInc = 0.19f / (float)sr;
    buildLuts();
    reset();
  }

  void reset() {
    for (auto &s : sec) {
      s.z1 = 0.0f;
      s.z2 = 0.0f;
      s.lpZ = 0.0f;
    }
    dryToneZ = 0.0f;
    lfoPhase = 0.0f;
    driftPhase = 0.0f;
  }

  void setMode(int m) { mode = (Mode)((m < 0) ? 0 : (m > C3 ? C3 : m)); }
  void setDepth(float d) { userDepth = std::clamp(d, 0.0f, 1.0f); }

  void process(float *data, int n) {
    if (mode == Off || n <= 0)
      return;

    const bool isChorus = (mode >= C1);
    const int depthIdx = isChorus ? ((int)mode - C1) : ((int)mode - V1);

    float centerTap = 3.2f;
    float baseHalfSpan = 1.0f;
    float dryMix = 0.0f;
    float wetMix = 1.0f;

    if (isChorus) {
      switch (depthIdx) {
      case 0: // C1: creamy, subtle ensemble thickener
        centerTap = 2.2f;
        baseHalfSpan = 0.95f;
        dryMix = 0.68f;
        wetMix = 0.44f;
        break;
      case 1: // C2: lush medium chorus
        centerTap = 3.3f;
        baseHalfSpan = 1.95f;
        dryMix = 0.60f;
        wetMix = 0.52f;
        break;
      default: // C3: classic rich Hammond C3
        centerTap = 4.0f;
        baseHalfSpan = 3.10f;
        dryMix = 0.54f;
        wetMix = 0.58f;
        break;
      }
    } else {
      switch (depthIdx) {
      case 0: centerTap = 2.2f; baseHalfSpan = 1.05f; break;
      case 1: centerTap = 3.3f; baseHalfSpan = 2.05f; break;
      default: centerTap = 4.0f; baseHalfSpan = 3.25f; break;
      }
    }

    // Scale excursion smoothly by userDepth (0 = static phase richness, 1 = full sweep)
    const float halfSpan = baseHalfSpan * userDepth;
    if (!lutsReady)
      buildLuts();

    for (int s = 0; s < n; ++s) {
      const float in = data[s];

      float taps[kNumTaps];
      taps[0] = in;
      float x = in;
      for (int i = 0; i < kNumSections; ++i) {
        auto &st = sec[(size_t)i];
        const float y = st.a2 * x + st.z1;
        st.z1 = st.a1 * x - st.a1 * y + st.z2;
        st.z2 = x - st.a2 * y;
        st.lpZ += st.lpCoef * (y - st.lpZ);
        const float lin = 0.84f * st.lpZ + 0.16f * y;
        // Subtle iron-core LC line-box inductor saturation for creamy thickness
        const float clamped = std::clamp(lin, -1.5f, 1.5f);
        x = lin - 0.045f * clamped * clamped * clamped;
        taps[i + 1] = x;
      }

      driftPhase += driftInc;
      if (driftPhase >= 1.0f)
        driftPhase -= 1.0f;
      const float slip = 1.0f + 0.006f * fastSinNorm(driftPhase);

      lfoPhase += scannerInc * slip;
      if (lfoPhase >= 1.0f)
        lfoPhase -= 1.0f;

      const float tri = 1.0f - 2.0f * std::abs(2.0f * lfoPhase - 1.0f);
      const float sinSwp = -fastCosNorm(lfoPhase);
      const float bipolar = 0.45f * tri + 0.55f * sinSwp;

      const float scanPos =
          std::clamp(centerTap + bipolar * halfSpan, 0.0f, (float)(kNumTaps - 1));

      float wet = 0.0f;
      float wSum = 0.0f;
      const int minTap = std::max(0, (int)(scanPos - 1.70f));
      const int maxTap = std::min(kNumTaps - 1, (int)(scanPos + 1.70f) + 1);
      for (int t = minTap; t <= maxTap; ++t) {
        const float dist = std::abs((float)t - scanPos) * (1.0f / 1.75f);
        if (dist < 1.0f) {
          const float w = fastHann(dist);
          wet += taps[t] * w;
          wSum += w;
        }
      }
      if (wSum > 1.0e-5f)
        wet /= wSum;

      if (isChorus) {
        dryToneZ += dryToneCoef * (in - dryToneZ);
        const float dryMatched = 0.75f * dryToneZ + 0.25f * in;
        data[s] = dryMix * dryMatched + wetMix * wet;
      } else {
        data[s] = wet;
      }
    }
  }

private:
  static constexpr int kTrigSize = 1024;
  static constexpr int kHannSize = 512;

  void buildLuts() {
    constexpr double kTwoPi = 6.28318530717958647692;
    constexpr double kPi = 3.14159265358979323846;
    for (int i = 0; i < kTrigSize; ++i) {
      const double a = (double)i * (kTwoPi / (double)kTrigSize);
      sinLut[(size_t)i] = (float)std::sin(a);
      cosLut[(size_t)i] = (float)std::cos(a);
    }
    for (int i = 0; i <= kHannSize; ++i) {
      const double u = (double)i / (double)kHannSize;
      hannLut[(size_t)i] = (float)(0.5 + 0.5 * std::cos(kPi * u));
    }
    lutsReady = true;
  }

  inline float fastSinNorm(float phase01) const noexcept {
    const float idx = phase01 * (float)kTrigSize;
    const int i0 = ((int)idx) & (kTrigSize - 1);
    const int i1 = (i0 + 1) & (kTrigSize - 1);
    const float f = idx - (float)(int)idx;
    const float a = sinLut[(size_t)i0];
    return a + f * (sinLut[(size_t)i1] - a);
  }

  inline float fastCosNorm(float phase01) const noexcept {
    const float idx = phase01 * (float)kTrigSize;
    const int i0 = ((int)idx) & (kTrigSize - 1);
    const int i1 = (i0 + 1) & (kTrigSize - 1);
    const float f = idx - (float)(int)idx;
    const float a = cosLut[(size_t)i0];
    return a + f * (cosLut[(size_t)i1] - a);
  }

  inline float fastHann(float u01) const noexcept {
    const float idx = u01 * (float)kHannSize;
    const int i0 = (int)idx;
    const float f = idx - (float)i0;
    const float a = hannLut[(size_t)i0];
    return a + f * (hannLut[(size_t)(i0 + 1)] - a);
  }

  struct LcSection {
    float a1 = 0.0f, a2 = 0.0f;
    float lpCoef = 0.5f;
    float z1 = 0.0f, z2 = 0.0f, lpZ = 0.0f;
  };

  std::array<LcSection, kNumSections> sec{};
  std::array<float, kTrigSize> sinLut{};
  std::array<float, kTrigSize> cosLut{};
  std::array<float, kHannSize + 1> hannLut{};
  bool lutsReady = false;
  float dryToneCoef = 0.5f;
  float dryToneZ = 0.0f;
  float lfoPhase = 0.0f;
  float driftPhase = 0.0f;
  float scannerInc = 6.25f / 44100.0f;
  float driftInc = 0.19f / 44100.0f;
  double sr = 44100.0;
  Mode mode = Off;
  float userDepth = 0.28f;
};

} // namespace B3
