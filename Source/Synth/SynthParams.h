#pragma once

#include <JuceHeader.h>
#include <array>
#include <cmath>
#include "SynthConstants.h"

namespace FanfareSynth {

enum ParamId : int {
  o1Wave = 0,
  o1Oct,
  o1Detune,
  o1Level,
  o2Wave,
  o2Oct,
  o2Semi,
  o2Detune,
  o2Level,
  pwmSource,
  pwmManual,
  pwmRate,
  pwmDepth,
  subWave,
  subLevel,
  noiseLevel,
  lfoToPitch,

  hpfFreq,

  cutoff,
  reso,
  filtEnvAmt,
  filtEnvPol,
  lfoToFilt,
  keyTrack,
  velToFilt,
  filtEnvSrc,
  filtSlope,

  vcaMode,
  velToAmp,
  lfoToAmp,

  atk,
  dec,
  sus,
  rel,

  fatk,
  fdec,
  fsus,
  frel,

  lfoRate,
  lfoDelay,
  lfoWave,
  lfoTrig,

  volume,
  voiceMode,
  glideMode,
  glideTime,
  unison,
  uniDetune,
  uniWidth,
  bendRange,
  vintage,

  choMode,
  choRate,
  choDepth,
  choMix,

  dlyTime,
  dlyFb,
  dlyDamp,
  dlyMix,

  revType,
  revSize,
  revDamp,
  revMix,

