#pragma once

#include <JuceHeader.h>
#include <array>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <vector>

namespace FanfareSynth::Dsp {

enum WavetableId : int {
  WtChoir = 0,
  WtGlass,
  WtVsAir,
  WtShimmer,
  WtCelestaPad,
  WtTineEp,
  WtDxEp,
  WtClavPluck,
  WtCrystalBell,
  WtSteelGamelan,
  WtFmClang,
  WtHardSync,
  WtWavefolder,
  WtResoSweep,
  WtSuperSaw,
  WtSubGrit,
  WtDrawbarOrgan,
  WtHollowReed,
  WtFluteBreath,
  WtBrassBloom,
  kNumWavetables
};

class WavetableBank {
public:
  static constexpr int kNumTables = kNumWavetables;
  static constexpr int kNumFrames = 64;
  static constexpr int kTableSize = 512;
  static constexpr int kTableMask = kTableSize - 1;
  static constexpr int kNumMips = 7;

  static const WavetableBank &instance() {
    static WavetableBank bank;
    return bank;
  }

  inline double sample(int tableIdx, double phase01, double dt,
                       double pos01) const noexcept {
    if (tableIdx < 0 || tableIdx >= kNumTables)
      return 0.0;

    // Choose lowest mip whose maxHarmonic stays safely below Nyquist (0.44 * sr).
    const double maxSafeH = 0.44 / juce::jmax(1e-6, dt);
    int mip = 0;
    while (mip < kNumMips - 1 && (double)kMipMaxHarmonic[(size_t)mip] > maxSafeH)
      ++mip;

    const double fPos = juce::jlimit(0.0, 1.0, pos01) * (double)(kNumFrames - 1);
    const int f0 = (int)fPos;
    const int f1 = (f0 < kNumFrames - 1) ? (f0 + 1) : f0;
    const float fFrac = (float)(fPos - (double)f0);

    const double p = (phase01 - std::floor(phase01)) * (double)kTableSize;
    const int i0 = ((int)p) & kTableMask;
    const int i1 = (i0 + 1) & kTableMask;
    const float pFrac = (float)(p - std::floor(p));

    const float *t0 = framePtr(tableIdx, f0, mip);
    const float *t1 = framePtr(tableIdx, f1, mip);

    const float s0 = t0[i0] + (t0[i1] - t0[i0]) * pFrac;
    const float s1 = t1[i0] + (t1[i1] - t1[i0]) * pFrac;
    return (double)(s0 + (s1 - s0) * fFrac);
  }

private:
  static constexpr std::array<int, kNumMips> kMipMaxHarmonic = {
      255, 127, 63, 31, 15, 7, 3};

  std::vector<float> storage;

  inline const float *framePtr(int tableIdx, int frameIdx,
                               int mip) const noexcept {
    const size_t idx =
        ((((size_t)tableIdx * (size_t)kNumFrames + (size_t)frameIdx) *
          (size_t)kNumMips) +
         (size_t)mip) *
        (size_t)kTableSize;
    return storage.data() + idx;
  }

  inline float *framePtrMut(int tableIdx, int frameIdx, int mip) noexcept {
    const size_t idx =
        ((((size_t)tableIdx * (size_t)kNumFrames + (size_t)frameIdx) *
          (size_t)kNumMips) +
         (size_t)mip) *
        (size_t)kTableSize;
    return storage.data() + idx;
  }

  static void ifft512(std::array<double, kTableSize> &re,
                      std::array<double, kTableSize> &im) noexcept {
    const int n = kTableSize;
    for (int i = 1, j = 0; i < n; ++i) {
      int bit = n >> 1;
      for (; j & bit; bit >>= 1)
        j ^= bit;
      j ^= bit;
      if (i < j) {
        std::swap(re[(size_t)i], re[(size_t)j]);
        std::swap(im[(size_t)i], im[(size_t)j]);
      }
    }
    const double twoPi = juce::MathConstants<double>::twoPi;
    for (int len = 2; len <= n; len <<= 1) {
      const double ang = twoPi / (double)len;
      const double wLenRe = std::cos(ang);
      const double wLenIm = std::sin(ang);
      const int half = len >> 1;
      for (int i = 0; i < n; i += len) {
        double wRe = 1.0;
        double wIm = 0.0;
        for (int j = 0; j < half; ++j) {
          const int uIdx = i + j;
          const int vIdx = uIdx + half;
          const double uRe = re[(size_t)uIdx];
          const double uIm = im[(size_t)uIdx];
          const double vRe =
              re[(size_t)vIdx] * wRe - im[(size_t)vIdx] * wIm;
          const double vIm =
              re[(size_t)vIdx] * wIm + im[(size_t)vIdx] * wRe;
          re[(size_t)uIdx] = uRe + vRe;
          im[(size_t)uIdx] = uIm + vIm;
          re[(size_t)vIdx] = uRe - vRe;
          im[(size_t)vIdx] = uIm - vIm;
          const double nextWRe = wRe * wLenRe - wIm * wLenIm;
          wIm = wRe * wLenIm + wIm * wLenRe;
          wRe = nextWRe;
        }
      }
    }
  }

