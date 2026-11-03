#pragma once

#include "FanfareConstants.h"
#include "JuceHeader.h"
#include <juce_dsp/juce_dsp.h>
#include "MusetteDetuner.h"
#include <atomic>
#include <cmath>

// ==============================================================================
// OpenRig Channel Strip DSP
// Dependency-free implementation of Gate, EQ, and Compressor
// ==============================================================================

namespace FanfareDSP {

// Simple math constants
constexpr float PI = 3.14159265359f;

// ==============================================================================
// SIMPLE GATE
// ==============================================================================
class SimpleGate {
public:
  void prepare(double sampleRate) {
    sr = sampleRate;
    attackCoeff = 1.0f - std::exp(-1.0f / (0.001f * (float)sr));  // 1ms
    releaseCoeff = 1.0f - std::exp(-1.0f / (0.2f * (float)sr));   // 200ms
  }

  void setThreshold(float db) { threshold = db; }
  void setEnabled(bool enabled) { isActive = enabled; }

  float process(float input) {
    if (!isActive)
      return input;

    float inputAbs = std::abs(input);
    float inputDb =
        (inputAbs > 0.000001f) ? 20.0f * std::log10(inputAbs) : -100.0f;

    // Fast Attack (1ms), Moderate Release (200ms)
    float targetGain = (inputDb > threshold) ? 1.0f : 0.0f;
    float coeff = (targetGain < currentGain) ? releaseCoeff : attackCoeff;

    currentGain += (targetGain - currentGain) * coeff;
    if (currentGain < 1.0e-15f)
      currentGain = 0.0f;

    return input * currentGain;
  }

private:
  double sr = 44100.0;
  bool isActive = false;
  float threshold = -60.0f;
  float currentGain = 0.0f;

  float attackCoeff = 0.05f;
  float releaseCoeff = 0.0002f;
};

// ==============================================================================
// SIMPLE EQ (Biquad)
// ==============================================================================
class Biquad {
public:
  // Coefficients are normalized by a0 on the way in so process() avoids a
  // per-sample division (and the divide-by-zero / inf risk if a0 == 0). A
  // degenerate a0 configures a safe passthrough instead of corrupting the chain.
  void setCoefficients(float newA0, float newA1, float newA2, float newB0,
                       float newB1, float newB2) {
    if (std::abs(newA0) < 1.0e-12f) {
      a1 = a2 = b1 = b2 = 0.0f;
      b0 = 1.0f;
      return;
    }
    const float inv = 1.0f / newA0;
    a1 = newA1 * inv;
    a2 = newA2 * inv;
    b0 = newB0 * inv;
    b1 = newB1 * inv;
    b2 = newB2 * inv;
  }

  void reset() { z1 = z2 = y1 = y2 = 0.0f; }

  float process(float x) {
    float out = b0 * x + b1 * z1 + b2 * z2 - a1 * y1 - a2 * y2;
    z2 = z1;
    z1 = x;
    y2 = y1;
    y1 = (std::abs(out) < 1.0e-15f) ? 0.0f : out;
    return y1;
  }

private:
  float a1 = 0.0f, a2 = 0.0f;
  float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f;
  float z1 = 0.0f, z2 = 0.0f; // Inputs
  float y1 = 0.0f, y2 = 0.0f; // Outputs
};

class SimpleEQ {
public:
  static constexpr float kBandFreqs[10] = {
    31.25f, 62.5f, 125.0f, 250.0f, 500.0f, 1000.0f, 2000.0f, 4000.0f, 8000.0f, 16000.0f
  };

  void prepare(double sampleRate) {
    sr = sampleRate > 0.0 ? sampleRate : 44100.0;
    for (int i = 0; i < 10; ++i) {
      bands[i].reset();
      updateBand(i, bandGains[i]);
    }
  }

  void setEnabled(bool enabled) { isActive = enabled; }

  void setBandGain(int index, float gainDb) {
    if (index >= 0 && index < 10) {
      if (std::abs(bandGains[index] - gainDb) > 0.01f) {
        bandGains[index] = gainDb;
        updateBand(index, gainDb);
      }
    }
  }

  void setVolGain(float db) {
    volGainLinear = std::pow(10.0f, db / 20.0f);
  }

  void setMasterGain(float db) {
    masterGainLinear = std::pow(10.0f, db / 20.0f);
  }

  // Legacy compatibility helpers
  void setHighPass(float freq) { juce::ignoreUnused(freq); }
  void setLowShelf(float db) { setBandGain(1, db); }
  void setHighShelf(float db) { setBandGain(8, db); }

  float process(float input) {
    if (!isActive)
      return input;

    float out = input * volGainLinear;
    for (int i = 0; i < 10; ++i) {
      out = bands[i].process(out);
    }
    return out * masterGainLinear;
  }

private:
  double sr = 44100.0;
  bool isActive = false;
  float bandGains[10]{0.0f};
  float volGainLinear = 1.0f;
  float masterGainLinear = 1.0f;
  Biquad bands[10];

  void updateBand(int idx, float gainDb) {
    float f0 = kBandFreqs[idx];
    if (std::abs(gainDb) < 0.05f) {
      bands[idx].setCoefficients(1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f);
      return;
    }
    float w0 = 2.0f * PI * f0 / (float)sr;
    float cs = std::cos(w0);
    float sn = std::sin(w0);
    float A = std::pow(10.0f, gainDb / 40.0f);
    float alpha = sn / (2.0f * 1.414f);

    float b0 = 1.0f + alpha * A;
    float b1 = -2.0f * cs;
    float b2 = 1.0f - alpha * A;
    float a0 = 1.0f + alpha / A;
    float a1 = -2.0f * cs;
    float a2 = 1.0f - alpha / A;

    bands[idx].setCoefficients(a0, a1, a2, b0, b1, b2);
  }
};

// ==============================================================================
// UPGRADED COMPRESSOR WITH PRESETS & MAKEUP GAIN
// ==============================================================================
enum class CompPreset {
  Manual = 0,
  MP3Leveler,    // Smooth MP3 / Playlist Leveler
  VocalFocus,    // Vocal / Lead Focus
  DrumsPunch,    // Punchy Drums / Beats
  HardLimiter    // Brickwall Peak Control
};

class SimpleComp {
public:
  void prepare(double sampleRate) {
    sr = sampleRate > 0.0 ? sampleRate : 44100.0;
    updateCoefficients(0.010f, 0.200f);
  }

  void setEnabled(bool enabled) { isActive = enabled; }

  void updateCoefficients(float attackSec, float releaseSec) {
    float att = juce::jmax(0.0005f, attackSec);
    float rel = juce::jmax(0.005f, releaseSec);
    attackCoeff = 1.0f - std::exp(-1.0f / (att * (float)sr));
    releaseCoeff = 1.0f - std::exp(-1.0f / (rel * (float)sr));
  }

