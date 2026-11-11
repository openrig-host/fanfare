#pragma once

// TX81Z-style operator waveforms for CoplanDX.
//
// The DX7 core (msfa) only offers the sine wave. The Yamaha TX81Z adds 8
// operator waveforms; their shapes are the ones measured/simulated by
// Csound's tx81z opcode (and the sc-tx81z SuperCollider port), expressed
// here as closed-form single-cycle tables in the same Q24 format as msfa's
// sintab:
//   W0 sin(x)                     W4 sin(2x)
//   W1 sin^3(x)*sign(sin x)       W5 sin^3(2x)*sign(sin 2x)
//   W2 max(sin(x),0)              W6 |sin(2x)|
//   W3 max(sin^3(x)*sign,0)       W7 |sin^3(2x)*sign(sin 2x)|
//
// Fanfare code (GPL v3); the wave shapes themselves follow the TX81Z
// hardware as documented by the Csound project.

#include <cmath>
#include <cstdint>
#include "MsfaIncludeOnce.h"
#include "msfa/synth.h"

namespace FanfareFm {

inline constexpr double kFmPi = 3.14159265358979323846;
inline constexpr int kNumFmWaves = 8;
inline constexpr int kWaveLgN = 10; // match SIN_LG_N_SAMPLES
inline constexpr int kWaveN = 1 << kWaveLgN;

inline int32_t fmWaveTabs[kNumFmWaves][kWaveN]; // Q24, one cycle

inline void initFmWaves() {
  static bool done = false;
  if (done)
    return;
  done = true;
  for (int i = 0; i < kWaveN; ++i) {
    const double x = 2.0 * kFmPi * (double)i / (double)kWaveN;
    const double s = std::sin(x);
    const double s2 = std::sin(2.0 * x);
    const double sign = s < 0.0 ? -1.0 : 1.0;
    const double sign2 = s2 < 0.0 ? -1.0 : 1.0;
    const double w[kNumFmWaves] = {
        s,
        s * s * s * sign,
        s < 0.0 ? 0.0 : s,
        (s * s * s * sign) < 0.0 ? 0.0 : (s * s * s * sign),
        s2,
        s2 * s2 * s2 * sign2,
        std::fabs(s2),
        std::fabs(s2 * s2 * s2 * sign2),
    };
    for (int k = 0; k < kNumFmWaves; ++k)
      fmWaveTabs[k][i] = (int32_t)std::lround(w[k] * 16777216.0); // Q24
  }
}

// phase is Q24 (1<<24 == one cycle), returns Q24 like Sin::lookup.
inline int32_t fmWaveLookup(int wave, int32_t phase) {
  if (wave <= 0)
    return Sin::lookup(phase);
  const int shift = 24 - kWaveLgN;
  const int32_t low = phase & ((1 << shift) - 1);
  const int idx = (phase >> shift) & (kWaveN - 1);
  const int32_t y0 = fmWaveTabs[wave][idx];
  const int32_t y1 = fmWaveTabs[wave][(idx + 1) & (kWaveN - 1)];
  return y0 + (int32_t)(((int64_t)(y1 - y0) * (int64_t)low) >> shift);
}

// Waveform-aware variant of msfa's FmOpKernel (same math, wave LUT instead of
// the sine LUT). wave == 0 is identical to FmOpKernel.
struct FmOpKernelW {
  static inline void compute(int32_t *output, const int32_t *input,
                             int32_t phase0, int32_t freq, int32_t gain1,
                             int32_t gain2, bool add, int wave) {
    int32_t dgain = (gain2 - gain1 + (N >> 1)) >> LG_N;
    int32_t gain = gain1;
    int32_t phase = phase0;
    if (add) {
      for (int i = 0; i < N; i++) {
        gain += dgain;
        int32_t y = fmWaveLookup(wave, phase + input[i]);
        output[i] += (int32_t)(((int64_t)y * (int64_t)gain) >> 24);
        phase += freq;
      }
    } else {
      for (int i = 0; i < N; i++) {
        gain += dgain;
        int32_t y = fmWaveLookup(wave, phase + input[i]);
        output[i] = (int32_t)(((int64_t)y * (int64_t)gain) >> 24);
        phase += freq;
      }
    }
  }

  static inline void compute_pure(int32_t *output, int32_t phase0,
                                  int32_t freq, int32_t gain1, int32_t gain2,
                                  bool add, int wave) {
    int32_t dgain = (gain2 - gain1 + (N >> 1)) >> LG_N;
    int32_t gain = gain1;
    int32_t phase = phase0;
    if (add) {
      for (int i = 0; i < N; i++) {
        gain += dgain;
        int32_t y = fmWaveLookup(wave, phase);
        output[i] += (int32_t)(((int64_t)y * (int64_t)gain) >> 24);
        phase += freq;
      }
    } else {
      for (int i = 0; i < N; i++) {
        gain += dgain;
        int32_t y = fmWaveLookup(wave, phase);
        output[i] = (int32_t)(((int64_t)y * (int64_t)gain) >> 24);
        phase += freq;
      }
    }
  }

  static inline void compute_fb(int32_t *output, int32_t phase0, int32_t freq,
                                int32_t gain1, int32_t gain2, int32_t *fb_buf,
                                int fb_shift, bool add, int wave) {
    int32_t dgain = (gain2 - gain1 + (N >> 1)) >> LG_N;
    int32_t gain = gain1;
    int32_t phase = phase0;
    int32_t y0 = fb_buf[0];
    int32_t y = fb_buf[1];
    if (add) {
      for (int i = 0; i < N; i++) {
        gain += dgain;
        int32_t scaled_fb = (y0 + y) >> (fb_shift + 1);
        y0 = y;
        y = fmWaveLookup(wave, phase + scaled_fb);
        y = (int32_t)(((int64_t)y * (int64_t)gain) >> 24);
        output[i] += y;
        phase += freq;
      }
    } else {
      for (int i = 0; i < N; i++) {
        gain += dgain;
        int32_t scaled_fb = (y0 + y) >> (fb_shift + 1);
        y0 = y;
        y = fmWaveLookup(wave, phase + scaled_fb);
        y = (int32_t)(((int64_t)y * (int64_t)gain) >> 24);
        output[i] = y;
        phase += freq;
      }
    }
    fb_buf[0] = y0;
    fb_buf[1] = y;
  }
};

} // namespace FanfareFm