  static inline double gaussPeak(double x, double center, double width) noexcept {
    const double d = (x - center) / width;
    return std::exp(-0.5 * d * d);
  }

  static inline double besselJ(int n, double x) noexcept {
    // Lightweight series for small |n| <= 24, |x| <= 8.
    double sum = 0.0;
    double term = 1.0;
    const double halfX = 0.5 * x;
    for (int k = 1; k <= n; ++k)
      term *= halfX / (double)k;
    sum = term;
    const double negX2 = -halfX * halfX;
    for (int m = 1; m <= 16; ++m) {
      term *= negX2 / ((double)m * (double)(m + n));
      sum += term;
      if (std::abs(term) < 1e-10)
        break;
    }
    return sum;
  }

  static void buildSpectrum(int tableIdx, double t,
                            std::array<double, kTableSize / 2> &mag,
                            std::array<double, kTableSize / 2> &phase) noexcept {
    mag.fill(0.0);
    phase.fill(0.0);
    const double pi = juce::MathConstants<double>::pi;

    switch (tableIdx) {
    case WtChoir: {
      // Morphing 3-formant vocal choir: U -> O -> A -> E -> I
      static constexpr double f1[5] = {2.2, 3.8, 6.2, 4.4, 2.5};
      static constexpr double f2[5] = {6.0, 7.5, 10.5, 16.5, 20.5};
      static constexpr double f3[5] = {18.0, 19.0, 22.0, 24.0, 27.0};
      const double u = t * 4.0;
      const int idx = juce::jlimit(0, 3, (int)u);
      const double frac = u - (double)idx;
      const double c1 = f1[idx] + (f1[idx + 1] - f1[idx]) * frac;
      const double c2 = f2[idx] + (f2[idx + 1] - f2[idx]) * frac;
      const double c3 = f3[idx] + (f3[idx + 1] - f3[idx]) * frac;
      for (int h = 1; h < 64; ++h) {
        const double dh = (double)h;
        const double glottal = 1.0 / std::pow(dh, 0.85);
        const double env = 0.25 + 1.0 * gaussPeak(dh, c1, 1.4) +
                           0.75 * gaussPeak(dh, c2, 2.2) +
                           0.35 * gaussPeak(dh, c3, 3.0);
        mag[(size_t)h] = glottal * env;
      }
      break;
    }

    case WtGlass: {
      // 90s Ballad Crystal Glass: warm core + shimmering upper glass cluster
      const double shimmer = std::pow(t, 1.15);
      mag[1] = 1.0;
      mag[2] = 0.45 * (1.0 - 0.35 * t);
      mag[3] = 0.20 + 0.18 * t;
      for (int h = 4; h < 48; ++h) {
        const double dh = (double)h;
        const double bellCluster =
            gaussPeak(dh, 7.0 + 18.0 * t, 3.2 + 4.5 * t) *
            ((h % 2 == 0) ? 0.65 : 1.0);
        mag[(size_t)h] = shimmer * (0.48 / std::pow(dh, 0.7)) * bellCluster;
      }
      break;
    }

    case WtVsAir: {
      // Prophet VS Air Pad: warm fundamental + airy high harmonic halo
      mag[1] = 1.0;
      mag[2] = 0.38 * (1.0 - 0.3 * t);
      mag[3] = 0.34;
      mag[5] = 0.18 + 0.12 * t;
      for (int h = 6; h < 72; ++h) {
        const double dh = (double)h;
        const double airPeak = gaussPeak(dh, 12.0 + 26.0 * t, 6.0);
        const double oddBoost = (h & 1) ? 1.0 : 0.45;
        mag[(size_t)h] = (0.04 + 0.42 * t) * oddBoost * airPeak / std::sqrt(dh);
        phase[(size_t)h] = 0.37 * dh * dh; // Soft phase dispersion for airy sheen
      }
      break;
    }

    case WtShimmer: {
      // Spectral Fifths & Octaves: 1, 2, 3, 4, 6, 8, 12, 16, 24, 32
      static constexpr int kFifths[] = {1, 2, 3, 4, 6, 8, 12, 16, 24, 32};
      for (int idx = 0; idx < 10; ++idx) {
        const int h = kFifths[idx];
        const double pos = (double)idx / 9.0;
        const double gate = juce::jlimit(0.0, 1.0, (t * 1.35 + 0.22) - pos * 0.95);
        mag[(size_t)h] = gate / std::pow((double)h, 0.52);
        phase[(size_t)h] = 0.25 * (double)idx;
      }
      mag[1] = 1.0;
      break;
    }

    case WtCelestaPad: {
      // Bell-Pad Hybrid: frame 0 = warm mellow pad, frame 63 = sparkling celesta bell
      // Ideal with ENV -> Wave Pos for 1-oscillator D-50 ballad attack-to-pad!
      const double warm = 1.0 - t;
      const double bell = t * t;
      mag[1] = 1.0;
      mag[2] = 0.42 * warm + 0.15 * bell;
      mag[3] = 0.22 * warm + 0.35 * bell;
      mag[4] = 0.48 * bell;
      mag[7] = 0.32 * bell;
      mag[9] = 0.36 * bell;
      mag[12] = 0.24 * bell;
      mag[16] = 0.22 * bell;
      mag[21] = 0.14 * bell;
      mag[25] = 0.10 * bell;
      break;
    }

    case WtTineEp: {
      // DW-8000 / Rhodes Tine EP
      mag[1] = 1.0;
      mag[2] = 0.36 + 0.22 * t;
      mag[3] = 0.14 + 0.28 * t;
      mag[4] = 0.06 + 0.24 * t;
      mag[5] = 0.18 * t;
      mag[7] = 0.24 * t * t;
      mag[11] = 0.16 * t * t;
      mag[14] = 0.12 * t * t;
      break;
    }

    case WtDxEp: {
      // 90s DX7-style FM Ballad Electric Piano
      mag[1] = 1.0;
      mag[2] = 0.24 + 0.26 * t;
      mag[3] = 0.10 + 0.22 * t;
      mag[7] = 0.18 * std::pow(t, 1.3);
      mag[13] = 0.16 * std::pow(t, 1.5);
      mag[14] = 0.28 * std::pow(t, 1.4);
      mag[15] = 0.18 * std::pow(t, 1.5);
      mag[28] = 0.08 * std::pow(t, 2.0);
      break;
    }

    case WtClavPluck: {
      // Comb-filtered pluck / clavinet
      const double pluckPos = 0.08 + 0.40 * (1.0 - t);
      const double rolloff = 1.35 - 0.65 * t;
      for (int h = 1; h < 48; ++h) {
        const double comb = std::abs(std::sin((double)h * pi * pluckPos));
        mag[(size_t)h] = comb / std::pow((double)h, rolloff);
      }
      mag[1] = juce::jmax(mag[1], 0.55);
      break;
    }

    case WtCrystalBell: {
      // Shimmering crystal / D-50 chime
      static constexpr int kBellH[] = {1, 3, 5, 8, 11, 15, 20, 27, 35};
      mag[1] = 0.90;
      for (int idx = 1; idx < 9; ++idx) {
        const int h = kBellH[idx];
        const double w = juce::jlimit(0.0, 1.0, t * 1.4 - (double)idx * 0.10);
        mag[(size_t)h] = w * (0.72 / std::pow((double)h, 0.58));
      }
      break;
    }

    case WtSteelGamelan: {
      // Metallic gamelan / kalimba attack partials
      static constexpr int kSteelH[] = {1, 4, 7, 10, 13, 17, 22, 29, 37};
      mag[1] = 0.85;
      for (int idx = 1; idx < 9; ++idx) {
        const int h = kSteelH[idx];
        const double w = juce::jlimit(0.0, 1.0, 0.15 + t * 1.2 - (double)idx * 0.08);
        mag[(size_t)h] = w * (0.85 / std::pow((double)h, 0.62));
        phase[(size_t)h] = 0.7 * (double)idx;
      }
      break;
    }

    case WtFmClang: {
      // True Bessel FM spectrum (c:m = 1:2, beta = 0.2 -> 5.8)
      const double beta = 0.2 + 5.6 * t;
      mag[1] = std::abs(besselJ(0, beta)) + 0.15;
      for (int n = 1; n <= 16; ++n) {
        const int hUp = 1 + 2 * n;
        const int hDn = std::abs(1 - 2 * n);
        const double j = std::abs(besselJ(n, beta));
        if (hUp < 64)
          mag[(size_t)hUp] += j;
        if (hDn >= 1 && hDn < 64)
          mag[(size_t)hDn] += j * 0.85;
      }
      break;
    }

    case WtHardSync: {
      // Hard-sync harmonic sweep: slave ratio 1.0 -> 5.5
      const double ratio = 1.0 + 4.5 * t;
      for (int h = 1; h < 64; ++h) {
        const double dh = (double)h;
        const double sincA = std::sin(pi * (dh - ratio));
        const double denomA = pi * (dh - ratio);
        const double lobe =
            (std::abs(denomA) < 1e-4) ? 1.0 : std::abs(sincA / denomA);
        mag[(size_t)h] = (0.28 / std::sqrt(dh)) + 0.95 * lobe / std::pow(dh, 0.45);
      }
      break;
    }

    case WtWavefolder: {
      // West-Coast Sine Wavefolder (1.0x -> 4.8x) via FFT of folded sine
      const double foldGain = 1.0 + 3.8 * t;
      std::array<double, kTableSize> timeRe{};
      std::array<double, kTableSize> timeIm{};
      for (int i = 0; i < kTableSize; ++i) {
        const double x = std::sin((double)i / (double)kTableSize *
                                  juce::MathConstants<double>::twoPi);
        timeRe[(size_t)i] = std::sin(x * foldGain * pi * 0.5);
      }
      // Forward DFT for harmonics 1..63 (runs only 64 frames once at startup)
      for (int h = 1; h < 64; ++h) {
        double sumSin = 0.0;
        const double w = (double)h * juce::MathConstants<double>::twoPi /
                         (double)kTableSize;
        for (int i = 0; i < kTableSize; ++i)
          sumSin += timeRe[(size_t)i] * std::sin(w * (double)i);
        mag[(size_t)h] = std::abs(sumSin) * (2.0 / (double)kTableSize);
      }
      break;
    }

    case WtResoSweep: {
      // Saw base + sweeping narrow resonant peak (H2 -> H28)
      const double center = 2.0 + 26.0 * t;
      for (int h = 1; h < 64; ++h) {
        const double dh = (double)h;
        const double peak = 1.0 + 3.6 * gaussPeak(dh, center, 1.35);
        mag[(size_t)h] = peak / std::pow(dh, 0.85);
      }
      break;
    }

    case WtSuperSaw: {
      // Dense phase-dispersed saw swarm
      const double spread = t;
      for (int h = 1; h < 72; ++h) {
        const double dh = (double)h;
        const double ripple = 0.65 + 0.35 * std::cos(dh * (0.4 + 1.8 * spread));
        mag[(size_t)h] = ripple / std::pow(dh, 0.92 - 0.15 * spread);
        phase[(size_t)h] = spread * (0.91 * dh * dh + 1.73 * dh);
      }
      break;
    }

    case WtSubGrit: {
      // Huge fundamental + warm odd/even saturation sweep
      mag[1] = 1.0;
      const double grit = t;
      for (int h = 2; h < 32; ++h) {
        const double dh = (double)h;
        const double oddWeight = (h & 1) ? 1.0 : 0.55;
        mag[(size_t)h] =
            (0.12 + 0.88 * grit) * oddWeight / std::pow(dh, 1.15 - 0.35 * grit);
      }
      break;
    }

    case WtDrawbarOrgan: {
      // Drawbar registration sweep: 800 -> 888 -> 888888888
      static constexpr int kDrawH[9] = {1, 3, 2, 4, 6, 8, 10, 12, 16};
      static constexpr double kOnAt[9] = {0.0, 0.15, 0.05, 0.30, 0.45,
                                          0.58, 0.70, 0.80, 0.88};
      for (int d = 0; d < 9; ++d) {
        const double w =
            (d == 0) ? 1.0
                     : juce::jlimit(0.0, 1.0, (t - kOnAt[d]) * 4.5 + 0.15);
        mag[(size_t)kDrawH[d]] += w * (0.85 / (1.0 + 0.15 * (double)d));
      }
      break;
    }

    case WtHollowReed: {
      // Pure odd-harmonic clarinet/woodwind sweep
      const double bright = 0.25 + 0.75 * t;
      for (int h = 1; h < 48; h += 2) {
        const double dh = (double)h;
        const double nasal = 1.0 + 0.65 * gaussPeak(dh, 5.0 + 6.0 * t, 2.2);
        mag[(size_t)h] = nasal / std::pow(dh, 1.45 - 0.65 * bright);
      }
      break;
    }

    case WtFluteBreath: {
      // Warm flute fundamental + soft breathy chiff
      mag[1] = 1.0;
      mag[2] = 0.22 + 0.20 * t;
      mag[3] = 0.08 + 0.16 * t;
      for (int h = 6; h < 28; ++h) {
        const double breath = gaussPeak((double)h, 11.0 + 6.0 * t, 4.0);
        mag[(size_t)h] = 0.16 * t * breath;
        phase[(size_t)h] = 1.13 * (double)h * (double)h;
      }
      break;
    }

    case WtBrassBloom:
    default: {
      // 90s Synth Brass swell
      const double cutoffH = 3.0 + 28.0 * t;
      for (int h = 1; h < 64; ++h) {
        const double dh = (double)h;
        const double lpf = 1.0 / (1.0 + std::pow(dh / cutoffH, 2.2));
        const double brassFormant = 1.0 + 0.45 * gaussPeak(dh, 6.0 + 6.0 * t, 3.0);
        mag[(size_t)h] = (lpf * brassFormant) / std::pow(dh, 0.80);
      }
      break;
    }
    }
  }