  void setPreset(int pIdx, float userMakeupDb = 0.0f) {
    presetIndex = pIdx;
    userMakeupGainDb = userMakeupDb;

    switch (pIdx) {
    case 1: // MP3 / Track Leveler
      thresholdDb = -26.0f;
      ratio = 3.5f;
      presetMakeupDb = 6.0f;
      updateCoefficients(0.015f, 0.280f);
      break;
    case 2: // Vocal / Lead Focus
      thresholdDb = -20.0f;
      ratio = 4.0f;
      presetMakeupDb = 4.0f;
      updateCoefficients(0.005f, 0.180f);
      break;
    case 3: // Punchy Drums / Beats
      thresholdDb = -16.0f;
      ratio = 4.0f;
      presetMakeupDb = 3.0f;
      updateCoefficients(0.030f, 0.100f);
      break;
    case 4: // Brickwall Peak Control / Limiter
      thresholdDb = -4.0f;
      ratio = 12.0f;
      presetMakeupDb = 0.0f;
      updateCoefficients(0.001f, 0.050f);
      break;
    case 0: // Manual (driven by amount knob)
    default:
      thresholdDb = -10.0f - (30.0f * amount);
      ratio = 2.0f + (6.0f * amount);
      presetMakeupDb = 6.0f * amount;
      updateCoefficients(0.010f, 0.200f);
      break;
    }
  }

  void setAmount(float amt) {
    amount = amt;
    if (presetIndex == 0) {
      setPreset(0, userMakeupGainDb);
    }
  }

  void setMakeupGain(float db) {
    userMakeupGainDb = db;
  }

  float getGainReductionDb() const { return currentGrDb.load(); }

  float process(float input) {
    if (!isActive) {
      currentGrDb.store(0.0f);
      return input;
    }

    float inputAbs = std::abs(input);
    float inputDb = (inputAbs > 0.000001f) ? 20.0f * std::log10(inputAbs) : -100.0f;

    float overDb = inputDb - thresholdDb;
    if (overDb < 0.0f)
      overDb = 0.0f;

    float grDb = overDb * (1.0f - 1.0f / ratio);
    float coeff = (grDb > envelopeDb) ? attackCoeff : releaseCoeff;
    envelopeDb += (grDb - envelopeDb) * coeff;
    if (std::abs(envelopeDb) < 1.0e-9f) envelopeDb = 0.0f;

    currentGrDb.store(envelopeDb);

    float totalGainDb = presetMakeupDb + userMakeupGainDb - envelopeDb;
    float totalGain = juce::Decibels::decibelsToGain(totalGainDb);

    return input * totalGain;
  }

private:
  double sr = 44100.0;
  bool isActive = false;
  int presetIndex = 0;
  float amount = 0.0f;
  float thresholdDb = -10.0f;
  float ratio = 2.0f;
  float presetMakeupDb = 0.0f;
  float userMakeupGainDb = 0.0f;

  float envelopeDb = 0.0f;
  std::atomic<float> currentGrDb{0.0f};

  float attackCoeff = 0.01f;
  float releaseCoeff = 0.001f;
};

// ==============================================================================
// SIMPLE CHORUS
// ==============================================================================
class SimpleChorus {
public:
  SimpleChorus() {
    std::fill(std::begin(buffer), std::end(buffer), 0.0f);
  }

  void prepare(double sampleRate) {
    sr = sampleRate;
    std::fill(std::begin(buffer), std::end(buffer), 0.0f);
    writeIndex = 0;
    lfoPhase = 0.0f;
  }

  void setEnabled(bool enabled) { isActive = enabled; }
  void setRate(float hz) { rate = hz; }
  void setMix(float mx) { mix = mx; }

  float process(float input, float lfoPhaseOffset) {
    if (!isActive || mix <= 0.001f)
      return input;

    // Modulate LFO
    float lfo = std::sin(lfoPhase + lfoPhaseOffset);
    lfoPhase += 2.0f * PI * rate / (float)sr;
    if (lfoPhase >= 2.0f * PI)
      lfoPhase -= 2.0f * PI;

    // Delay ranges between 10ms and 20ms
    float delayMs = 15.0f + 5.0f * lfo;
    float delaySamples = delayMs * 0.001f * (float)sr;

    // Read with linear interpolation
    float readPos = (float)writeIndex - delaySamples;
    while (readPos < 0.0f)
      readPos += (float)bufferSize;

    int idx0 = (int)readPos % bufferSize;
    int idx1 = (idx0 + 1) % bufferSize;
    float frac = readPos - (float)((int)readPos);

    float wet = buffer[idx0] * (1.0f - frac) + buffer[idx1] * frac;

    buffer[writeIndex] = input;
    writeIndex = (writeIndex + 1) % bufferSize;

    return input * (1.0f - mix) + wet * mix;
  }

private:
  double sr = 44100.0;
  bool isActive = false;
  float rate = 1.0f;
  float mix = 0.0f;

  static constexpr int bufferSize = 4096;
  float buffer[bufferSize];
  int writeIndex = 0;
  float lfoPhase = 0.0f;
};

// ==============================================================================
// STEREO / PING-PONG / MONO / TAPE DELAY
// Zero-allocation real-time delay line (pre-allocated up to 2.5s in prepare()).
// ==============================================================================
class StereoDelay {
public:
  StereoDelay() { prepare(48000.0); }

  void prepare(double sampleRate) {
    sr = sampleRate > 1000.0 ? sampleRate : 48000.0;
    const int needed = juce::jmax(16384, (int)std::ceil(sr * 2.5) + 16);
    if ((int)bufL.size() != needed) {
      bufL.assign((size_t)needed, 0.0f);
      bufR.assign((size_t)needed, 0.0f);
    } else {
      reset();
    }
    bufSize = needed;
    writeIdx = 0;
    timeSmoothCoeff = 1.0f - std::exp(-1.0f / (0.035f * (float)sr));
    hpCoeff = 1.0f - std::exp(-2.0f * PI * 65.0f / (float)sr);
    currentDelaySamples = juce::jlimit(
        1.0f, (float)(bufSize - 4), targetTimeMs * 0.001f * (float)sr);
    updateToneFilter(toneHz);
  }

  void reset() {
    std::fill(bufL.begin(), bufL.end(), 0.0f);
    std::fill(bufR.begin(), bufR.end(), 0.0f);
    lpStateL = lpStateR = 0.0f;
    hpStateL = hpStateR = 0.0f;
    tapePhase = 0.0f;
  }

  void setParameters(bool enabled, int modeIdx, float timeMs, float fb,
                     float cutoffHz, float wetMix) {
    if (enabled && !wasEnabled) {
      reset();
      currentDelaySamples = juce::jlimit(
          4.0f, (float)(bufSize - 8), timeMs * 0.001f * (float)sr);
    }
    wasEnabled = enabled;
    isActive = enabled;
    mode = juce::jlimit(0, 3, modeIdx);
    targetTimeMs = juce::jlimit(10.0f, 2000.0f, timeMs);
    feedback = juce::jlimit(0.0f, 0.92f, fb);
    mix = juce::jlimit(0.0f, 1.0f, wetMix);
    if (std::abs(cutoffHz - toneHz) > 1.0f) {
      toneHz = juce::jlimit(600.0f, 16000.0f, cutoffHz);
      updateToneFilter(toneHz);
    }
  }