  kNumParams
};

enum SectionId : int {
  SecControl = 0,
  SecLfo,
  SecDco,
  SecHpf,
  SecVcf,
  SecVca,
  SecAmpEnv,
  SecFltEnv,
  SecChorus,
  SecDelay,
  SecReverb,
  kNumSections
};

enum ParamKind : int {
  KindContinuous = 0,
  KindEnum = 1
};

struct ParamDef {
  const char *id;
  const char *shortLabel;
  const char *label;
  int section;
  int kind;
  float min;
  float max;
  float def;
  float skew;
  float step;
  float wheelNormStep;
  int defaultCC;
  const char *choices;
  const char *unit;
  bool isPercent;
  bool bipolar;
};

inline const std::array<ParamDef, kNumParams> &paramDefs() {
  using DspConfig::kUnassignedCC;
  static const std::array<ParamDef, kNumParams> defs = {{
      // id            short     full label            section     kind            min     max      def     skew  step  wheel   cc             choices                 unit  pct    bip
      {"o1Wave",     "WAVE",   "OSC 1 Waveform",     SecDco,     KindEnum,       0.0f,   3.0f,    0.0f,   1.0f, 1.0f, 0.25f,  kUnassignedCC, "SAW,PUL,TRI,S+P",      "",   false, false},
      {"o1Oct",      "RANGE",  "OSC 1 Range",        SecDco,     KindEnum,      -2.0f,   2.0f,    0.0f,   1.0f, 1.0f, 0.25f,  kUnassignedCC, "32',16',8',4',2'",     "",   false, false},
      {"o1Detune",   "FINE",   "OSC 1 Fine Tune",    SecDco,     KindContinuous,-50.0f,  50.0f,   0.0f,   1.0f, 0.0f, 0.01f,  kUnassignedCC, nullptr,                "ct", false, true},
      {"o1Level",    "OSC 1",  "OSC 1 Level",        SecDco,     KindContinuous, 0.0f,   1.0f,    0.85f,  1.0f, 0.0f, 0.01f,  kUnassignedCC, nullptr,                "",   true,  false},

      {"o2Wave",     "WAVE",   "OSC 2 Waveform",     SecDco,     KindEnum,       0.0f,   3.0f,    0.0f,   1.0f, 1.0f, 0.25f,  kUnassignedCC, "SAW,PUL,TRI,S+P",      "",   false, false},
      {"o2Oct",      "RANGE",  "OSC 2 Range",        SecDco,     KindEnum,      -2.0f,   2.0f,    0.0f,   1.0f, 1.0f, 0.25f,  kUnassignedCC, "32',16',8',4',2'",     "",   false, false},
      {"o2Semi",     "SEMI",   "OSC 2 Semitone",     SecDco,     KindContinuous,-12.0f,  12.0f,   0.0f,   1.0f, 1.0f, 0.0417f,kUnassignedCC, nullptr,                "st", false, true},
      {"o2Detune",   "FINE",   "OSC 2 Fine Tune",    SecDco,     KindContinuous,-50.0f,  50.0f,   0.0f,   1.0f, 0.0f, 0.01f,  kUnassignedCC, nullptr,                "ct", false, true},
      {"o2Level",    "OSC 2",  "OSC 2 Level",        SecDco,     KindContinuous, 0.0f,   1.0f,    0.0f,   1.0f, 0.0f, 0.01f,  kUnassignedCC, nullptr,                "",   true,  false},

      {"pwmSource",  "PWM SRC","PWM Mod Source",     SecDco,     KindEnum,       0.0f,   2.0f,    0.0f,   1.0f, 1.0f, 0.5f,   kUnassignedCC, "MAN,LFO,RATE",         "",   false, false},
      {"pwmManual",  "PW",     "Pulse Width",        SecDco,     KindContinuous, 0.05f,  0.95f,   0.50f,  1.0f, 0.0f, 0.01f,  kUnassignedCC, nullptr,                "",   true,  true},
      {"pwmRate",    "PWM RT", "PWM Mod Rate",       SecDco,     KindContinuous, 0.05f,  20.0f,   0.80f,  0.45f,0.0f, 0.015f, kUnassignedCC, nullptr,                "Hz", false, false},
      {"pwmDepth",   "PWM",    "PWM Mod Depth",      SecDco,     KindContinuous, 0.0f,   1.0f,    0.0f,   1.0f, 0.0f, 0.01f,  kUnassignedCC, nullptr,                "",   true,  false},

      {"subWave",    "SUB OCT","Sub Osc Mode",       SecDco,     KindEnum,       0.0f,   2.0f,    0.0f,   1.0f, 1.0f, 0.5f,   kUnassignedCC, "-1SQ,-2SQ,-2PL",       "",   false, false},
      {"subLevel",   "SUB",    "Sub Osc Level",      SecDco,     KindContinuous, 0.0f,   1.0f,    0.0f,   1.0f, 0.0f, 0.01f,  kUnassignedCC, nullptr,                "",   true,  false},
      {"noiseLevel", "NOISE",  "Noise Level",        SecDco,     KindContinuous, 0.0f,   1.0f,    0.0f,   1.0f, 0.0f, 0.01f,  kUnassignedCC, nullptr,                "",   true,  false},
      {"lfoToPitch", "LFO",    "LFO to Pitch",       SecDco,     KindContinuous, 0.0f,   100.0f,  0.0f,   0.65f,0.0f, 0.01f,  kUnassignedCC, nullptr,                "ct", false, false},

      {"hpfFreq",    "FREQ",   "HPF / Bass Boost",   SecHpf,     KindContinuous, 0.0f,   1.0f,    0.12f,  1.0f, 0.0f, 0.015f, kUnassignedCC, nullptr,                "hpf",false, false},

      {"cutoff",     "FREQ",   "VCF Cutoff",         SecVcf,     KindContinuous, 20.0f,  18000.0f,9000.0f,0.24f,0.0f, 0.01f,  74,            nullptr,                "Hz", false, false},
      {"reso",       "RES",    "VCF Resonance",      SecVcf,     KindContinuous, 0.0f,   1.0f,    0.12f,  1.0f, 0.0f, 0.01f,  71,            nullptr,                "",   true,  false},
      {"filtEnvAmt", "ENV",    "VCF Env Depth",      SecVcf,     KindContinuous, 0.0f,   1.0f,    0.35f,  1.0f, 0.0f, 0.01f,  kUnassignedCC, nullptr,                "",   true,  false},
      {"filtEnvPol", "POL",    "VCF Env Polarity",   SecVcf,     KindEnum,       0.0f,   1.0f,    0.0f,   1.0f, 1.0f, 1.0f,   kUnassignedCC, "+,-",                  "",   false, false},
      {"lfoToFilt",  "LFO",    "LFO to VCF",         SecVcf,     KindContinuous, 0.0f,   1.0f,    0.0f,   1.0f, 0.0f, 0.01f,  kUnassignedCC, nullptr,                "",   true,  false},
      {"keyTrack",   "KYBD",   "VCF Key Follow",     SecVcf,     KindContinuous, 0.0f,   1.0f,    0.25f,  1.0f, 0.0f, 0.01f,  kUnassignedCC, nullptr,                "",   true,  false},
      {"velToFilt",  "VEL",    "Velocity to VCF",    SecVcf,     KindContinuous, 0.0f,   1.0f,    0.30f,  1.0f, 0.0f, 0.01f,  kUnassignedCC, nullptr,                "",   true,  false},
      {"filtEnvSrc", "ENV SRC","VCF Env Source",     SecVcf,     KindEnum,       0.0f,   1.0f,    0.0f,   1.0f, 1.0f, 1.0f,   kUnassignedCC, "FENV,AENV",            "",   false, false},
      {"filtSlope",  "SLOPE",  "VCF Slope",          SecVcf,     KindEnum,       0.0f,   1.0f,    1.0f,   1.0f, 1.0f, 1.0f,   kUnassignedCC, "12dB,24dB",            "",   false, false},

      {"vcaMode",    "MODE",   "VCA Mode",           SecVca,     KindEnum,       0.0f,   1.0f,    0.0f,   1.0f, 1.0f, 1.0f,   kUnassignedCC, "ENV,GATE",             "",   false, false},
      {"velToAmp",   "VEL",    "Velocity to VCA",    SecVca,     KindContinuous, 0.0f,   1.0f,    0.35f,  1.0f, 0.0f, 0.01f,  kUnassignedCC, nullptr,                "",   true,  false},
      {"lfoToAmp",   "LFO",    "LFO to VCA",         SecVca,     KindContinuous, 0.0f,   1.0f,    0.0f,   1.0f, 0.0f, 0.01f,  kUnassignedCC, nullptr,                "",   true,  false},

      {"atk",        "A",      "Amp Attack",         SecAmpEnv,  KindContinuous, 0.001f, 10.0f,   0.008f, 0.35f,0.0f, 0.012f, 73,            nullptr,                "s",  false, false},
      {"dec",        "D",      "Amp Decay",          SecAmpEnv,  KindContinuous, 0.005f, 10.0f,   0.30f,  0.38f,0.0f, 0.012f, kUnassignedCC, nullptr,                "s",  false, false},
      {"sus",        "S",      "Amp Sustain",        SecAmpEnv,  KindContinuous, 0.0f,   1.0f,    0.80f,  1.0f, 0.0f, 0.01f,  kUnassignedCC, nullptr,                "",   true,  false},
      {"rel",        "R",      "Amp Release",        SecAmpEnv,  KindContinuous, 0.005f, 16.0f,   0.35f,  0.38f,0.0f, 0.012f, 72,            nullptr,                "s",  false, false},

      {"fatk",       "A",      "Filter Attack",      SecFltEnv,  KindContinuous, 0.001f, 10.0f,   0.008f, 0.35f,0.0f, 0.012f, kUnassignedCC, nullptr,                "s",  false, false},
      {"fdec",       "D",      "Filter Decay",       SecFltEnv,  KindContinuous, 0.005f, 10.0f,   0.35f,  0.38f,0.0f, 0.012f, kUnassignedCC, nullptr,                "s",  false, false},
      {"fsus",       "S",      "Filter Sustain",     SecFltEnv,  KindContinuous, 0.0f,   1.0f,    0.25f,  1.0f, 0.0f, 0.01f,  kUnassignedCC, nullptr,                "",   true,  false},
      {"frel",       "R",      "Filter Release",     SecFltEnv,  KindContinuous, 0.005f, 16.0f,   0.40f,  0.38f,0.0f, 0.012f, kUnassignedCC, nullptr,                "s",  false, false},

      {"lfoRate",    "RATE",   "LFO Rate",           SecLfo,     KindContinuous, 0.05f,  25.0f,   4.5f,   0.45f,0.0f, 0.012f, kUnassignedCC, nullptr,                "Hz", false, false},
      {"lfoDelay",   "DELAY",  "LFO Delay Time",     SecLfo,     KindContinuous, 0.0f,   5.0f,    0.0f,   0.55f,0.0f, 0.015f, kUnassignedCC, nullptr,                "s",  false, false},
      {"lfoWave",    "WAVE",   "LFO Waveform",       SecLfo,     KindEnum,       0.0f,   3.0f,    0.0f,   1.0f, 1.0f, 0.34f,  kUnassignedCC, "TRI,SQR,S&H,SIN",      "",   false, false},
      {"lfoTrig",    "TRIG",   "LFO Trigger Mode",   SecLfo,     KindEnum,       0.0f,   1.0f,    0.0f,   1.0f, 1.0f, 1.0f,   kUnassignedCC, "FREE,KEY",             "",   false, false},

      {"volume",     "VOL",    "Master Output",      SecControl, KindContinuous, 0.0f,   1.0f,    0.80f,  1.0f, 0.0f, 0.01f,  7,             nullptr,                "",   true,  false},
      {"voiceMode",  "ASSIGN", "Voice Assign Mode",  SecControl, KindEnum,       0.0f,   2.0f,    0.0f,   1.0f, 1.0f, 0.5f,   kUnassignedCC, "POLY,MONO,LEG",        "",   false, false},
      {"glideMode",  "PORTA",  "Portamento Mode",    SecControl, KindEnum,       0.0f,   2.0f,    0.0f,   1.0f, 1.0f, 0.5f,   kUnassignedCC, "OFF,I,II",             "",   false, false},
      {"glideTime",  "GLIDE",  "Portamento Time",    SecControl, KindContinuous, 0.005f, 2.0f,    0.10f,  0.48f,0.0f, 0.015f, kUnassignedCC, nullptr,                "s",  false, false},
      {"unison",     "UNISON", "Unison Stack",       SecControl, KindEnum,       0.0f,   3.0f,    0.0f,   1.0f, 1.0f, 0.34f,  kUnassignedCC, "OFF,2,4,6",            "",   false, false},
      {"uniDetune",  "DETUNE", "Unison Detune",      SecControl, KindContinuous, 0.0f,   50.0f,   12.0f,  1.0f, 0.0f, 0.01f,  kUnassignedCC, nullptr,                "ct", false, false},
      {"uniWidth",   "WIDTH",  "Unison Stereo",      SecControl, KindContinuous, 0.0f,   1.0f,    0.65f,  1.0f, 0.0f, 0.01f,  kUnassignedCC, nullptr,                "",   true,  false},
      {"bendRange",  "BEND",   "Pitch Bend Range",   SecControl, KindContinuous, 0.0f,   12.0f,   2.0f,   1.0f, 1.0f, 0.0834f,kUnassignedCC, nullptr,                "st", false, false},
      {"vintage",    "DRIFT",  "Analog Voice Drift", SecControl, KindContinuous, 0.0f,   1.0f,    0.15f,  1.0f, 0.0f, 0.01f,  kUnassignedCC, nullptr,                "",   true,  false},

      {"choMode",    "MODE",   "Juno Chorus Mode",   SecChorus,  KindEnum,       0.0f,   4.0f,    0.0f,   1.0f, 1.0f, 0.25f,  kUnassignedCC, "OFF,I,II,I+II,CUST",   "",   false, false},
      {"choRate",    "RATE",   "Chorus Custom Rate", SecChorus,  KindContinuous, 0.10f,  8.0f,    0.65f,  0.50f,0.0f, 0.015f, kUnassignedCC, nullptr,                "Hz", false, false},
      {"choDepth",   "DEPTH",  "Chorus Custom Depth",SecChorus,  KindContinuous, 0.0f,   1.0f,    0.50f,  1.0f, 0.0f, 0.01f,  kUnassignedCC, nullptr,                "",   true,  false},
      {"choMix",     "MIX",    "Chorus Wet Level",   SecChorus,  KindContinuous, 0.0f,   1.0f,    0.50f,  1.0f, 0.0f, 0.01f,  kUnassignedCC, nullptr,                "",   true,  false},

      {"dlyTime",    "TIME",   "Delay Time",         SecDelay,   KindContinuous, 0.02f,  1.50f,   0.32f,  0.55f,0.0f, 0.012f, kUnassignedCC, nullptr,                "s",  false, false},
      {"dlyFb",      "FDBK",   "Delay Feedback",     SecDelay,   KindContinuous, 0.0f,   0.95f,   0.35f,  1.0f, 0.0f, 0.01f,  kUnassignedCC, nullptr,                "",   true,  false},
      {"dlyDamp",    "DAMP",   "Delay HF Damping",   SecDelay,   KindContinuous, 0.0f,   1.0f,    0.35f,  1.0f, 0.0f, 0.01f,  kUnassignedCC, nullptr,                "",   true,  false},
      {"dlyMix",     "MIX",    "Delay Mix",          SecDelay,   KindContinuous, 0.0f,   1.0f,    0.0f,   1.0f, 0.0f, 0.01f,  kUnassignedCC, nullptr,                "",   true,  false},

      {"revType",    "TYPE",   "Reverb Character",   SecReverb,  KindEnum,       0.0f,   2.0f,    0.0f,   1.0f, 1.0f, 0.5f,   kUnassignedCC, "ROOM,HALL,PLATE",      "",   false, false},
      {"revSize",    "SIZE",   "Reverb Size",        SecReverb,  KindContinuous, 0.0f,   1.0f,    0.55f,  1.0f, 0.0f, 0.01f,  kUnassignedCC, nullptr,                "",   true,  false},
      {"revDamp",    "DAMP",   "Reverb Damping",     SecReverb,  KindContinuous, 0.0f,   1.0f,    0.45f,  1.0f, 0.0f, 0.01f,  kUnassignedCC, nullptr,                "",   true,  false},
      {"revMix",     "MIX",    "Reverb Mix",         SecReverb,  KindContinuous, 0.0f,   1.0f,    0.0f,   1.0f, 0.0f, 0.01f,  kUnassignedCC, nullptr,                "",   true,  false},
  }};
  return defs;
}

inline int paramIndexForId(const juce::String &id) {
  const auto &defs = paramDefs();
  for (int i = 0; i < kNumParams; ++i)
    if (id == defs[(size_t)i].id)
      return i;
  return -1;
}

inline float clampAndSnapParam(const ParamDef &d, float rawValue) {
  if (!std::isfinite(rawValue))
    return d.def;
  float v = juce::jlimit(d.min, d.max, rawValue);
  if (d.kind == KindEnum || d.step >= 1.0f)
    v = std::round(v);
  else if (d.step > 0.0f)
    v = d.min + std::round((v - d.min) / d.step) * d.step;
  return juce::jlimit(d.min, d.max, v);
}

inline float valueToNormalized(const ParamDef &d, float value) {
  const float span = d.max - d.min;
  if (span <= 0.0f)
    return 0.0f;
  const float linear = juce::jlimit(0.0f, 1.0f, (value - d.min) / span);
  return (d.skew > 0.0f && std::abs(d.skew - 1.0f) > 1e-4f)
             ? std::pow(linear, d.skew)
             : linear;
}

inline float normalizedToValue(const ParamDef &d, float norm) {
  const float clamped = juce::jlimit(0.0f, 1.0f, norm);
  const float curved =
      (d.skew > 0.0f && std::abs(d.skew - 1.0f) > 1e-4f)
          ? std::pow(clamped, 1.0f / d.skew)
          : clamped;
  return clampAndSnapParam(d, d.min + curved * (d.max - d.min));
}

inline float computeHpfCutoffHz(float hpfNorm) {
  if (hpfNorm <= (float)DspConfig::kHpfBoostShelfEnd)
    return 0.0f;
  const float t = (hpfNorm - (float)DspConfig::kHpfBoostShelfEnd) /
                  (1.0f - (float)DspConfig::kHpfBoostShelfEnd);
  return (float)(DspConfig::kHpfSweepMinHz *
                 std::pow(DspConfig::kHpfSweepMaxHz / DspConfig::kHpfSweepMinHz,
                          (double)juce::jlimit(0.0f, 1.0f, t)));
}

inline juce::String formatParamValue(const ParamDef &d, float v, bool detailed = false) {
  if (d.kind == KindEnum && d.choices != nullptr) {
    auto tokens = juce::StringArray::fromTokens(d.choices, ",", "");
    int idx = juce::jlimit(0, juce::jmax(0, tokens.size() - 1),
                           (int)std::lround(v) - (int)std::lround(d.min));
    return tokens[idx];
  }

  const juce::String unit(d.unit != nullptr ? d.unit : "");
  if (unit == "hpf") {
    if (v < (float)(DspConfig::kHpfBoostShelfEnd * 0.85)) {
      const float boostDb =
          (float)DspConfig::kHpfBoostMaxDb *
          (1.0f - v / (float)DspConfig::kHpfBoostShelfEnd);
      return detailed ? ("Bass +" + juce::String(boostDb, 1) + " dB") : "BOOST";
    }
    if (v <= (float)(DspConfig::kHpfBoostShelfEnd * 1.15))
      return "FLAT";
    const float hz = computeHpfCutoffHz(v);
    return juce::String((int)std::lround(hz)) + (detailed ? " Hz" : "Hz");
  }

  if (d.isPercent) {
    const float pct = v * 100.0f;
    if (detailed && std::abs(pct - std::round(pct)) >= 0.05f)
      return juce::String(pct, 1) + "%";
    return juce::String((int)std::lround(pct)) + "%";
  }

  if (unit == "Hz") {
    if (v >= 1000.0f) {
      if (detailed)
        return juce::String((int)std::lround(v)) + " Hz";
      return juce::String(v / 1000.0f, v >= 10000.0f ? 1 : 2) + "k";
    }
    if (v < 10.0f)
      return juce::String(v, 2) + (detailed ? " Hz" : "Hz");
    if (v < 100.0f && detailed)
      return juce::String(v, 1) + " Hz";
    return juce::String((int)std::lround(v)) + (detailed ? " Hz" : "Hz");
  }

  if (unit == "s") {
    if (v < 0.10f) {
      const float ms = v * 1000.0f;
      return juce::String(ms, ms < 10.0f ? 1 : 0) + (detailed ? " ms" : "ms");
    }
    if (v < 1.0f)
      return juce::String((int)std::lround(v * 1000.0f)) + (detailed ? " ms" : "ms");
    return juce::String(v, 2) + (detailed ? " s" : "s");
  }

  if (unit == "ct") {
    const juce::String sign = v > 0.05f ? "+" : "";
    if (detailed)
      return sign + juce::String(v, 1) + " ct";
    return sign + juce::String((int)std::lround(v)) + "ct";
  }

  if (unit == "st") {
    const int iv = (int)std::lround(v);
    const juce::String sign = (d.bipolar && iv > 0) ? "+" : "";
    return sign + juce::String(iv) + (detailed ? " st" : "st");
  }

  return juce::String(v, detailed ? 2 : 1);
}

inline juce::String formatEditableText(const ParamDef &d, float v) {
  const juce::String unit(d.unit != nullptr ? d.unit : "");
  if (d.isPercent)
    return juce::String(v * 100.0f, 1).trimCharactersAtEnd("0").trimCharactersAtEnd(".");
  if (unit == "hpf") {
    if (v <= (float)DspConfig::kHpfBoostShelfEnd)
      return "0";
    return juce::String((int)std::lround(computeHpfCutoffHz(v)));
  }
  if (unit == "Hz")
    return v < 20.0f ? juce::String(v, 2) : juce::String((int)std::lround(v));
  if (unit == "s")
    return v < 1.0f ? (juce::String(v * 1000.0f, 1) + "ms") : juce::String(v, 2);
  if (unit == "st")
    return juce::String((int)std::lround(v));
  if (unit == "ct")
    return juce::String(v, 1).trimCharactersAtEnd("0").trimCharactersAtEnd(".");
  return juce::String(v, 2);
}

inline bool parseParamText(const ParamDef &d, const juce::String &rawInput,
                           float &outValue) {
  juce::String s = rawInput.trim().toLowerCase();
  if (s.isEmpty())
    return false;

  const juce::String unit(d.unit != nullptr ? d.unit : "");
  if (unit == "hpf") {
    if (s.startsWith("boost") || s == "bst") {
      outValue = 0.0f;
      return true;
    }
    if (s == "flat" || s == "off") {
      outValue = (float)DspConfig::kHpfBoostShelfEnd;
      return true;
    }
  }

  bool hasMillis = s.contains("ms");
  bool hasKilo = !hasMillis && (s.contains("khz") || s.endsWith("k"));
  juce::String numeric = s.retainCharacters("0123456789.+-");
  if (numeric.isEmpty() || numeric == "+" || numeric == "-" || numeric == ".")
    return false;

  double parsed = numeric.getDoubleValue();
  if (!std::isfinite(parsed))
    return false;

  if (hasKilo)
    parsed *= 1000.0;
  if (unit == "s" && hasMillis)
    parsed *= 0.001;

  if (unit == "hpf") {
    if (parsed <= 0.0) {
      outValue = (float)DspConfig::kHpfBoostShelfEnd;
      return true;
    }
    const double clampedHz = juce::jlimit(DspConfig::kHpfSweepMinHz,
                                          DspConfig::kHpfSweepMaxHz, parsed);
    const double t =
        std::log(clampedHz / DspConfig::kHpfSweepMinHz) /
        std::log(DspConfig::kHpfSweepMaxHz / DspConfig::kHpfSweepMinHz);
    outValue = clampAndSnapParam(
        d, (float)(DspConfig::kHpfBoostShelfEnd +
                   t * (1.0 - DspConfig::kHpfBoostShelfEnd)));
    return true;
  }

  if (d.isPercent) {
    if (std::abs(parsed) > 1.0 || s.contains("%") || d.min >= 0.0f)
      parsed *= 0.01;
  }

  outValue = clampAndSnapParam(d, (float)parsed);
  return true;
}

} // namespace FanfareSynth