  WavetableBank() {
    storage.resize((size_t)kNumTables * (size_t)kNumFrames * (size_t)kNumMips *
                       (size_t)kTableSize,
                   0.0f);

    std::array<double, kTableSize / 2> mag{};
    std::array<double, kTableSize / 2> phase{};
    std::array<double, kTableSize> re{};
    std::array<double, kTableSize> im{};

    for (int tbl = 0; tbl < kNumTables; ++tbl) {
      for (int frm = 0; frm < kNumFrames; ++frm) {
        const double t = (double)frm / (double)(kNumFrames - 1);
        buildSpectrum(tbl, t, mag, phase);

        // Compute full-bandwidth RMS normalization factor first (mip 0).
        double rmsFull = 1.0;
        for (int mip = 0; mip < kNumMips; ++mip) {
          re.fill(0.0);
          im.fill(0.0);
          const int maxH = kMipMaxHarmonic[(size_t)mip];
          for (int h = 1; h <= maxH && h < kTableSize / 2; ++h) {
            const double m = mag[(size_t)h] * 0.5;
            if (m <= 1e-9)
              continue;
            const double ph = phase[(size_t)h];
            // Sine-basis: X[h] = -i * m * e^(i*ph), X[N-h] = conj(X[h])
            const double c = m * std::cos(ph);
            const double s = m * std::sin(ph);
            re[(size_t)h] = s;
            im[(size_t)h] = -c;
            re[(size_t)(kTableSize - h)] = s;
            im[(size_t)(kTableSize - h)] = c;
          }
          ifft512(re, im);

          if (mip == 0) {
            double sumSq = 0.0;
            for (int i = 0; i < kTableSize; ++i)
              sumSq += re[(size_t)i] * re[(size_t)i];
            rmsFull = std::sqrt(sumSq / (double)kTableSize);
            if (rmsFull < 1e-6)
              rmsFull = 1.0;
          }

          // Scale to match analog oscillator RMS (~0.58) so switching waves
          // doesn't blast or drop relative to Saw/Pulse/Tri.
          const float scale = (float)(0.58 / rmsFull);
          float *dst = framePtrMut(tbl, frm, mip);
          for (int i = 0; i < kTableSize; ++i)
            dst[i] = (float)re[(size_t)i] * scale;
        }
      }
    }
  }
};

} // namespace FanfareSynth::Dsp