  void processStereo(float &L, float &R) {
    if (!isActive || mix <= 0.001f || bufSize < 32)
      return;

    const float targetSamples = juce::jlimit(
        4.0f, (float)(bufSize - 8), targetTimeMs * 0.001f * (float)sr);
    currentDelaySamples +=
        (targetSamples - currentDelaySamples) * timeSmoothCoeff;

    float modSamplesL = 0.0f;
    float modSamplesR = 0.0f;
    if (mode == 3) { // Tape Warm subtle wow/flutter
      modSamplesL = std::sin(tapePhase) * (0.00055f * (float)sr);
      modSamplesR = std::cos(tapePhase) * (0.00055f * (float)sr);
      tapePhase += 2.0f * PI * 0.55f / (float)sr;
      if (tapePhase >= 2.0f * PI)
        tapePhase -= 2.0f * PI;
    }

    const float delL = readInterp(bufL, currentDelaySamples + modSamplesL);
    const float delR = readInterp(bufR, currentDelaySamples + modSamplesR);

    // Damping LPF + gentle 65 Hz HPF in feedback path so repeats stay musical
    lpStateL += (delL - lpStateL) * lpCoeff;
    lpStateR += (delR - lpStateR) * lpCoeff;
    if (std::abs(lpStateL) < 1.0e-15f) lpStateL = 0.0f;
    if (std::abs(lpStateR) < 1.0e-15f) lpStateR = 0.0f;
    hpStateL += (lpStateL - hpStateL) * hpCoeff;
    hpStateR += (lpStateR - hpStateR) * hpCoeff;
    if (std::abs(hpStateL) < 1.0e-15f) hpStateL = 0.0f;
    if (std::abs(hpStateR) < 1.0e-15f) hpStateR = 0.0f;
    float filtL = lpStateL - hpStateL;
    float filtR = lpStateR - hpStateR;

    if (mode == 3) {
      filtL = std::tanh(filtL * 1.35f) * 0.82f;
      filtR = std::tanh(filtR * 1.35f) * 0.82f;
    } else {
      filtL = std::tanh(filtL);
      filtR = std::tanh(filtR);
    }
    if (std::abs(filtL) < 1.0e-15f)
      filtL = 0.0f;
    if (std::abs(filtR) < 1.0e-15f)
      filtR = 0.0f;

    const float monoIn = 0.5f * (L + R);
    float writeL = 0.0f;
    float writeR = 0.0f;
    float wetOutL = delL;
    float wetOutR = delR;

    switch (mode) {
    case 1: // Ping-Pong (bounces L -> R -> L -> R)
      writeL = monoIn + filtR * feedback;
      writeR = filtL * feedback;
      break;
    case 2: // Mono
      writeL = monoIn + filtL * feedback;
      writeR = writeL;
      wetOutR = wetOutL;
      break;
    case 0: // Stereo
    case 3: // Tape Warm
    default:
      writeL = L + filtL * feedback;
      writeR = R + filtR * feedback;
      break;
    }

    bufL[(size_t)writeIdx] = writeL;
    bufR[(size_t)writeIdx] = writeR;
    writeIdx = (writeIdx + 1) % bufSize;

    // Unity dry up to 50% mix so adding stage delay never drops dry level
    const float dryGain = (mix <= 0.5f) ? 1.0f : (2.0f * (1.0f - mix));
    const float wetGain = juce::jmin(1.0f, mix * 1.6f);
    L = L * dryGain + wetOutL * wetGain;
    R = R * dryGain + wetOutR * wetGain;
  }

  void processMono(float &M) {
    float dummyR = M;
    processStereo(M, dummyR);
    M = 0.5f * (M + dummyR);
  }

private:
  void updateToneFilter(float hz) {
    const float clamped =
        juce::jlimit(400.0f, (float)(sr * 0.45), hz);
    lpCoeff = 1.0f - std::exp(-2.0f * PI * clamped / (float)sr);
  }

  float readInterp(const std::vector<float> &buf, float delaySamps) const {
    const float d = juce::jlimit(2.0f, (float)(bufSize - 4), delaySamps);
    float readPos = (float)writeIdx - d;
    if (readPos < 0.0f)
      readPos += (float)bufSize;
    const int i0 = (int)readPos;
    const int i1 = (i0 + 1) % bufSize;
    const float frac = readPos - (float)i0;
    return buf[(size_t)(i0 % bufSize)] * (1.0f - frac) +
           buf[(size_t)i1] * frac;
  }

  std::vector<float> bufL, bufR;
  int bufSize = 0;
  int writeIdx = 0;
  double sr = 48000.0;
  bool isActive = false;
  bool wasEnabled = false;
  int mode = 0; // 0 = Stereo, 1 = Ping-Pong, 2 = Mono, 3 = Tape Warm
  float targetTimeMs = 375.0f;
  float currentDelaySamples = 18000.0f;
  float feedback = 0.35f;
  float toneHz = 6500.0f;
  float mix = 0.25f;
  float timeSmoothCoeff = 0.001f;
  float lpCoeff = 0.5f;
  float hpCoeff = 0.01f;
  float lpStateL = 0.0f, lpStateR = 0.0f;
  float hpStateL = 0.0f, hpStateR = 0.0f;
  float tapePhase = 0.0f;
};

// ==============================================================================
// CONVOLUTION REVERB (IR loader + Physically Synthesized Acoustic Presets)
// Impulse-response reverb / cab sim via juce::dsp::Convolution. Loading WAVs or
// built-in synthesized presets is wait-free-safe from the message thread while
// processBlock() runs on the audio thread.
// ==============================================================================
class ConvolutionReverb {
public:
  struct BuiltinPreset {
    const char *id;
    const char *menuLabel;
    const char *displayName;
  };

  static constexpr int kNumBuiltinPresets = 6;

  static const BuiltinPreset *getBuiltinPresets() {
    static const BuiltinPreset kPresets[kNumBuiltinPresets] = {
        {"builtin://wood_room", "Room: Studio Wood Room (0.55s)",
         "Built-In: Studio Wood Room"},
        {"builtin://warm_chamber", "Room: Warm Studio Chamber (1.05s)",
         "Built-In: Warm Studio Chamber"},
        {"builtin://concert_hall", "Hall: Symphony Concert Hall (2.1s)",
         "Built-In: Symphony Concert Hall"},
        {"builtin://cathedral", "Hall: Stone Sanctuary (3.0s)",
         "Built-In: Stone Sanctuary"},
        {"builtin://vintage_plate", "Plate: Vintage Studio Plate (1.5s)",
         "Built-In: Vintage Studio Plate"},
        {"builtin://warm_cab_2x12", "Cab: 2x12 Warm Tube Cab",
         "Built-In: 2x12 Warm Tube Cab"},
    };
    return kPresets;
  }

  static bool isBuiltinPath(const juce::String &pathOrId) {
    return pathOrId.startsWith("builtin://");
  }

  static int findBuiltinPresetIndex(const juce::String &pathOrId) {
    const auto *presets = getBuiltinPresets();
    for (int i = 0; i < kNumBuiltinPresets; ++i) {
      if (pathOrId == presets[i].id)
        return i;
    }
    return -1;
  }

  void prepare(double sampleRate, int maxBlockSize) {
    const double safeSr = (sampleRate > 8000.0) ? sampleRate : 48000.0;
    sr = safeSr;
    blockSize = juce::jmax(maxBlockSize, 8192);
    wetBuffer.setSize(2, blockSize, false, false, true);
    juce::dsp::ProcessSpec spec;
    spec.sampleRate = sr;
    spec.maximumBlockSize = (juce::uint32)blockSize;
    spec.numChannels = 2;
    convolution.prepare(spec);

    // Regenerate active built-in IR natively if sample rate changed
    if (hasIR && isBuiltinPath(loadedPath) &&
        std::abs(synthesizedSr - sr) > 1.0) {
      loadBuiltinPreset(loadedPath, true);
    }
  }

