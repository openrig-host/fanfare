#pragma once

#include <array>
#include <cmath>
#include <vector>
#include "../FanfareSynthProcessorImpl.h"
#include "../SynthParams.h"

namespace FanfareSynth::Ui {

enum class WidgetKind {
  Fader,
  Knob,
  KnobCompact,
  SwitchStacked,
  SwitchInline,
  SwitchNoLabel
};

struct WidgetSpec {
  ParamId param;
  WidgetKind kind;
  const char *customLabel = nullptr;
  float widthWeight = 1.0f;
};

struct DeckRowSpec {
  std::vector<WidgetSpec> items;
  int preferredHeight = 28;
};

struct SectionLayoutSpec {
  SectionId section;
  const char *title;
  int tier; // 0 = left cheek, 1 = voice strip, 2 = env + fx strip
  float widthWeight;
  std::vector<WidgetSpec> primaryStrip;
  std::vector<DeckRowSpec> deckRows;
};

inline const std::vector<SectionLayoutSpec> &getJunoLayoutSpecs() {
  static const std::vector<SectionLayoutSpec> specs = {
      {SecControl,
       "CONTROL",
       0,
       1.0f,
       {{ParamId::volume, WidgetKind::Fader, "VOL", 1.0f},
        {ParamId::bendRange, WidgetKind::Knob, "BEND", 1.0f},
        {ParamId::vintage, WidgetKind::Knob, "DRIFT", 1.0f}},
       {{{
             {ParamId::voiceMode, WidgetKind::SwitchStacked, "VOICE ASSIGN", 1.0f},
         },
         38},
        {{
             {ParamId::glideMode, WidgetKind::SwitchStacked, "PORTAMENTO", 1.0f},
         },
         38},
        {{
             {ParamId::glideTime, WidgetKind::KnobCompact, "GLIDE", 1.0f},
         },
         30},
        {{
             {ParamId::unison, WidgetKind::SwitchStacked, "UNISON STACK", 1.0f},
         },
         38},
        {{
             {ParamId::uniDetune, WidgetKind::Knob, "DETUNE", 1.0f},
             {ParamId::uniWidth, WidgetKind::Knob, "WIDTH", 1.0f},
         },
         76}}},

      // ===== TIER 1: LFO -> DCO -> HPF -> VCF -> VCA =========================
      {SecLfo,
       "LFO",
       1,
       2.05f,
       {{ParamId::lfoRate, WidgetKind::Fader, "RATE", 1.0f},
        {ParamId::lfoDelay, WidgetKind::Fader, "DELAY", 1.0f}},
       {{{
             {ParamId::lfoWave, WidgetKind::SwitchNoLabel, "", 1.0f},
         },
         26},
        {{
             {ParamId::lfoTrig, WidgetKind::SwitchInline, "TRIG", 1.0f},
         },
         26}}},

      {SecDco,
       "DCO",
       1,
       8.15f,
       {{ParamId::lfoToPitch, WidgetKind::Fader, "LFO", 1.0f},
        {ParamId::pwmManual, WidgetKind::Fader, "PW", 1.0f},
        {ParamId::pwmDepth, WidgetKind::Fader, "PWM", 1.0f},
        {ParamId::pwmRate, WidgetKind::Fader, "PWM RT", 1.0f},
        {ParamId::o1Level, WidgetKind::Fader, "OSC 1", 1.0f},
        {ParamId::o2Level, WidgetKind::Fader, "OSC 2", 1.0f},
        {ParamId::subLevel, WidgetKind::Fader, "SUB", 1.0f},
        {ParamId::noiseLevel, WidgetKind::Fader, "NOISE", 1.0f}},
       {{{
             {ParamId::o1Oct, WidgetKind::SwitchInline, "O1", 2.05f},
             {ParamId::o1Wave, WidgetKind::SwitchNoLabel, "", 1.55f},
             {ParamId::o1Detune, WidgetKind::KnobCompact, "FINE", 1.15f},
         },
         28},
        {{
             {ParamId::o2Oct, WidgetKind::SwitchInline, "O2", 1.95f},
             {ParamId::o2Wave, WidgetKind::SwitchNoLabel, "", 1.45f},
             {ParamId::o2Semi, WidgetKind::KnobCompact, "SEMI", 1.05f},
             {ParamId::o2Detune, WidgetKind::KnobCompact, "FINE", 1.05f},
         },
         28},
        {{
             {ParamId::pwmSource, WidgetKind::SwitchInline, "PWM", 1.0f},
             {ParamId::subWave, WidgetKind::SwitchInline, "SUB", 1.0f},
         },
         26}}},

      {SecHpf,
       "HPF",
       1,
       1.12f,
       {{ParamId::hpfFreq, WidgetKind::Fader, "FREQ", 1.0f}},
       {}},

      {SecVcf,
       "VCF",
       1,
       6.00f,
       {{ParamId::cutoff, WidgetKind::Fader, "FREQ", 1.0f},
        {ParamId::reso, WidgetKind::Fader, "RES", 1.0f},
        {ParamId::filtEnvAmt, WidgetKind::Fader, "ENV", 1.0f},
        {ParamId::lfoToFilt, WidgetKind::Fader, "LFO", 1.0f},
        {ParamId::keyTrack, WidgetKind::Fader, "KYBD", 1.0f},
        {ParamId::velToFilt, WidgetKind::Fader, "VEL", 1.0f}},
       {{{
             {ParamId::filtEnvPol, WidgetKind::SwitchInline, "POL", 1.0f},
             {ParamId::filtSlope, WidgetKind::SwitchInline, "SLOPE", 1.35f},
         },
         26},
        {{
             {ParamId::filtEnvSrc, WidgetKind::SwitchInline, "ENV SRC", 1.0f},
         },
         26}}},

      {SecVca,
       "VCA",
       1,
       2.00f,
       {{ParamId::velToAmp, WidgetKind::Fader, "VEL", 1.0f},
        {ParamId::lfoToAmp, WidgetKind::Fader, "LFO", 1.0f}},
       {{{
             {ParamId::vcaMode, WidgetKind::SwitchNoLabel, "", 1.0f},
         },
         26}}},

      // ===== TIER 2: AMP ENV | FILTER ENV | CHORUS | DELAY | REVERB ==========
      {SecAmpEnv,
       "AMP ENV",
       2,
       3.40f,
       {{ParamId::atk, WidgetKind::Fader, "A", 1.0f},
        {ParamId::dec, WidgetKind::Fader, "D", 1.0f},
        {ParamId::sus, WidgetKind::Fader, "S", 1.0f},
        {ParamId::rel, WidgetKind::Fader, "R", 1.0f}},
       {}},

      {SecFltEnv,
       "FILTER ENV",
       2,
       3.40f,
       {{ParamId::fatk, WidgetKind::Fader, "A", 1.0f},
        {ParamId::fdec, WidgetKind::Fader, "D", 1.0f},
        {ParamId::fsus, WidgetKind::Fader, "S", 1.0f},
        {ParamId::frel, WidgetKind::Fader, "R", 1.0f}},
       {}},

      {SecChorus,
       "CHORUS",
       2,
       3.80f,
       {{ParamId::choRate, WidgetKind::Knob, "RATE", 1.0f},
        {ParamId::choDepth, WidgetKind::Knob, "DEPTH", 1.0f},
        {ParamId::choMix, WidgetKind::Knob, "MIX", 1.0f}},
       {{{
             {ParamId::choMode, WidgetKind::SwitchNoLabel, "", 1.0f},
         },
         26}}},

      {SecDelay,
       "DELAY",
       2,
       4.45f,
       {{ParamId::dlyTime, WidgetKind::Knob, "TIME", 1.0f},
        {ParamId::dlyFb, WidgetKind::Knob, "FDBK", 1.0f},
        {ParamId::dlyDamp, WidgetKind::Knob, "DAMP", 1.0f},
        {ParamId::dlyMix, WidgetKind::Knob, "MIX", 1.0f}},
       {}},

      {SecReverb,
       "REVERB",
       2,
       3.80f,
       {{ParamId::revSize, WidgetKind::Knob, "SIZE", 1.0f},
        {ParamId::revDamp, WidgetKind::Knob, "DAMP", 1.0f},
        {ParamId::revMix, WidgetKind::Knob, "MIX", 1.0f}},
       {{{
             {ParamId::revType, WidgetKind::SwitchNoLabel, "", 1.0f},
         },
         26}}},
  };
  return specs;
}

inline bool verifyLayoutCompleteness() {
  std::array<int, kNumParams> counts{};
  for (const auto &sec : getJunoLayoutSpecs()) {
    for (const auto &w : sec.primaryStrip)
      if ((int)w.param >= 0 && (int)w.param < kNumParams)
        counts[(size_t)w.param]++;
    for (const auto &row : sec.deckRows)
      for (const auto &w : row.items)
        if ((int)w.param >= 0 && (int)w.param < kNumParams)
          counts[(size_t)w.param]++;
  }
  for (int c : counts)
    if (c != 1)
      return false;
  return true;
}

inline bool isParamEffectivelyActive(int paramIdx,
                                     const FanfareSynthProcessor &proc) noexcept {
  const auto hasPulseWave = [](int wave) { return wave == 1 || wave == 3; };
  const bool o1Audible = proc.getParam(ParamId::o1Level) > 0.002f;
  const bool o2Audible = proc.getParam(ParamId::o2Level) > 0.002f;
  const int w1 = (int)std::lround(proc.getParam(ParamId::o1Wave));
  const int w2 = (int)std::lround(proc.getParam(ParamId::o2Wave));
  const bool anyPulseAudible =
      (o1Audible && hasPulseWave(w1)) || (o2Audible && hasPulseWave(w2));
  const int pwmSrc = (int)std::lround(proc.getParam(ParamId::pwmSource));
  const int vMode = (int)std::lround(proc.getParam(ParamId::voiceMode));
  const int uni = (int)std::lround(proc.getParam(ParamId::unison));
  const bool monoCapable = (vMode != 0) || (uni > 0);
  const int glide = (int)std::lround(proc.getParam(ParamId::glideMode));
  const int cho = (int)std::lround(proc.getParam(ParamId::choMode));
  const bool envSrcAmp =
      ((int)std::lround(proc.getParam(ParamId::filtEnvSrc)) == 1);
  const bool gateVca =
      ((int)std::lround(proc.getParam(ParamId::vcaMode)) == 1);

  switch (paramIdx) {
  case ParamId::o1Oct:
  case ParamId::o1Wave:
  case ParamId::o1Detune:
    return o1Audible;
  case ParamId::o2Oct:
  case ParamId::o2Wave:
  case ParamId::o2Semi:
  case ParamId::o2Detune:
    return o2Audible;
  case ParamId::subWave:
    return proc.getParam(ParamId::subLevel) > 0.002f;
  case ParamId::pwmManual:
    return anyPulseAudible;
  case ParamId::pwmDepth:
    return anyPulseAudible && (pwmSrc != 0);
  case ParamId::pwmRate:
    return anyPulseAudible && (pwmSrc == 2);
  case ParamId::atk:
  case ParamId::dec:
  case ParamId::sus:
  case ParamId::rel:
    return !gateVca || envSrcAmp;
  case ParamId::fatk:
  case ParamId::fdec:
  case ParamId::fsus:
  case ParamId::frel:
    return !envSrcAmp && (proc.getParam(ParamId::filtEnvAmt) > 0.002f);
  case ParamId::filtEnvPol:
  case ParamId::filtEnvSrc:
    return proc.getParam(ParamId::filtEnvAmt) > 0.002f;
  case ParamId::glideMode:
    return monoCapable;
  case ParamId::glideTime:
    return monoCapable && (glide > 0);
  case ParamId::uniDetune:
  case ParamId::uniWidth:
    return uni > 0;
  case ParamId::choRate:
  case ParamId::choDepth:
    return cho == 4;
  case ParamId::choMix:
    return cho > 0;
  case ParamId::dlyTime:
  case ParamId::dlyFb:
  case ParamId::dlyDamp:
    return proc.getParam(ParamId::dlyMix) > 0.002f;
  case ParamId::revType:
  case ParamId::revSize:
  case ParamId::revDamp:
    return proc.getParam(ParamId::revMix) > 0.002f;
  default:
    return true;
  }
}

} // namespace FanfareSynth::Ui

