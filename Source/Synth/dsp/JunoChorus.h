#pragma once

#include <JuceHeader.h>
#include <cmath>
#include <vector>
#include "SynthDspMath.h"

namespace FanfareSynth::Dsp {

class JunoChorus {
public:
  enum Mode : int {
    Off = 0,
    ChorusI = 1,
    ChorusII = 2,
    ChorusBoth = 3,
    Custom = 4
  };

  void prepare(double sampleRate) {
    sr = juce::jmax(DspConfig::kMinSampleRate, sampleRate);
    const size_t len =
        (size_t)(sr * DspConfig::kChorusBufferSeconds) + 64u;
    bufL.assign(len, 0.0f);
    bufR.assign(len, 0.0f);
    writePos = 0;
    lfoPhase = 0.0;
    wetFilterL.reset();
    wetFilterR.reset();
    wetFilterL.setCutoff(DspConfig::kChorusBbdLowpassHz, sr);
    wetFilterR.setCutoff(DspConfig::kChorusBbdLowpassHz, sr);
  }

  void reset() noexcept {
    std::fill(bufL.begin(), bufL.end(), 0.0f);
    std::fill(bufR.begin(), bufR.end(), 0.0f);
    wetFilterL.reset();
    wetFilterR.reset();
  }

  void process(float *left, float *right, int numSamples, int mode,
               float customRateHz, float customDepthNorm,
               OnePoleSmoother &mixSmoother) noexcept {
    const int bufLen = (int)bufL.size();
    if (bufLen < 64)
      return;

    if (mode <= Off) {
      mixSmoother.setTarget(0.0f);
      if (mixSmoother.current < 1e-4f) {
        mixSmoother.reset(0.0f);
        return;
      }
    }

    float rateHz = DspConfig::kChorusModeIRateHz;
    float depthMs = DspConfig::kChorusModeIDepthMs;
    if (mode == ChorusII) {
      rateHz = DspConfig::kChorusModeIIRateHz;
      depthMs = DspConfig::kChorusModeIIDepthMs;
    } else if (mode == ChorusBoth) {
      rateHz = DspConfig::kChorusModeBothRateHz;
      depthMs = DspConfig::kChorusModeBothDepthMs;
    } else if (mode >= Custom) {
      rateHz = customRateHz;
      depthMs = DspConfig::kChorusCustomMinDepthMs +
                customDepthNorm * DspConfig::kChorusCustomSpanDepthMs;
    }

    const double phaseInc = (double)rateHz / sr;
    const double baseDelaySamples = DspConfig::kChorusBaseDelaySec * sr;
    const double depthSamples = ((double)depthMs * 0.001) * sr;

    for (int i = 0; i < numSamples; ++i) {
      const float inL = left[i];
      const float inR = right[i];
      const float monoSeed = 0.5f * (inL + inR);

      bufL[(size_t)writePos] = 0.65f * inL + 0.35f * monoSeed;
      bufR[(size_t)writePos] = 0.65f * inR + 0.35f * monoSeed;

      lfoPhase += phaseInc;
      if (lfoPhase >= 1.0)
        lfoPhase -= 1.0;

      const double tri = (lfoPhase < 0.5) ? (4.0 * lfoPhase - 1.0)
                                          : (3.0 - 4.0 * lfoPhase);

      const double delayL = baseDelaySamples + depthSamples * (0.5 + 0.5 * tri);
      const double delayR = baseDelaySamples + depthSamples * (0.5 - 0.5 * tri);

      const float rawWetL = readHermite(bufL, bufLen, writePos, delayL);
      const float rawWetR = readHermite(bufR, bufLen, writePos, delayR);

      const float wetL = (float)wetFilterL.processLowpass((double)rawWetL);
      const float wetR = (float)wetFilterR.processLowpass((double)rawWetR);

      const float mix = mixSmoother.next();
      const float dryGain = 1.0f - 0.28f * mix;
      left[i] = inL * dryGain + wetL * mix;
      right[i] = inR * dryGain + wetR * mix;

      writePos = (writePos + 1) % bufLen;
    }
  }

private:
  static inline float readHermite(const std::vector<float> &buf, int bufLen,
                                  int wpos, double delaySamples) noexcept {
    double readPos = (double)wpos - juce::jlimit(2.0, (double)(bufLen - 4), delaySamples);
    while (readPos < 0.0)
      readPos += (double)bufLen;
    const int i1 = (int)readPos % bufLen;
    const float frac = (float)(readPos - std::floor(readPos));
    const int i0 = (i1 + bufLen - 1) % bufLen;
    const int i2 = (i1 + 1) % bufLen;
    const int i3 = (i1 + 2) % bufLen;

    const float y0 = buf[(size_t)i0];
    const float y1 = buf[(size_t)i1];
    const float y2 = buf[(size_t)i2];
    const float y3 = buf[(size_t)i3];

    const float c0 = y1;
    const float c1 = 0.5f * (y2 - y0);
    const float c2 = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3;
    const float c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);
    return ((c3 * frac + c2) * frac + c1) * frac + c0;
  }

  double sr = DspConfig::kDefaultSampleRate;
  std::vector<float> bufL;
  std::vector<float> bufR;
  int writePos = 0;
  double lfoPhase = 0.0;
  OnePoleTpt wetFilterL;
  OnePoleTpt wetFilterR;
};

} // namespace FanfareSynth::Dsp