  // Unified loader accepting either "builtin://<id>" or a filesystem .wav path.
  bool loadPathOrPreset(const juce::String &pathOrId) {
    if (pathOrId.isEmpty())
      return false;
    if (isBuiltinPath(pathOrId))
      return loadBuiltinPreset(pathOrId, false);
    return loadIR(juce::File(pathOrId));
  }

  bool loadPresetByIndex(int presetIdx) {
    if (presetIdx < 0 || presetIdx >= kNumBuiltinPresets)
      return false;
    return loadBuiltinPreset(getBuiltinPresets()[presetIdx].id, false);
  }

  // Loads an IR (.wav). Only reloads if the path changed. Message-thread only.
  bool loadIR(const juce::File &f) {
    if (!f.existsAsFile())
      return false;
    if (f.getFullPathName() == loadedPath && hasIR)
      return true;
    convolution.loadImpulseResponse(f,
                                    juce::dsp::Convolution::Stereo::yes,
                                    juce::dsp::Convolution::Trim::yes,
                                    0,
                                    juce::dsp::Convolution::Normalise::yes);
    loadedPath = f.getFullPathName();
    synthesizedSr = 0.0;
    hasIR = true;
    return true;
  }

  bool loadBuiltinPreset(const juce::String &presetId, bool forceReload = false) {
    const int idx = findBuiltinPresetIndex(presetId);
    if (idx < 0)
      return false;
    const double targetSr = (sr > 8000.0) ? sr : 48000.0;
    if (!forceReload && hasIR && loadedPath == presetId &&
        std::abs(synthesizedSr - targetSr) <= 1.0) {
      return true;
    }

    juce::AudioBuffer<float> irBuf = synthesizePresetBuffer(idx, targetSr);
    if (irBuf.getNumSamples() <= 0)
      return false;

    convolution.loadImpulseResponse(std::move(irBuf),
                                    targetSr,
                                    juce::dsp::Convolution::Stereo::yes,
                                    juce::dsp::Convolution::Trim::no,
                                    juce::dsp::Convolution::Normalise::no);
    loadedPath = presetId;
    synthesizedSr = targetSr;
    hasIR = true;
    return true;
  }

  bool hasIrLoaded() const { return hasIR; }
  const juce::String &getLoadedPath() const { return loadedPath; }

  juce::String getDisplayName() const {
    if (loadedPath.isEmpty())
      return "(no IR loaded)";
    const int idx = findBuiltinPresetIndex(loadedPath);
    if (idx >= 0)
      return getBuiltinPresets()[idx].displayName;
    return juce::File(loadedPath).getFileName();
  }

  void processBlock(juce::AudioBuffer<float> &buffer, bool enabled, float mix) {
    if (!enabled || !hasIR || mix <= 0.001f)
      return;
    const int n = juce::jmin(buffer.getNumSamples(), wetBuffer.getNumSamples());
    const int chans = buffer.getNumChannels();
    if (n <= 0)
      return;
    for (int c = 0; c < 2; ++c)
      wetBuffer.copyFrom(c, 0, buffer, juce::jmin(c, chans - 1), 0, n);
    juce::dsp::AudioBlock<float> wetBlock =
        juce::dsp::AudioBlock<float>(wetBuffer).getSubBlock(0, (size_t)n);
    juce::dsp::ProcessContextReplacing<float> ctx(wetBlock);
    convolution.process(ctx);
    const float w = mix, d = 1.0f - mix;
    for (int c = 0; c < chans; ++c) {
      auto *dry = buffer.getWritePointer(c);
      const float *wet = wetBuffer.getReadPointer(juce::jmin(c, 1));
      for (int i = 0; i < n; ++i)
        dry[i] = dry[i] * d + wet[i] * w;
    }
  }

  void reset() {
    convolution.reset();
    wetBuffer.clear();
  }

  void clear() {
    loadedPath.clear();
    synthesizedSr = 0.0;
    hasIR = false;
    reset();
  }

private:
  // Fast deterministic PRNG so synthesized stereo spaces never drift between loads.
  struct FastRng {
    juce::uint64 state;
    explicit FastRng(juce::uint64 seed) : state(seed ? seed : 0x9E3779B97F4A7C15ULL) {}
    juce::uint64 nextU64() {
      juce::uint64 x = (state += 0x9E3779B97F4A7C15ULL);
      x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
      x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
      return x ^ (x >> 31);
    }
    float nextFloat01() {
      return (float)((nextU64() >> 40) * (1.0 / 16777216.0));
    }
    float nextBipolar() { return nextFloat01() * 2.0f - 1.0f; }
  };

  struct OfflineBiquad {
    float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f, a1 = 0.0f, a2 = 0.0f;
    static OfflineBiquad makeHPF(double sampleRate, float freqHz, float q) {
      OfflineBiquad f;
      const float w0 = 2.0f * PI * juce::jlimit(10.0f, (float)(sampleRate * 0.45), freqHz) / (float)sampleRate;
      const float cs = std::cos(w0), sn = std::sin(w0);
      const float alpha = sn / (2.0f * q);
      const float a0 = 1.0f + alpha;
      f.b0 = ((1.0f + cs) * 0.5f) / a0;
      f.b1 = (-(1.0f + cs)) / a0;
      f.b2 = ((1.0f + cs) * 0.5f) / a0;
      f.a1 = (-2.0f * cs) / a0;
      f.a2 = (1.0f - alpha) / a0;
      return f;
    }
    static OfflineBiquad makeLPF(double sampleRate, float freqHz, float q) {
      OfflineBiquad f;
      const float w0 = 2.0f * PI * juce::jlimit(40.0f, (float)(sampleRate * 0.47), freqHz) / (float)sampleRate;
      const float cs = std::cos(w0), sn = std::sin(w0);
      const float alpha = sn / (2.0f * q);
      const float a0 = 1.0f + alpha;
      f.b0 = ((1.0f - cs) * 0.5f) / a0;
      f.b1 = (1.0f - cs) / a0;
      f.b2 = ((1.0f - cs) * 0.5f) / a0;
      f.a1 = (-2.0f * cs) / a0;
      f.a2 = (1.0f - alpha) / a0;
      return f;
    }
    static OfflineBiquad makePeak(double sampleRate, float freqHz, float q, float gainDb) {
      OfflineBiquad f;
      const float A = std::pow(10.0f, gainDb / 40.0f);
      const float w0 = 2.0f * PI * juce::jlimit(20.0f, (float)(sampleRate * 0.46), freqHz) / (float)sampleRate;
      const float cs = std::cos(w0), sn = std::sin(w0);
      const float alpha = sn / (2.0f * q);
      const float a0 = 1.0f + alpha / A;
      f.b0 = (1.0f + alpha * A) / a0;
      f.b1 = (-2.0f * cs) / a0;
      f.b2 = (1.0f - alpha * A) / a0;
      f.a1 = (-2.0f * cs) / a0;
      f.a2 = (1.0f - alpha / A) / a0;
      return f;
    }
    void process(float *samples, int numSamples) const {
      float z1 = 0.0f, z2 = 0.0f;
      for (int i = 0; i < numSamples; ++i) {
        const float x = samples[i];
        const float y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        samples[i] = y;
      }
    }
  };

  struct SpaceSpec {
    float lx, ly, lz;
    float wallReflect;
    float rt60Low, rt60Mid, rt60High;
    float preDelaySec;
    float bloomSec;
    float hpfHz;
    float airLpfHz;
    float erGain;
    float tailGain;
    float targetNorm;
    juce::uint64 seed;
  };

