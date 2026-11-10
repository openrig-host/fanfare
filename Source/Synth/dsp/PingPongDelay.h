#pragma once

#include <JuceHeader.h>
#include <cmath>
#include <vector>
#include "SynthDspMath.h"

namespace FanfareSynth::Dsp {

class PingPongDelay {
public:
  void prepare(double sampleRate, float initialDelaySec) {
    sr = juce::jmax(DspConfig::kMinSampleRate, sampleRate);
    const size_t len =
        (size_t)(sr * DspConfig::kDelayMaxBufferSeconds) + 64u;
    bufL.assign(len, 0.0f);
    bufR.assign(len, 0.0f);
    writePos = 0;
    timeSmoother.prepare(sr, DspConfig::kDelayTimeSmoothSec, initialDelaySec);
    hpL.reset();
    hpR.reset();
    lpL.reset();
    lpR.reset();
    hpL.setCutoff(DspConfig::kDelayHighpassHz, sr);
    hpR.setCutoff(DspConfig::kDelayHighpassHz, sr);
  }

  void reset() noexcept {
    std::fill(bufL.begin(), bufL.end(), 0.0f);
    std::fill(bufR.begin(), bufR.end(), 0.0f);
    hpL.reset();
    hpR.reset();
    lpL.reset();
    lpR.reset();
  }

  void process(float *left, float *right, int numSamples, float delayTimeSec,
               float feedback, float dampNorm,
               OnePoleSmoother &mixSmoother) noexcept {
    const int bufLen = (int)bufL.size();
    if (bufLen < 64)
      return;

    timeSmoother.setTarget(delayTimeSec);

    if (mixSmoother.target < 1e-4f && mixSmoother.current < 1e-4f) {
      timeSmoother.advanceBlock(numSamples);
      return;
    }

    const double dampCutoff =
        DspConfig::kDelayDampBrightHz *
        std::pow(DspConfig::kDelayDampDarkHz / DspConfig::kDelayDampBrightHz,
                 juce::jlimit(0.0, 1.0, (double)dampNorm));
    lpL.setCutoff(dampCutoff, sr);
    lpR.setCutoff(dampCutoff, sr);

    const float fb = juce::jlimit(0.0f, 0.96f, feedback);
    const double maxDelaySamples = (double)(bufLen - 4);

    for (int i = 0; i < numSamples; ++i) {
      const double dlySamples =
          juce::jlimit(16.0, maxDelaySamples, (double)timeSmoother.next() * sr);

      const float tapL = readLinear(bufL, bufLen, writePos, dlySamples);
      const float tapR = readLinear(bufR, bufLen, writePos, dlySamples);

      const float fltL =
          (float)lpL.processLowpass(hpL.processHighpass((double)tapL));
      const float fltR =
          (float)lpR.processLowpass(hpR.processHighpass((double)tapR));

      const float inL = left[i];
      const float inR = right[i];
      const float monoSend = 0.5f * (inL + inR);

      // True alternating ping-pong: input enters L, L feeds R, R feeds L.
      bufL[(size_t)writePos] = fastTanhf(monoSend + fb * fltR);
      bufR[(size_t)writePos] = fastTanhf(fb * fltL);

      const float mix = mixSmoother.next();
      left[i] = inL * (1.0f - 0.35f * mix) + tapL * mix;
      right[i] = inR * (1.0f - 0.35f * mix) + tapR * mix;

      writePos = (writePos + 1) % bufLen;
    }
  }

private:
  static inline float readLinear(const std::vector<float> &buf, int bufLen,
                                 int wpos, double delaySamples) noexcept {
    double readPos = (double)wpos - delaySamples;
    while (readPos < 0.0)
      readPos += (double)bufLen;
    const int i0 = (int)readPos % bufLen;
    const int i1 = (i0 + 1) % bufLen;
    const float frac = (float)(readPos - std::floor(readPos));
    return buf[(size_t)i0] * (1.0f - frac) + buf[(size_t)i1] * frac;
  }

  double sr = DspConfig::kDefaultSampleRate;
  std::vector<float> bufL;
  std::vector<float> bufR;
  int writePos = 0;
  OnePoleSmoother timeSmoother;
  OnePoleTpt hpL, hpR;
  OnePoleTpt lpL, lpR;
};

} // namespace FanfareSynth::Dsp

