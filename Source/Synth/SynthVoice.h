#pragma once

#include <JuceHeader.h>
#include <array>
#include <cmath>
#include "SynthConstants.h"
#include "SynthParams.h"
#include "dsp/AnalogEnvelope.h"
#include "dsp/BlepOscillator.h"
#include "dsp/OtaLadderFilter.h"
#include "dsp/SynthDspMath.h"

namespace FanfareSynth {

struct ParamSnapshot {
  std::array<float, kNumParams> v{};
  float operator[](int idx) const noexcept { return v[(size_t)idx]; }
};

struct GlobalModulationState {
  double lfoPhase = 0.0;
  float lfoSampleAndHold = 0.0f;
  uint32_t rngState = 0x9E3779B9u;
  double wanderPhase = 0.0;

  inline float nextRandomBipolar() noexcept {
    rngState ^= rngState << 13;
    rngState ^= rngState >> 17;
    rngState ^= rngState << 5;
    return (float)((int32_t)rngState) * (1.0f / 2147483648.0f);
  }

  inline double stepLfo(double phaseInc, int waveShape) noexcept {
    double out = 0.0;
    switch (waveShape) {
    case 0: // Triangle
      out = (lfoPhase < 0.5) ? (4.0 * lfoPhase - 1.0) : (3.0 - 4.0 * lfoPhase);
      break;
    case 1: // Square
      out = (lfoPhase < 0.5) ? 1.0 : -1.0;
      break;
    case 2: // Sample & Hold
      out = (double)lfoSampleAndHold;
      break;
    case 3: // Sine
    default:
      out = std::sin(lfoPhase * juce::MathConstants<double>::twoPi);
      break;
    }

    lfoPhase += phaseInc;
    if (lfoPhase >= 1.0) {
      lfoPhase -= std::floor(lfoPhase);
      if (waveShape == 2)
        lfoSampleAndHold = nextRandomBipolar();
    }
    return out;
  }
};

struct SynthVoiceUnit {
  Dsp::DcoOscillator osc1;
  Dsp::DcoOscillator osc2;
  Dsp::AnalogNoiseGenerator noiseGen;
  Dsp::JunoHpf hpf;
  Dsp::OtaLadderFilter ladder;
  Dsp::AnalogAdsr ampEnv;
  Dsp::AnalogAdsr filtEnv;
  Dsp::GateEnvelope gateEnv;

  bool active = false;
  bool reservedMono = false;
  int voiceCardIndex = 0;
  int midiNote = -1;
  float velocity = 0.0f;
  double baseFreq = DspConfig::kReferencePitchHz;
  float detuneCents = 0.0f;
  float panL = 0.70710678f;
  float panR = 0.70710678f;

  double pwmPhase = 0.0;
  double ageSeconds = 0.0;

  bool gliding = false;
  double glideFreq = DspConfig::kReferencePitchHz;
  double glideTarget = DspConfig::kReferencePitchHz;

  double cardPitchTrimCents = 0.0;
  double cardCutoffTrimOct = 0.0;
  double cardEnvScale = 1.0;

  double lastOutputLevel = 0.0;
  int stealFadeRemaining = 0;
  int stealFadeTotal = 1;

  juce::uint64 startedTick = 0;

  void initCardTolerance(int cardIdx) noexcept {
    voiceCardIndex = cardIdx;
    noiseGen.setSeed((uint32_t)(cardIdx * 747796405u + 2891336453u));
    const double normA = std::sin((double)(cardIdx + 1) * 2.39996323);
    const double normB = std::cos((double)(cardIdx + 1) * 3.88322207);
    const double normC = std::sin((double)(cardIdx + 1) * 5.12345678);
    cardPitchTrimCents = normA * DspConfig::kVintageMaxPitchCents;
    cardCutoffTrimOct = normB * DspConfig::kVintageMaxCutoffOctaves;
    cardEnvScale = 1.0 + normC * DspConfig::kVintageMaxEnvScale;
  }

  void prepare(double sampleRate) noexcept {
    ampEnv.sampleRate = sampleRate;
    filtEnv.sampleRate = sampleRate;
    gateEnv.prepare(sampleRate);
    hardReset();
  }

  void hardReset() noexcept {
    active = false;
    reservedMono = false;
    midiNote = -1;
    osc1.reset(0.0);
    osc2.reset(0.25);
    hpf.reset();
    ladder.reset();
    ampEnv.hardKill();
    filtEnv.hardKill();
    gateEnv.hardKill();
    detuneCents = 0.0f;
    panL = 0.70710678f;
    panR = 0.70710678f;
    ageSeconds = 0.0;
    gliding = false;
    lastOutputLevel = 0.0;
    stealFadeRemaining = 0;
    stealFadeTotal = 1;
  }

  bool isFinished(bool useGateVca) const noexcept {
    if (stealFadeRemaining > 0)
      return false;
    return useGateVca ? gateEnv.isIdle() : ampEnv.isIdle();
  }
};

} // namespace FanfareSynth