  static void normalizeSampleRateInvariant(juce::AudioBuffer<float> &buf,
                                           double sampleRate,
                                           float targetNorm) {
    const int n = buf.getNumSamples();
    if (n <= 0)
      return;
    double maxSumSq = 0.0;
    for (int c = 0; c < buf.getNumChannels(); ++c) {
      const float *p = buf.getReadPointer(c);
      double sumSq = 0.0;
      for (int i = 0; i < n; ++i)
        sumSq += (double)p[i] * (double)p[i];
      if (sumSq > maxSumSq)
        maxSumSq = sumSq;
    }
    // Scale energy relative to 48 kHz so perceived loudness stays identical at 44.1 / 48 / 96 kHz
    const double scaledRms = std::sqrt(maxSumSq * (48000.0 / sampleRate));
    if (scaledRms > 1.0e-9) {
      const float gain = (float)((double)targetNorm / scaledRms);
      buf.applyGain(gain);
    }
  }

  static void addSoftenedTap(float *dst, int numSamples, double samplePos,
                             float amp, int spreadSamples) {
    const int center = (int)std::floor(samplePos);
    const float frac = (float)(samplePos - (double)center);
    if (spreadSamples <= 1) {
      if (center >= 0 && center < numSamples)
        dst[center] += amp * (1.0f - frac);
      if (center + 1 >= 0 && center + 1 < numSamples)
        dst[center + 1] += amp * frac;
      return;
    }
    // Raised-cosine micro-kernel softens high-order wall reflections naturally
    float weightSum = 0.0f;
    const int half = spreadSamples;
    for (int k = -half; k <= half; ++k) {
      const float u = ((float)k - frac) / (float)(half + 1);
      if (std::abs(u) < 1.0f)
        weightSum += 0.5f * (1.0f + std::cos(PI * u));
    }
    if (weightSum <= 1.0e-6f)
      return;
    const float scale = amp / weightSum;
    for (int k = -half; k <= half; ++k) {
      const int idx = center + k;
      if (idx < 0 || idx >= numSamples)
        continue;
      const float u = ((float)k - frac) / (float)(half + 1);
      if (std::abs(u) < 1.0f)
        dst[idx] += scale * 0.5f * (1.0f + std::cos(PI * u));
    }
  }

  static juce::AudioBuffer<float> synthesizeSpace(const SpaceSpec &sp,
                                                  double sampleRate) {
    const double totalSec = juce::jmax((double)sp.rt60Low, (double)sp.rt60Mid) * 1.12 +
                            (double)sp.preDelaySec + 0.05;
    const int numSamples = juce::jmax(256, (int)std::ceil(totalSec * sampleRate));
    juce::AudioBuffer<float> buf(2, numSamples);
    buf.clear();

    float *left = buf.getWritePointer(0);
    float *right = buf.getWritePointer(1);

    // 1. 3D Shoebox Image-Source Early Reflections (asymmetric source & ORTF pair)
    const float rx = sp.lx * 0.41f, ry = sp.ly * 0.31f, rz = juce::jmin(1.55f, sp.lz * 0.46f);
    const float sx = sp.lx * 0.64f, sy = sp.ly * 0.72f, sz = juce::jmin(1.42f, sp.lz * 0.42f);
    const float ddx0 = sx - rx, ddy0 = sy - ry, ddz0 = sz - rz;
    const float directDist = std::sqrt(ddx0 * ddx0 + ddy0 * ddy0 + ddz0 * ddz0);

    FastRng erRng(sp.seed ^ 0xA5A5F0F012345678ULL);
    for (int nx = -4; nx <= 4; ++nx) {
      const float ix = (nx & 1) ? ((float)(nx + 1) * sp.lx - sx) : ((float)nx * sp.lx + sx);
      for (int ny = -4; ny <= 4; ++ny) {
        const float iy = (ny & 1) ? ((float)(ny + 1) * sp.ly - sy) : ((float)ny * sp.ly + sy);
        for (int nz = -3; nz <= 3; ++nz) {
          if (nx == 0 && ny == 0 && nz == 0)
            continue;
          const int order = std::abs(nx) + std::abs(ny) + std::abs(nz);
          if (order > 5)
            continue;

          const float iz = (nz & 1) ? ((float)(nz + 1) * sp.lz - sz) : ((float)nz * sp.lz + sz);
          const float dx = ix - rx, dy = iy - ry, dz = iz - rz;
          const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
          const double relSec = (double)sp.preDelaySec +
                                (double)juce::jmax(0.0f, dist - directDist) / 343.0 +
                                (double)(erRng.nextBipolar() * 0.00035f);

          const float horiz = std::sqrt(dx * dx + dy * dy + 1.0e-6f);
          const float sinTheta = juce::jlimit(-1.0f, 1.0f, dx / horiz);
          const float cosTheta = dy / horiz;

          // ORTF stereo capsule spacing (+/- 0.25 ms ITD + cardioid ILD)
          const double itdSec = 0.00025 * (double)sinTheta;
          const float cardL = 0.55f + 0.45f * (-0.82f * sinTheta + 0.57f * cosTheta);
          const float cardR = 0.55f + 0.45f * (0.82f * sinTheta + 0.57f * cosTheta);

          const float polarity = ((order & 1) == 0) ? -1.0f : 1.0f;
          const float atten = (directDist / juce::jmax(dist, 1.0f)) *
                              std::pow(sp.wallReflect, (float)order) * sp.erGain * polarity;

          const int spread = juce::jlimit(1, 8, (order - 1) * (int)std::round(sampleRate / 44100.0));
          addSoftenedTap(left, numSamples, (relSec - itdSec) * sampleRate, atten * cardL, spread);
          addSoftenedTap(right, numSamples, (relSec + itdSec) * sampleRate, atten * cardR, spread);
        }
      }
    }

    // 2. Decorrelated True-Stereo 3-Band Velvet-Noise Diffuse Tail
    const float gLow = 1.0f - std::exp(-2.0f * PI * 260.0f / (float)sampleRate);
    const float gMidHi = 1.0f - std::exp(-2.0f * PI * 2800.0f / (float)sampleRate);
    const float kLn1000 = 6.9077553f; // ln(10^3) for -60 dB RT60 decay

    for (int ch = 0; ch < 2; ++ch) {
      float *out = buf.getWritePointer(ch);
      FastRng rng(sp.seed + (juce::uint64)(ch * 0x517CC1B727220A95ULL + 17ULL));

      int nextPulse = (int)(sp.preDelaySec * sampleRate);
      float lp260 = 0.0f;
      float lp2800 = 0.0f;
      float airState = 0.0f;

      for (int i = nextPulse; i < numSamples; ++i) {
        const float t = (float)((double)i / sampleRate) - sp.preDelaySec;
        if (t < 0.0f)
          continue;

        float excite = 0.0f;
        if (i >= nextPulse) {
          // Smoothly ramp velvet pulse density from 2000/s to 4400/s as echo density builds
          const float density = 2000.0f + 2400.0f * (1.0f - std::exp(-t / 0.08f));
          const float avgGrid = juce::jmax(2.0f, (float)sampleRate / density);
          const int step = juce::jmax(1, (int)std::round(avgGrid * (0.45f + 1.10f * rng.nextFloat01())));
          const float sign = (rng.nextU64() & 1ULL) ? 1.0f : -1.0f;
          excite = sign * (0.65f + 0.35f * rng.nextFloat01());
          nextPulse = i + step;
        }

        // Phase-coherent 3-band crossover (low + mid + high == excite)
        lp260 += gLow * (excite - lp260);
        lp2800 += gMidHi * (excite - lp2800);
        const float bandLow = lp260;
        const float bandHigh = excite - lp2800;
        const float bandMid = excite - bandLow - bandHigh;

        const float envLow = std::exp(-kLn1000 * t / sp.rt60Low);
        const float envMid = std::exp(-kLn1000 * t / sp.rt60Mid);
        const float envHigh = std::exp(-kLn1000 * t / sp.rt60High);
        const float bloom = 1.0f - std::exp(-t / juce::jmax(0.002f, sp.bloomSec));

        const float wetSample =
            sp.tailGain * bloom * (bandLow * envLow + bandMid * envMid + bandHigh * envHigh);

        // Progressive distance air absorption along late tail
        const float dynCutoff =
            juce::jmax(1400.0f, sp.airLpfHz * std::exp(-0.45f * t / sp.rt60Mid));
        const float gAir =
            1.0f - std::exp(-2.0f * PI * juce::jmin(dynCutoff, (float)(sampleRate * 0.44)) /
                            (float)sampleRate);
        airState += gAir * (wetSample - airState);
        out[i] += airState;
      }

      // Clean sub-bass rumble HPF and gentle top-end polish
      OfflineBiquad::makeHPF(sampleRate, sp.hpfHz, 0.707f).process(out, numSamples);
      OfflineBiquad::makeLPF(sampleRate, sp.airLpfHz, 0.707f).process(out, numSamples);

      // 25 ms cosine fade-out at buffer end so tail terminates silently
      const int fadeSamples = juce::jmin(numSamples / 4, (int)(0.025 * sampleRate));
      for (int k = 0; k < fadeSamples; ++k) {
        const float w = 0.5f * (1.0f - std::cos(PI * (float)k / (float)fadeSamples));
        out[numSamples - 1 - k] *= w;
      }
    }

    normalizeSampleRateInvariant(buf, sampleRate, sp.targetNorm);
    return buf;
  }

  static void runOfflineAllpass(float *samples, int numSamples, int delaySamples,
                                float feedback) {
    if (delaySamples <= 0 || numSamples <= 0)
      return;
    std::vector<float> line((size_t)delaySamples, 0.0f);
    int idx = 0;
    for (int i = 0; i < numSamples; ++i) {
      const float delayed = line[(size_t)idx];
      const float x = samples[i];
      const float v = x - feedback * delayed;
      samples[i] = delayed + feedback * v;
      line[(size_t)idx] = v;
      if (++idx >= delaySamples)
        idx = 0;
    }
  }

  static juce::AudioBuffer<float> synthesizePlate(double sampleRate) {
    const int numSamples = (int)std::ceil(1.75 * sampleRate);
    juce::AudioBuffer<float> buf(2, numSamples);
    buf.clear();

    const int preDelay = (int)std::round(0.004 * sampleRate); // 4 ms studio plate onset
    const float gLow = 1.0f - std::exp(-2.0f * PI * 300.0f / (float)sampleRate);
    const float gMidHi = 1.0f - std::exp(-2.0f * PI * 3400.0f / (float)sampleRate);
    const float kLn1000 = 6.9077553f;

    const double apMs[2][4] = {
        {1.37, 2.63, 4.19, 6.77},
        {1.51, 2.89, 4.57, 7.13},
    };

    for (int ch = 0; ch < 2; ++ch) {
      float *out = buf.getWritePointer(ch);
      FastRng rng(0x504C415445313430ULL + (juce::uint64)(ch * 99991ULL));

      // Initial tensioned-plate transient impulse + dense velvet bed
      if (preDelay < numSamples)
        out[preDelay] = (ch == 0 ? 0.55f : -0.52f);

      int nextPulse = preDelay + 1;
      float lpLow = 0.0f, lpHi = 0.0f;
      for (int i = preDelay; i < numSamples; ++i) {
        const float t = (float)((double)(i - preDelay) / sampleRate);
        float excite = 0.0f;
        if (i >= nextPulse) {
          const float avgGrid = juce::jmax(2.0f, (float)(sampleRate / 5200.0));
          const int step = juce::jmax(1, (int)std::round(avgGrid * (0.4f + 1.2f * rng.nextFloat01())));
          excite = ((rng.nextU64() & 1ULL) ? 1.0f : -1.0f) * (0.7f + 0.3f * rng.nextFloat01());
          nextPulse = i + step;
        }
        lpLow += gLow * (excite - lpLow);
        lpHi += gMidHi * (excite - lpHi);
        const float bLow = lpLow;
        const float bHigh = excite - lpHi;
        const float bMid = excite - bLow - bHigh;

        const float envLow = std::exp(-kLn1000 * t / 1.22f);
        const float envMid = std::exp(-kLn1000 * t / 1.50f);
        const float envHigh = std::exp(-kLn1000 * t / 0.85f);
        const float bloom = 1.0f - std::exp(-t / 0.0035f);
        out[i] += bloom * (bLow * envLow + bMid * envMid + bHigh * envHigh);
      }

      // 4 cascaded short Schroeder allpasses create classic tensioned steel dispersion
      for (int a = 0; a < 4; ++a) {
        const int dSamp = juce::jmax(2, (int)std::round(apMs[ch][a] * 0.001 * sampleRate));
        runOfflineAllpass(out, numSamples, dSamp, 0.62f);
      }

      OfflineBiquad::makeHPF(sampleRate, 115.0f, 0.75f).process(out, numSamples);
      OfflineBiquad::makePeak(sampleRate, 3200.0f, 1.1f, 1.8f).process(out, numSamples);
      OfflineBiquad::makeLPF(sampleRate, 10500.0f, 0.707f).process(out, numSamples);

      const int fadeSamples = juce::jmin(numSamples / 4, (int)(0.025 * sampleRate));
      for (int k = 0; k < fadeSamples; ++k) {
        const float w = 0.5f * (1.0f - std::cos(PI * (float)k / (float)fadeSamples));
        out[numSamples - 1 - k] *= w;
      }
    }

    normalizeSampleRateInvariant(buf, sampleRate, 0.55f);
    return buf;
  }

  static juce::AudioBuffer<float> synthesizeWarmCab2x12(double sampleRate) {
    const int numSamples = juce::jmax(128, (int)std::ceil(0.065 * sampleRate)); // 65 ms cab + studio air
    juce::AudioBuffer<float> buf(2, numSamples);
    buf.clear();

    for (int ch = 0; ch < 2; ++ch) {
      float *out = buf.getWritePointer(ch);
      const double micOffsetMs = (ch == 0) ? 0.0 : 0.11; // subtle dual-mic stereo width

      // Dual 12-inch speaker cones + baffle diffraction + open-back rear bounce
      addSoftenedTap(out, numSamples, (0.08 + micOffsetMs) * 0.001 * sampleRate,
                     (ch == 0) ? 1.00f : 0.94f, 0);
      addSoftenedTap(out, numSamples, (0.23 + micOffsetMs * 1.4) * 0.001 * sampleRate,
                     (ch == 0) ? 0.44f : 0.52f, 1);
      addSoftenedTap(out, numSamples, (0.52 + micOffsetMs) * 0.001 * sampleRate, -0.22f, 1);
      addSoftenedTap(out, numSamples, (1.18 + micOffsetMs) * 0.001 * sampleRate, 0.15f, 1);
      addSoftenedTap(out, numSamples, ((ch == 0 ? 3.25 : 3.48)) * 0.001 * sampleRate, -0.26f, 3);
      addSoftenedTap(out, numSamples, ((ch == 0 ? 5.90 : 6.25)) * 0.001 * sampleRate, 0.09f, 4);

      // Subtle tight tracking booth floor/wall reflections (-24 dB) for natural stereo feel
      FastRng rng(0x32783132434142ULL + (juce::uint64)(ch * 7919ULL));
      for (int r = 0; r < 14; ++r) {
        const double tMs = 8.5 + (double)r * 2.6 + (double)rng.nextFloat01() * 1.4;
        const float env = 0.045f * std::exp(-(float)tMs / 16.0f) *
                          ((rng.nextU64() & 1ULL) ? 1.0f : -1.0f);
        addSoftenedTap(out, numSamples, tMs * 0.001 * sampleRate, env, 3);
      }

      // 2x12 open-back speaker enclosure & paper-cone resonance cascade
      OfflineBiquad::makeHPF(sampleRate, 82.0f, 0.88f).process(out, numSamples);
      OfflineBiquad::makePeak(sampleRate, 118.0f, 1.55f, 4.5f).process(out, numSamples);
      OfflineBiquad::makePeak(sampleRate, 420.0f, 1.30f, -4.0f).process(out, numSamples);
      OfflineBiquad::makePeak(sampleRate, 1380.0f, 1.20f, 2.0f).process(out, numSamples);
      OfflineBiquad::makePeak(sampleRate, 2450.0f, 1.55f, 3.8f).process(out, numSamples);
      OfflineBiquad::makePeak(sampleRate, 4150.0f, 2.60f, -2.5f).process(out, numSamples);
      OfflineBiquad::makeLPF(sampleRate, 4850.0f, 0.85f).process(out, numSamples);
      OfflineBiquad::makeLPF(sampleRate, 5600.0f, 0.707f).process(out, numSamples);

      const int fadeSamples = juce::jmin(numSamples / 3, (int)(0.012 * sampleRate));
      for (int k = 0; k < fadeSamples; ++k) {
        const float w = 0.5f * (1.0f - std::cos(PI * (float)k / (float)fadeSamples));
        out[numSamples - 1 - k] *= w;
      }
    }

    normalizeSampleRateInvariant(buf, sampleRate, 0.44f);
    return buf;
  }

  static juce::AudioBuffer<float> synthesizePresetBuffer(int presetIdx,
                                                         double sampleRate) {
    switch (presetIdx) {
    case 0: // Studio Wood Room (0.55s)
      return synthesizeSpace(
          {7.4f, 5.3f, 3.2f, 0.79f, 0.62f, 0.55f, 0.28f, 0.006f, 0.014f, 75.0f,
           7800.0f, 0.34f, 0.22f, 0.55f, 0x574F4F44524F4F4DULL},
          sampleRate);
    case 1: // Warm Studio Chamber (1.05s)
      return synthesizeSpace(
          {9.8f, 6.9f, 3.9f, 0.84f, 1.15f, 1.05f, 0.48f, 0.011f, 0.024f, 85.0f,
           6200.0f, 0.30f, 0.26f, 0.55f, 0x4348414D42455231ULL},
          sampleRate);
    case 2: // Symphony Concert Hall (2.1s)
      return synthesizeSpace(
          {24.5f, 16.8f, 12.2f, 0.89f, 2.35f, 2.10f, 0.92f, 0.018f, 0.046f,
           80.0f, 7000.0f, 0.26f, 0.28f, 0.55f, 0x434F4E4348414C4CULL},
          sampleRate);
    case 3: // Stone Sanctuary (3.0s)
      return synthesizeSpace(
          {38.0f, 21.5f, 18.5f, 0.92f, 3.30f, 3.00f, 1.15f, 0.026f, 0.072f,
           90.0f, 5800.0f, 0.22f, 0.30f, 0.54f, 0x434154484544524CULL},
          sampleRate);
    case 4: // Vintage Studio Plate (1.5s)
      return synthesizePlate(sampleRate);
    case 5: // 2x12 Warm Tube Cab
      return synthesizeWarmCab2x12(sampleRate);
    default:
      return {};
    }
  }

  juce::dsp::Convolution convolution;
  juce::AudioBuffer<float> wetBuffer;
  double sr = 48000.0;
  double synthesizedSr = 0.0;
  int blockSize = 512;
  bool hasIR = false;
  juce::String loadedPath;
};

// ==============================================================================
// MAIN PROCESSOR WRAPPER
// ==============================================================================
class ChannelStripProcessor {
public:
  ChannelStripProcessor() = default;

  SimpleGate gateL, gateR;
  SimpleEQ eqL, eqR;
  SimpleComp compL, compR;
  SimpleChorus chorusL, chorusR;
  StereoDelay delay;
  juce::Reverb reverb;
  ConvolutionReverb irReverb;

  // Parameters (Atomic for thread-safety)
  std::atomic<bool> gateEnabled{false};
  std::atomic<float> gateThreshold{-60.0f};

  std::atomic<bool> eqEnabled{false};
  std::atomic<bool> eqCurveMode{false}; // false = Independent, true = Proportional Curve Link
  std::atomic<float> hpfFreq{20.0f};
  std::atomic<float> lowShelfGain{0.0f};
  std::atomic<float> highShelfGain{0.0f};
  std::atomic<float> eqVolGain{0.0f};
  std::atomic<float> eqMasterGain{0.0f};
  std::atomic<float> eqBands[10]{};

  std::atomic<bool> compEnabled{false};
  std::atomic<float> compAmount{0.0f};
  std::atomic<float> compMakeupDb{0.0f};
  std::atomic<int> compPreset{0};

  std::atomic<bool> chorusEnabled{false};
  std::atomic<float> chorusRate{1.0f};
  std::atomic<float> chorusMix{0.0f};

  // Stereo / Ping-Pong / Mono / Tape Delay
  std::atomic<bool> delayEnabled{false};
  std::atomic<int> delayMode{0}; // 0 = Stereo, 1 = Ping-Pong, 2 = Mono, 3 = Tape Warm
  std::atomic<float> delayTimeMs{375.0f};
  std::atomic<float> delayFeedback{0.35f};
  std::atomic<float> delayTone{6500.0f};
  std::atomic<float> delayMix{0.25f};

  std::atomic<bool> reverbEnabled{false};
  std::atomic<float> reverbSize{0.5f};
  std::atomic<float> reverbMix{0.0f};

  // IR / convolution reverb
  std::atomic<bool> irEnabled{false};
  std::atomic<float> irMix{0.3f};

  // Musette Detuner (Wet MM/MMM accordion emulation)
  MusetteDetuner musette;
  std::atomic<bool> musetteEnabled{false};
  std::atomic<float> musetteCents{6.0f};
  std::atomic<int> musetteMode{0}; // 0 = 2-Reed MM, 1 = 3-Reed MMM, 2 = TEMU Musette
  std::atomic<bool> musette3Reed{false};
  std::atomic<float> musetteMix{0.25f};

  // Input Channel Mode: 0 = Stereo, 1 = Mono (L+R), 2 = Mono (Left), 3 = Mono (Right)
  std::atomic<int> channelMode{0};

  // Cached parameter state to prevent per-block coefficient recomputation
  bool cachedGateEnabled = false;
  float cachedGateThresh = -999.0f;
  bool cachedEqEnabled = false;
  float cachedVolGain = -999.0f;
  float cachedMasterGain = -999.0f;
  float cachedEqBands[10] = {-999.0f, -999.0f, -999.0f, -999.0f, -999.0f, -999.0f, -999.0f, -999.0f, -999.0f, -999.0f};
  bool cachedCompEnabled = false;
  float cachedCompAmt = -999.0f;
  float cachedCompMakeup = -999.0f;
  int cachedCompPreset = -1;
  bool cachedChorusEnabled = false;
  float cachedChorusRate = -999.0f;
  float cachedChorusMix = -999.0f;

  ConvolutionReverb &getIRReverb() { return irReverb; }
  const ConvolutionReverb &getIRReverb() const { return irReverb; }

  void prepare(double sampleRate) {
    gateL.prepare(sampleRate);
    gateR.prepare(sampleRate);
    eqL.prepare(sampleRate);
    eqR.prepare(sampleRate);
    compL.prepare(sampleRate);
    compR.prepare(sampleRate);
    chorusL.prepare(sampleRate);
    chorusR.prepare(sampleRate);
    delay.prepare(sampleRate);
    reverb.setSampleRate(sampleRate);
    irReverb.prepare(sampleRate, 4096);
    musette.prepare(sampleRate);
  }

  void processBlock(juce::AudioBuffer<float> &buffer) {
    bool gEnabled = gateEnabled.load();
    float gThresh = gateThreshold.load();
    bool eEnabled = eqEnabled.load();
    float vGain = eqVolGain.load();
    float mGain = eqMasterGain.load();
    bool cEnabled = compEnabled.load();
    float cAmt = compAmount.load();
    float cMakeup = compMakeupDb.load();
    int cPreset = compPreset.load();
    
    bool choEnabled = chorusEnabled.load();
    float choRate = chorusRate.load();
    float choMix = chorusMix.load();

    bool delEnabled = delayEnabled.load(std::memory_order_relaxed);
    int delMode = delayMode.load(std::memory_order_relaxed);
    float delTime = delayTimeMs.load(std::memory_order_relaxed);
    float delFb = delayFeedback.load(std::memory_order_relaxed);
    float delTone = delayTone.load(std::memory_order_relaxed);
    float delMix = delayMix.load(std::memory_order_relaxed);
    delay.setParameters(delEnabled, delMode, delTime, delFb, delTone, delMix);

    bool musEnabled = musetteEnabled.load();
    float musCents = musetteCents.load();
    int musMode = musetteMode.load();
    float musMix = musetteMix.load();

    if (gEnabled != cachedGateEnabled || gThresh != cachedGateThresh) {
      cachedGateEnabled = gEnabled;
      cachedGateThresh = gThresh;
      gateL.setEnabled(gEnabled);
      gateL.setThreshold(gThresh);
      gateR.setEnabled(gEnabled);
      gateR.setThreshold(gThresh);
    }

    if (eEnabled != cachedEqEnabled || vGain != cachedVolGain || mGain != cachedMasterGain) {
      cachedEqEnabled = eEnabled;
      cachedVolGain = vGain;
      cachedMasterGain = mGain;
      eqL.setEnabled(eEnabled);
      eqL.setVolGain(vGain);
      eqL.setMasterGain(mGain);
      eqR.setEnabled(eEnabled);
      eqR.setVolGain(vGain);
      eqR.setMasterGain(mGain);
    }

    for (int b = 0; b < 10; ++b) {
      float bg = eqBands[b].load();
      if (bg != cachedEqBands[b]) {
        cachedEqBands[b] = bg;
        eqL.setBandGain(b, bg);
        eqR.setBandGain(b, bg);
      }
    }

    if (cEnabled != cachedCompEnabled || cAmt != cachedCompAmt || cMakeup != cachedCompMakeup || cPreset != cachedCompPreset) {
      cachedCompEnabled = cEnabled;
      cachedCompAmt = cAmt;
      cachedCompMakeup = cMakeup;
      cachedCompPreset = cPreset;
      compL.setEnabled(cEnabled);
      compL.setPreset(cPreset, cMakeup);
      compL.setAmount(cAmt);
      compR.setEnabled(cEnabled);
      compR.setPreset(cPreset, cMakeup);
      compR.setAmount(cAmt);
    }

    if (choEnabled != cachedChorusEnabled || choRate != cachedChorusRate || choMix != cachedChorusMix) {
      cachedChorusEnabled = choEnabled;
      cachedChorusRate = choRate;
      cachedChorusMix = choMix;
      chorusL.setEnabled(choEnabled);
      chorusL.setRate(choRate);
      chorusL.setMix(choMix);
      chorusR.setEnabled(choEnabled);
      chorusR.setRate(choRate);
      chorusR.setMix(choMix);
    }

    int numSamples = buffer.getNumSamples();
    auto *L = buffer.getWritePointer(0);
    auto *R = buffer.getNumChannels() > 1 ? buffer.getWritePointer(1) : nullptr;

    if (R != nullptr) {
      int mode = channelMode.load(std::memory_order_relaxed);
      if (mode == 1) { // Mono (L+R)
        for (int i = 0; i < numSamples; ++i) {
          float m = 0.5f * (L[i] + R[i]);
          L[i] = m;
          R[i] = m;
        }
      } else if (mode == 2) { // Mono (Left)
        for (int i = 0; i < numSamples; ++i) {
          R[i] = L[i];
        }
      } else if (mode == 3) { // Mono (Right)
        for (int i = 0; i < numSamples; ++i) {
          L[i] = R[i];
        }
      }
    }

    for (int i = 0; i < numSamples; ++i) {
      L[i] = gateL.process(L[i]);
      if (R)
        R[i] = gateR.process(R[i]);

      L[i] = eqL.process(L[i]);
      if (R)
        R[i] = eqR.process(R[i]);

      L[i] = compL.process(L[i]);
      if (R)
        R[i] = compR.process(R[i]);

      if (musEnabled && musMix > 0.0001f) {
        if (R)
          musette.process(L[i], R[i], true, musCents, musMode, musMix);
        else
          musette.processMono(L[i], musCents, musMode, musMix);
      }

      L[i] = chorusL.process(L[i], 0.0f);
      if (R)
        R[i] = chorusR.process(R[i], PI * 0.5f);

      if (delEnabled && delMix > 0.001f) {
        if (R)
          delay.processStereo(L[i], R[i]);
        else
          delay.processMono(L[i]);
      }
    }

    if (reverbEnabled.load() && reverbMix.load() > 0.001f) {
      juce::Reverb::Parameters rp;
      rp.roomSize = reverbSize.load();
      rp.wetLevel = reverbMix.load() * 0.5f;
      rp.dryLevel = 1.0f - reverbMix.load() * 0.3f;
      rp.width = 1.0f;
      rp.damping = 0.4f;
      reverb.setParameters(rp);

      if (R) {
        reverb.processStereo(L, R, numSamples);
      } else {
        reverb.processMono(L, numSamples);
      }
    }

    // IR / convolution reverb (impulse-response cab/room sim)
    irReverb.processBlock(buffer, irEnabled.load(), irMix.load());
  }

  float getGainReductionDb() const { return compL.getGainReductionDb(); }
  SimpleComp &getCompReference() { return compL; }

private:
  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ChannelStripProcessor);
};

} // namespace FanfareDSP
