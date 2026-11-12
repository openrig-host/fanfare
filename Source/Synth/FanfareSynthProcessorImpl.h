#pragma once

#include <JuceHeader.h>
#include <array>
#include <atomic>
#include <cmath>
#include <vector>
#include "../LushReverb.h"
#include "SynthConstants.h"
#include "SynthParams.h"
#include "SynthPresets.h"
#include "SynthVoice.h"
#include "dsp/JunoChorus.h"
#include "dsp/PingPongDelay.h"
#include "dsp/SynthDspMath.h"

class FanfareSynthProcessor {
public:
  using ParamId = FanfareSynth::ParamId;
  using SectionId = FanfareSynth::SectionId;
  using ParamDef = FanfareSynth::ParamDef;

  static constexpr int kNumParams = FanfareSynth::kNumParams;
  static constexpr int kNumSections = FanfareSynth::kNumSections;

  static const std::array<ParamDef, kNumParams> &paramDefs() {
    return FanfareSynth::paramDefs();
  }

  static int paramIndexForId(const juce::String &id) {
    return FanfareSynth::paramIndexForId(id);
  }

  static const juce::Array<juce::var> &factoryPresets() {
    return FanfareSynth::SynthPresetBank::factoryPresets();
  }

  FanfareSynthProcessor() {
    synthBuffer.setSize(2, FanfareSynth::DspConfig::kMinBlockSize, false, false, true);
    synthBuffer.clear();
    lfoScratch.assign((size_t)FanfareSynth::DspConfig::kMinBlockSize, 0.0f);
    uiMidiQueue.ensureSize(2048);
    for (int i = 0; i < (int)units.size(); ++i)
      units[(size_t)i].initCardTolerance(i);
    for (int i = 0; i < (int)stealTails.size(); ++i)
      stealTails[(size_t)i].initCardTolerance(i);
    resetAllCCsToDefault();
    applyDefaults();
    setPresetMetadata(FanfareSynth::StateSchema::kDefaultInitPresetName, false);
  }

  void prepare(double sampleRate, int maxBlockSize = 8192) {
    using namespace FanfareSynth;
    currentSampleRate = juce::jmax(DspConfig::kMinSampleRate, sampleRate);
    const int safeBlock = juce::jmax(maxBlockSize, DspConfig::kMinBlockSize);

    synthBuffer.setSize(2, safeBlock, false, false, true);
    synthBuffer.clear();
    lfoScratch.assign((size_t)safeBlock, 0.0f);
    uiMidiQueue.ensureSize(2048);

    chorus.prepare(currentSampleRate);
    delay.prepare(currentSampleRate, getParam(ParamId::dlyTime));
    reverb.prepare(currentSampleRate);

    prepareSmoothers();

    for (auto &u : units)
      u.prepare(currentSampleRate);
    for (auto &t : stealTails)
      t.prepare(currentSampleRate);

    allNotesOff();
  }

  void reset() {
    allNotesOff();
    chorus.reset();
    delay.reset();
    reverb.reset();
    snapSmoothersPending.store(true, std::memory_order_release);
  }

  std::atomic<bool> enabled{false};

  float getParam(int idx) const noexcept {
    return (idx >= 0 && idx < kNumParams)
               ? p[(size_t)idx].load(std::memory_order_relaxed)
               : 0.0f;
  }

  void setParam(int idx, float value, bool markDirty = true) noexcept {
    if (idx < 0 || idx >= kNumParams)
      return;
    const auto &d = paramDefs()[(size_t)idx];
    const float clamped = FanfareSynth::clampAndSnapParam(d, value);
    p[(size_t)idx].store(clamped, std::memory_order_relaxed);
    if (markDirty)
      presetDirty.store(true, std::memory_order_relaxed);
  }

  void applyDefaults() noexcept {
    const auto &defs = paramDefs();
    for (int i = 0; i < kNumParams; ++i)
      p[(size_t)i].store(defs[(size_t)i].def, std::memory_order_relaxed);
    snapSmoothersPending.store(true, std::memory_order_release);
  }

  void applyPreset(const juce::var &preset) {
    applyDefaults();
    auto pObj = preset.getProperty("p", juce::var());
    if (auto *obj = pObj.getDynamicObject()) {
      for (const auto &entry : obj->getProperties()) {
        const int idx = paramIndexForId(entry.name.toString());
        if (idx >= 0)
          setParam(idx, (float)(double)entry.value, false);
      }
    }
    const juce::String name =
        preset
            .getProperty("name",
                         FanfareSynth::StateSchema::kDefaultInitPresetName)
            .toString();
    setPresetMetadata(name, false);
    snapSmoothersPending.store(true, std::memory_order_release);
  }

  juce::var getState() const {
    using namespace FanfareSynth;
    auto *obj = new juce::DynamicObject();
    obj->setProperty(StateSchema::kKeyVersion, StateSchema::kCurrentVersion);
    obj->setProperty(StateSchema::kKeyEnabled, enabled.load());
    obj->setProperty(StateSchema::kKeyPresetName, getCurrentPresetName());
    obj->setProperty(StateSchema::kKeyPresetDirty,
                     presetDirty.load(std::memory_order_relaxed));

    const auto &defs = paramDefs();
    for (int i = 0; i < kNumParams; ++i)
      obj->setProperty(juce::String(defs[(size_t)i].id),
                       (double)p[(size_t)i].load(std::memory_order_relaxed));

    auto *ccObj = new juce::DynamicObject();
    for (int i = 0; i < kNumParams; ++i) {
      const int cc = ccMappings[(size_t)i].load(std::memory_order_relaxed);
      if (cc != defs[(size_t)i].defaultCC)
        ccObj->setProperty(juce::String(defs[(size_t)i].id), cc);
    }
    obj->setProperty(StateSchema::kKeyCcMap, juce::var(ccObj));

    obj->setProperty("ccCutoff", getCCForParamIndex(ParamId::cutoff));
    obj->setProperty("ccReso", getCCForParamIndex(ParamId::reso));
    obj->setProperty("ccAtk", getCCForParamIndex(ParamId::atk));
    obj->setProperty("ccRel", getCCForParamIndex(ParamId::rel));
    obj->setProperty("ccVol", getCCForParamIndex(ParamId::volume));
    return juce::var(obj);
  }

  bool applyState(const juce::var &v, bool preserveEnabledIfMissing = true) {
    using namespace FanfareSynth;
    auto *obj = v.getDynamicObject();
    if (obj == nullptr)
      return false;

    const int version = obj->hasProperty(StateSchema::kKeyVersion)
                            ? (int)obj->getProperty(StateSchema::kKeyVersion)
                            : 1;

    if (obj->hasProperty(StateSchema::kKeyEnabled))
      enabled.store((bool)obj->getProperty(StateSchema::kKeyEnabled));
    else if (!preserveEnabledIfMissing)
      enabled.store(false);

    applyDefaults();
    resetAllCCsToDefault();

    const auto &defs = paramDefs();
    for (int i = 0; i < kNumParams; ++i) {
      if (obj->hasProperty(defs[(size_t)i].id))
        setParam(i, (float)(double)obj->getProperty(defs[(size_t)i].id), false);
    }

    if (version < 2)
      migrateVersion1State(*obj);

    auto readLegacyCC = [&](const char *key, int paramIdx) {
      if (obj->hasProperty(key))
        setCCForParamIndex(paramIdx, (int)obj->getProperty(key));
    };
    readLegacyCC("ccCutoff", ParamId::cutoff);
    readLegacyCC("ccReso", ParamId::reso);
    readLegacyCC("ccAtk", ParamId::atk);
    readLegacyCC("ccRel", ParamId::rel);
    readLegacyCC("ccVol", ParamId::volume);

    if (auto *ccObj =
            obj->getProperty(StateSchema::kKeyCcMap).getDynamicObject()) {
      for (const auto &entry : ccObj->getProperties()) {
        const int idx = paramIndexForId(entry.name.toString());
        if (idx >= 0)
          setCCForParamIndex(idx, (int)entry.value);
      }
    }

    const juce::String loadedName =
        obj->hasProperty(StateSchema::kKeyPresetName)
            ? obj->getProperty(StateSchema::kKeyPresetName).toString()
            : juce::String(StateSchema::kDefaultSlotPatchName);
    const bool loadedDirty =
        obj->hasProperty(StateSchema::kKeyPresetDirty)
            ? (bool)obj->getProperty(StateSchema::kKeyPresetDirty)
            : !obj->hasProperty(StateSchema::kKeyPresetName);
    setPresetMetadata(loadedName, loadedDirty);
    snapSmoothersPending.store(true, std::memory_order_release);
    return true;
  }

  // ===== MIDI CC mapping (all parameters learnable) ==========================

  int getCCForParamIndex(int idx) const noexcept {
    if (idx < 0 || idx >= kNumParams)
      return FanfareSynth::DspConfig::kUnassignedCC;
    return ccMappings[(size_t)idx].load(std::memory_order_relaxed);
  }

  void setCCForParamIndex(int idx, int cc) noexcept {
    if (idx < 0 || idx >= kNumParams)
      return;
    const int clamped = juce::jlimit(-1, 127, cc);
    if (clamped >= 0) {
      for (int i = 0; i < kNumParams; ++i)
        if (i != idx &&
            ccMappings[(size_t)i].load(std::memory_order_relaxed) == clamped)
          ccMappings[(size_t)i].store(-1, std::memory_order_relaxed);
    }
    ccMappings[(size_t)idx].store(clamped, std::memory_order_relaxed);
  }

  void resetCCToDefault(int idx) noexcept {
    if (idx < 0 || idx >= kNumParams)
      return;
    setCCForParamIndex(idx, paramDefs()[(size_t)idx].defaultCC);
  }

  void resetAllCCsToDefault() noexcept {
    const auto &defs = paramDefs();
    for (int i = 0; i < kNumParams; ++i)
      ccMappings[(size_t)i].store(defs[(size_t)i].defaultCC,
                                  std::memory_order_relaxed);
  }

  void setCCForParam(const juce::String &paramId, int cc) noexcept {
    setCCForParamIndex(paramIndexForId(paramId), cc);
  }

  int getCCForParam(const juce::String &paramId) const noexcept {
    return getCCForParamIndex(paramIndexForId(paramId));
  }

  // ===== Patch metadata & live telemetry =====================================

  juce::String getCurrentPresetName() const {
    juce::SpinLock::ScopedLockType sl(metadataLock);
    return currentPresetName;
  }

  void setPresetMetadata(const juce::String &name, bool dirty) {
    {
      juce::SpinLock::ScopedLockType sl(metadataLock);
      currentPresetName = name.isNotEmpty()
                              ? name
                              : juce::String(FanfareSynth::StateSchema::kDefaultInitPresetName);
    }
    presetDirty.store(dirty, std::memory_order_relaxed);
  }

  bool isPresetDirty() const noexcept {
    return presetDirty.load(std::memory_order_relaxed);
  }

  void setPresetDirty(bool dirty) noexcept {
    presetDirty.store(dirty, std::memory_order_relaxed);
  }

  float getOutputPeakL() const noexcept {
    return peakOutL.load(std::memory_order_relaxed);
  }
  float getOutputPeakR() const noexcept {
    return peakOutR.load(std::memory_order_relaxed);
  }
  int getActiveVoiceCount() const noexcept {
    return activeVoicesCount.load(std::memory_order_relaxed);
  }

  // ===== UI-thread MIDI / Wheel injection ====================================

  void injectNote(int note, bool on, float vel = 0.85f) {
    juce::SpinLock::ScopedLockType sl(uiMidiLock);
    uiMidiQueue.addEvent(
        on ? juce::MidiMessage::noteOn(1, juce::jlimit(0, 127, note), vel)
           : juce::MidiMessage::noteOff(1, juce::jlimit(0, 127, note)),
        0);
  }

  void setUiPitchBend(float bipolar) noexcept {
    pitchBend.store(juce::jlimit(-1.0f, 1.0f, bipolar),
                    std::memory_order_relaxed);
  }
  float getPitchBend() const noexcept {
    return pitchBend.load(std::memory_order_relaxed);
  }

  void setUiModWheel(float norm) noexcept {
    modWheel.store(juce::jlimit(0.0f, 1.0f, norm), std::memory_order_relaxed);
  }
  float getModWheel() const noexcept {
    return modWheel.load(std::memory_order_relaxed);
  }

  void allNotesOff() noexcept {
    panicPending.store(true, std::memory_order_release);
    juce::SpinLock::ScopedTryLockType sl(uiMidiLock);
    if (sl.isLocked())
      uiMidiQueue.clear();
  }

  // ===== Real-time audio processing ==========================================

  void processBlock(juce::AudioBuffer<float> &buffer,
                    juce::MidiBuffer &midiMessages) {
    using namespace FanfareSynth;
    juce::ScopedNoDenormals noDenormals;

    const int numSamples = buffer.getNumSamples();
    if (numSamples <= 0)
      return;

    if (synthBuffer.getNumSamples() < numSamples ||
        synthBuffer.getNumChannels() < 2) {
      synthBuffer.setSize(2, numSamples, false, false, true);
    }
    if ((int)lfoScratch.size() < numSamples)
      lfoScratch.resize((size_t)numSamples, 0.0f);

    synthBuffer.clear(0, numSamples);

    if (panicPending.exchange(false, std::memory_order_acq_rel))
      panicNow();

    {
      juce::SpinLock::ScopedTryLockType sl(uiMidiLock);
      if (sl.isLocked() && !uiMidiQueue.isEmpty()) {
        midiMessages.addEvents(uiMidiQueue, 0, -1, 0);
        uiMidiQueue.clear();
      }
    }

    ParamSnapshot snap;
    for (int i = 0; i < kNumParams; ++i)
      snap.v[(size_t)i] = p[(size_t)i].load(std::memory_order_relaxed);

    const bool useGateVca = ((int)std::lround(snap[ParamId::vcaMode]) == 1);
    for (auto &u : units) {
      if (u.active && u.midiNote == -1 && u.isFinished(useGateVca)) {
        u.active = false;
      }
    }
    for (auto &t : stealTails) {
      if (t.active && t.stealFadeRemaining <= 0)
        t.active = false;
    }

    for (const auto meta : midiMessages) {
      const auto msg = meta.getMessage();
      if (msg.isNoteOn()) {
        handleNoteOn(msg.getNoteNumber(), msg.getVelocity() / 127.0f, snap);
      } else if (msg.isNoteOff()) {
        handleNoteOff(msg.getNoteNumber(), snap);
      } else if (msg.isController()) {
        handleController(msg, snap);
      } else if (msg.isPitchWheel()) {
        pitchBend.store((msg.getPitchWheelValue() - 8192) / 8192.0f,
                        std::memory_order_relaxed);
      } else if (msg.isChannelPressure()) {
        aftertouch.store(msg.getChannelPressureValue() / 127.0f,
                         std::memory_order_relaxed);
      }
    }

    // Re-read CC-updated parameters into snapshot for immediate block response
    for (int i = 0; i < kNumParams; ++i)
      snap.v[(size_t)i] = p[(size_t)i].load(std::memory_order_relaxed);

    updateSmoothersTargets(snap);

    const double sr = currentSampleRate;
    const double lfoInc = (double)snap[ParamId::lfoRate] / sr;
    const int lfoShape = (int)std::lround(snap[ParamId::lfoWave]);
    for (int i = 0; i < numSamples; ++i)
      lfoScratch[(size_t)i] = (float)globalMod.stepLfo(lfoInc, lfoShape);

    globalMod.wanderPhase +=
        (DspConfig::kVintageWanderHz * (double)numSamples) / sr;
    if (globalMod.wanderPhase >= 1.0)
      globalMod.wanderPhase -= std::floor(globalMod.wanderPhase);

    float *outL = synthBuffer.getWritePointer(0);
    float *outR = synthBuffer.getWritePointer(1);

    // Advance parameter smoothers once per control sub-block during voice render
    renderAllVoices(snap, outL, outR, numSamples, sr);

    processEffectsAndMaster(snap, numSamples, sr);

    float blockPeakL = 0.0f;
    float blockPeakR = 0.0f;
    for (int i = 0; i < numSamples; ++i) {
      if (!std::isfinite(outL[i]))
        outL[i] = 0.0f;
      if (!std::isfinite(outR[i]))
        outR[i] = 0.0f;
      blockPeakL = juce::jmax(blockPeakL, std::abs(outL[i]));
      blockPeakR = juce::jmax(blockPeakR, std::abs(outR[i]));
    }

    const float decayedL = peakOutL.load(std::memory_order_relaxed) * 0.86f;
    const float decayedR = peakOutR.load(std::memory_order_relaxed) * 0.86f;
    peakOutL.store(juce::jmax(blockPeakL, decayedL), std::memory_order_relaxed);
    peakOutR.store(juce::jmax(blockPeakR, decayedR), std::memory_order_relaxed);

    int sounding = 0;
    for (const auto &u : units)
      if (u.active)
        ++sounding;
    activeVoicesCount.store(sounding, std::memory_order_relaxed);

    for (int ch = 0; ch < 2 && ch < buffer.getNumChannels(); ++ch)
      buffer.addFrom(ch, 0, synthBuffer, ch, 0, numSamples);

    globalTick += (juce::uint64)numSamples;
  }

private:
  enum SmootherIdx : int {
    SmCutoff = 0,
    SmReso,
    SmO1Level,
    SmO2Level,
    SmSubLevel,
    SmNoiseLevel,
    SmPulseWidth,
    SmVolume,
    SmChorusMix,
    SmDelayMix,
    SmReverbMix,
    kNumSmoothers
  };

  void prepareSmoothers() noexcept {
    using namespace FanfareSynth;
    smoothers[SmCutoff].prepare(currentSampleRate, DspConfig::kParamSmoothMediumSec,
                                getParam(ParamId::cutoff));
    smoothers[SmReso].prepare(currentSampleRate, DspConfig::kParamSmoothFastSec,
                              getParam(ParamId::reso));
    smoothers[SmO1Level].prepare(currentSampleRate, DspConfig::kParamSmoothFastSec,
                                 getParam(ParamId::o1Level));
    smoothers[SmO2Level].prepare(currentSampleRate, DspConfig::kParamSmoothFastSec,
                                 getParam(ParamId::o2Level));
    smoothers[SmSubLevel].prepare(currentSampleRate, DspConfig::kParamSmoothFastSec,
                                  getParam(ParamId::subLevel));
    smoothers[SmNoiseLevel].prepare(currentSampleRate, DspConfig::kParamSmoothFastSec,
                                    getParam(ParamId::noiseLevel));
    smoothers[SmPulseWidth].prepare(currentSampleRate, DspConfig::kParamSmoothFastSec,
                                    getParam(ParamId::pwmManual));
    smoothers[SmVolume].prepare(currentSampleRate, DspConfig::kParamSmoothFastSec,
                                getParam(ParamId::volume));
    smoothers[SmChorusMix].prepare(currentSampleRate, DspConfig::kParamSmoothMediumSec,
                                   getParam(ParamId::choMix));
    smoothers[SmDelayMix].prepare(currentSampleRate, DspConfig::kParamSmoothMediumSec,
                                  getParam(ParamId::dlyMix));
    smoothers[SmReverbMix].prepare(currentSampleRate, DspConfig::kParamSmoothMediumSec,
                                   getParam(ParamId::revMix));
  }

  void updateSmoothersTargets(const FanfareSynth::ParamSnapshot &s) noexcept {
    using FanfareSynth::ParamId;
    const bool snapNow =
        snapSmoothersPending.exchange(false, std::memory_order_acq_rel);

    auto applyTarget = [&](int smIdx, float val) {
      if (snapNow)
        smoothers[(size_t)smIdx].reset(val);
      else
        smoothers[(size_t)smIdx].setTarget(val);
    };

    applyTarget(SmCutoff, s[ParamId::cutoff]);
    applyTarget(SmReso, s[ParamId::reso]);
    applyTarget(SmO1Level, s[ParamId::o1Level]);
    applyTarget(SmO2Level, s[ParamId::o2Level]);
    applyTarget(SmSubLevel, s[ParamId::subLevel]);
    applyTarget(SmNoiseLevel, s[ParamId::noiseLevel]);
    applyTarget(SmPulseWidth, s[ParamId::pwmManual]);
    applyTarget(SmVolume, s[ParamId::volume]);
    applyTarget(SmChorusMix,
                ((int)std::lround(s[ParamId::choMode]) > 0) ? s[ParamId::choMix]
                                                            : 0.0f);
    applyTarget(SmDelayMix, s[ParamId::dlyMix]);
    applyTarget(SmReverbMix, s[ParamId::revMix]);
  }

  void migrateVersion1State(const juce::DynamicObject &obj) noexcept {
    using FanfareSynth::ParamId;
    if ((int)std::lround(getParam(ParamId::o1Wave)) == 3) {
      const float lvl = getParam(ParamId::o1Level);
      setParam(ParamId::noiseLevel,
               juce::jmax(getParam(ParamId::noiseLevel), lvl), false);
      setParam(ParamId::o1Level, 0.0f, false);
      setParam(ParamId::o1Wave, 0.0f, false);
    }
    if ((int)std::lround(getParam(ParamId::o2Wave)) == 3) {
      const float lvl = getParam(ParamId::o2Level);
      setParam(ParamId::noiseLevel,
               juce::jmax(getParam(ParamId::noiseLevel), lvl), false);
      setParam(ParamId::o2Level, 0.0f, false);
      setParam(ParamId::o2Wave, 0.0f, false);
    }
    if (obj.hasProperty("filtEnvAmt")) {
      const float rawEnv = (float)(double)obj.getProperty("filtEnvAmt");
      if (rawEnv < 0.0f) {
        setParam(ParamId::filtEnvPol, 1.0f, false);
        setParam(ParamId::filtEnvAmt, std::abs(rawEnv), false);
      }
    }
    if ((int)std::lround(getParam(ParamId::pwmSource)) == 1 &&
        getParam(ParamId::pwmDepth) <= 0.001f && obj.hasProperty("lfoToPwm")) {
      setParam(ParamId::pwmDepth, (float)(double)obj.getProperty("lfoToPwm"),
               false);
    }
    if (obj.hasProperty("choMode") &&
        (int)std::lround((double)obj.getProperty("choMode")) == 3) {
      setParam(ParamId::choMode, 4.0f, false);
    }
  }

  void panicNow() noexcept {
    for (auto &u : units)
      u.hardReset();
    for (auto &t : stealTails)
      t.hardReset();
    monoStack.clear();
    monoUnits.clear();
    sustainedNotes.clear();
    sustainOn = false;
    aftertouch.store(0.0f, std::memory_order_relaxed);
  }

  static double midiNoteToFreq(int note) noexcept {
    using namespace FanfareSynth::DspConfig;
    return kReferencePitchHz *
           std::pow(2.0, ((double)note - kReferenceMidiNote) /
                             kSemitonesPerOctave);
  }

  static int unisonVoiceCount(float v) noexcept {
    static constexpr int kCounts[] = {1, 2, 4, 6};
    return kCounts[juce::jlimit(0, 3, (int)std::lround(v))];
  }

  void spawnStealTail(const FanfareSynth::SynthVoiceUnit &victim) noexcept {
    if (!victim.active || victim.lastOutputLevel < 0.0005)
      return;

    FanfareSynth::SynthVoiceUnit *slot = &stealTails[0];
    for (auto &t : stealTails) {
      if (!t.active) {
        slot = &t;
        break;
      }
      if (t.stealFadeRemaining < slot->stealFadeRemaining)
        slot = &t;
    }

    *slot = victim;
    slot->reservedMono = false;
    slot->midiNote = -1;
    const int fadeSamples = juce::jmax(
        24, (int)std::lround(FanfareSynth::DspConfig::kStealFadeSeconds *
                             currentSampleRate));
    slot->stealFadeTotal = fadeSamples;
    slot->stealFadeRemaining = fadeSamples;
  }

  FanfareSynth::SynthVoiceUnit *allocPolyUnit(int incomingNote,
                                              bool useGateVca) noexcept {
    // 1. Re-trigger existing voice playing the exact same note (move tail first)
    if (incomingNote >= 0) {
      for (auto &u : units) {
        if (u.active && !u.reservedMono && u.midiNote == incomingNote) {
          spawnStealTail(u);
          u.hardReset();
          return &u;
        }
      }
    }

    // 2. Any completely idle voice
    for (auto &u : units) {
      if (!u.active && !u.reservedMono)
        return &u;
    }

    // 3. Quietest voice already in release stage
    FanfareSynth::SynthVoiceUnit *quietestRel = nullptr;
    for (auto &u : units) {
      if (u.reservedMono)
        continue;
      const bool releasing =
          (u.midiNote == -1) || (!useGateVca && u.ampEnv.isReleasing());
      if (releasing) {
        if (quietestRel == nullptr ||
            u.lastOutputLevel < quietestRel->lastOutputLevel)
          quietestRel = &u;
      }
    }
    if (quietestRel != nullptr) {
      spawnStealTail(*quietestRel);
      quietestRel->hardReset();
      return quietestRel;
    }

    // 4. Oldest held polyphonic voice (crossfaded via stealTail)
    FanfareSynth::SynthVoiceUnit *oldest = nullptr;
    for (auto &u : units) {
      if (u.reservedMono)
        continue;
      if (oldest == nullptr || u.startedTick < oldest->startedTick)
        oldest = &u;
    }
    if (oldest != nullptr) {
      spawnStealTail(*oldest);
      oldest->hardReset();
    }
    return oldest;
  }

  void handleNoteOn(int note, float vel,
                    const FanfareSynth::ParamSnapshot &s) noexcept {
    using namespace FanfareSynth;
    const int mode = (int)std::lround(s[ParamId::voiceMode]);
    const int uni = unisonVoiceCount(s[ParamId::unison]);
    const bool monoFamily = (mode != 0) || (uni > 1);
    const bool useGateVca = ((int)std::lround(s[ParamId::vcaMode]) == 1);

    bool anyKeyHeld = !monoStack.empty();
    if (!anyKeyHeld) {
      for (const auto &u : units)
        if (u.active && u.midiNote >= 0) {
          anyKeyHeld = true;
          break;
        }
    }

    if ((int)std::lround(s[ParamId::lfoTrig]) == 1 && !anyKeyHeld) {
      globalMod.lfoPhase = 0.0;
      globalMod.lfoSampleAndHold = globalMod.nextRandomBipolar();
    }

    if (!monoFamily) {
      if (!monoUnits.empty()) {
        for (int ui : monoUnits)
          units[(size_t)ui].reservedMono = false;
        monoUnits.clear();
      }
      if (auto *u = allocPolyUnit(note, useGateVca))
        triggerPolyUnit(*u, note, vel);
      return;
    }

    const bool wasHeld = !monoStack.empty();
    monoStack.erase(std::remove(monoStack.begin(), monoStack.end(), note),
                    monoStack.end());
    if ((int)monoStack.size() < DspConfig::kMaxMonoStackDepth)
      monoStack.push_back(note);

    const bool legatoOverlap = (mode == 2) && wasHeld;
    const int glideSetting = (int)std::lround(s[ParamId::glideMode]);
    bool doGlide = (glideSetting > 0) && (wasHeld || glideSetting == 1);
    if (!glideStarted) {
      doGlide = false;
      glideStarted = true;
    }

    const double targetFreq = midiNoteToFreq(note);

    while ((int)monoUnits.size() > uni) {
      auto &u = units[(size_t)monoUnits.back()];
      u.ampEnv.noteOff();
      u.filtEnv.noteOff();
      u.gateEnv.noteOff();
      u.midiNote = -1;
      u.reservedMono = false;
      monoUnits.pop_back();
    }
    while ((int)monoUnits.size() < uni) {
      auto *u = allocPolyUnit(-1, useGateVca);
      if (u == nullptr)
        break;
      u->active = false;
      u->reservedMono = true;
      u->midiNote = note;
      u->glideFreq = targetFreq;
      u->glideTarget = targetFreq;
      monoUnits.push_back((int)(u - units.data()));
    }

    const int count = (int)monoUnits.size();
    const float uniNorm = 1.0f / std::sqrt((float)juce::jmax(1, count));
    for (int idx = 0; idx < count; ++idx) {
      auto &u = units[(size_t)monoUnits[(size_t)idx]];
      const bool wasSounding = u.active && !u.isFinished(useGateVca);
      u.active = true;
      u.reservedMono = true;
      u.midiNote = note;
      u.baseFreq = targetFreq;
      u.glideTarget = targetFreq;
      u.velocity = juce::jmax(vel, (float)DspConfig::kMinNoteVelocity);
      u.startedTick = globalTick;

      if (!wasSounding) {
        u.osc1.reset((double)idx * 0.17 + 0.25);
        u.osc2.reset((double)idx * 0.31 + 0.25);
        u.hpf.reset();
        u.ladder.reset();
        u.pwmPhase = 0.0;
        u.ageSeconds = 0.0;
        u.gliding = false;
        u.glideFreq = targetFreq;
        u.ampEnv.noteOn(true);
        u.filtEnv.noteOn(true);
        u.gateEnv.noteOn(true);
      } else {
        if (doGlide) {
          u.gliding = true;
        } else {
          u.gliding = false;
          u.glideFreq = targetFreq;
        }
        if (!legatoOverlap) {
          u.ampEnv.noteOn(false);
          u.filtEnv.noteOn(false);
          u.gateEnv.noteOn(false);
          u.ageSeconds = 0.0;
        }
      }

      const float spreadPos =
          (count > 1) ? ((float)idx / (float)(count - 1)) : 0.5f;
      u.detuneCents = s[ParamId::uniDetune] * (2.0f * spreadPos - 1.0f);
      const float panAngle =
          juce::MathConstants<float>::halfPi *
          (0.5f + s[ParamId::uniWidth] * (spreadPos - 0.5f));
      u.panL = std::cos(panAngle) * uniNorm;
      u.panR = std::sin(panAngle) * uniNorm;
    }
  }

  void triggerPolyUnit(FanfareSynth::SynthVoiceUnit &u, int note,
                       float vel) noexcept {
    u.active = true;
    u.reservedMono = false;
    u.midiNote = note;
    u.velocity =
        juce::jmax(vel, (float)FanfareSynth::DspConfig::kMinNoteVelocity);
    u.baseFreq = midiNoteToFreq(note);
    u.detuneCents = 0.0f;
    u.panL = 0.70710678f;
    u.panR = 0.70710678f;
    u.osc1.reset(0.25);
    u.osc2.reset(0.50);
    u.hpf.reset();
    u.ladder.reset();
    u.ampEnv.noteOn(true);
    u.filtEnv.noteOn(true);
    u.gateEnv.noteOn(true);
    u.pwmPhase = 0.0;
    u.ageSeconds = 0.0;
    u.gliding = false;
    u.glideFreq = u.baseFreq;
    u.glideTarget = u.baseFreq;
    u.startedTick = globalTick;
  }

  void handleNoteOff(int note,
                     const FanfareSynth::ParamSnapshot &s) noexcept {
    if (sustainOn) {
      if ((int)sustainedNotes.size() < 64)
        sustainedNotes.push_back(note);
      return;
    }
    releaseNote(note, s);
  }

  void releaseNote(int note, const FanfareSynth::ParamSnapshot &s) noexcept {
    using FanfareSynth::ParamId;
    const int mode = (int)std::lround(s[ParamId::voiceMode]);
    const int uni = unisonVoiceCount(s[ParamId::unison]);
    const bool monoFamily = (mode != 0) || (uni > 1);

    if (monoFamily) {
      for (int i = (int)monoStack.size() - 1; i >= 0; --i) {
        if (monoStack[(size_t)i] == note) {
          monoStack.erase(monoStack.begin() + i);
          break;
        }
      }
      if (!monoStack.empty()) {
        const int fallbackNote = monoStack.back();
        const double targetFreq = midiNoteToFreq(fallbackNote);
        const bool glideEnabled = ((int)std::lround(s[ParamId::glideMode]) > 0);
        for (int ui : monoUnits) {
          auto &u = units[(size_t)ui];
          u.midiNote = fallbackNote;
          u.baseFreq = targetFreq;
          u.glideTarget = targetFreq;
          if (glideEnabled)
            u.gliding = true;
          else
            u.glideFreq = targetFreq;
        }
      } else {
        for (int ui : monoUnits) {
          auto &u = units[(size_t)ui];
          u.ampEnv.noteOff();
          u.filtEnv.noteOff();
          u.gateEnv.noteOff();
          u.midiNote = -1;
        }
      }
      return;
    }

    for (auto &u : units) {
      if (u.active && u.midiNote == note) {
        u.ampEnv.noteOff();
        u.filtEnv.noteOff();
        u.gateEnv.noteOff();
        u.midiNote = -1;
      }
    }
  }

  void handleController(const juce::MidiMessage &msg,
                        const FanfareSynth::ParamSnapshot &s) noexcept {
    using namespace FanfareSynth;
    const int ccNum = msg.getControllerNumber();
    const int ccVal = msg.getControllerValue();

    if (ccNum == DspConfig::kMidiSustainPedalCC) {
      const bool down = (ccVal >= 64);
      if (sustainOn && !down) {
        sustainOn = false;
        for (int n : sustainedNotes)
          releaseNote(n, s);
        sustainedNotes.clear();
      } else {
        sustainOn = down;
      }
      return;
    }

    if (ccNum == DspConfig::kMidiModWheelCC) {
      modWheel.store((float)ccVal / 127.0f, std::memory_order_relaxed);
      return;
    }

    if (ccNum == DspConfig::kMidiAllSoundOffCC ||
        ccNum == DspConfig::kMidiAllNotesOffCC) {
      panicNow();
      return;
    }

    const auto &defs = paramDefs();
    const float norm = (float)ccVal / 127.0f;
    for (int i = 0; i < kNumParams; ++i) {
      if (ccMappings[(size_t)i].load(std::memory_order_relaxed) == ccNum) {
        const float mapped = normalizedToValue(defs[(size_t)i], norm);
        setParam(i, mapped, true);
      }
    }
  }

  // ===== Block & sub-block voice rendering ===================================

  void renderAllVoices(const FanfareSynth::ParamSnapshot &s, float *outL,
                       float *outR, int numSamples, double sr) noexcept {
    using namespace FanfareSynth;
    int offset = 0;
    while (offset < numSamples) {
      const int subCount =
          juce::jmin(DspConfig::kControlRateStep, numSamples - offset);

      const float curCutoff = smoothers[SmCutoff].advanceBlock(subCount);
      const float curReso = smoothers[SmReso].advanceBlock(subCount);
      const float curO1Gain = smoothers[SmO1Level].advanceBlock(subCount);
      const float curO2Gain = smoothers[SmO2Level].advanceBlock(subCount);
      const float curSubGain = smoothers[SmSubLevel].advanceBlock(subCount);
      const float curNoiseGain = smoothers[SmNoiseLevel].advanceBlock(subCount);
      const float curPwManual = smoothers[SmPulseWidth].advanceBlock(subCount);

      for (auto &u : units) {
        if (u.active) {
          renderVoiceSubBlock(u, s, outL + offset, outR + offset, offset,
                              subCount, sr, curCutoff, curReso, curO1Gain,
                              curO2Gain, curSubGain, curNoiseGain, curPwManual);
        }
      }
      for (auto &t : stealTails) {
        if (t.active && t.stealFadeRemaining > 0) {
          renderVoiceSubBlock(t, s, outL + offset, outR + offset, offset,
                              subCount, sr, curCutoff, curReso, curO1Gain,
                              curO2Gain, curSubGain, curNoiseGain, curPwManual);
        }
      }
      offset += subCount;
    }
  }

  void renderVoiceSubBlock(FanfareSynth::SynthVoiceUnit &u,
                           const FanfareSynth::ParamSnapshot &s, float *outL,
                           float *outR, int lfoOffset, int subSamples,
                           double sr, float smoothedCutoff, float smoothedReso,
                           float o1Gain, float o2Gain, float subGain,
                           float noiseGain, float pwManual) noexcept {
    using namespace FanfareSynth;

    const double vintageAmt = (double)s[ParamId::vintage];
    const double envVintage =
        1.0 + vintageAmt * (u.cardEnvScale - 1.0);

    u.ampEnv.sampleRate = sr;
    u.ampEnv.aTime = (double)s[ParamId::atk] * envVintage;
    u.ampEnv.dTime = (double)s[ParamId::dec] * envVintage;
    u.ampEnv.sLevel = (double)s[ParamId::sus];
    u.ampEnv.rTime = (double)s[ParamId::rel] * envVintage;
    u.ampEnv.updateCoefficients();

    u.filtEnv.sampleRate = sr;
    u.filtEnv.aTime = (double)s[ParamId::fatk] * envVintage;
    u.filtEnv.dTime = (double)s[ParamId::fdec] * envVintage;
    u.filtEnv.sLevel = (double)s[ParamId::fsus];
    u.filtEnv.rTime = (double)s[ParamId::frel] * envVintage;
    u.filtEnv.updateCoefficients();

    u.hpf.update(s[ParamId::hpfFreq], sr);

    const int w1 = (int)std::lround(s[ParamId::o1Wave]);
    const int w2 = (int)std::lround(s[ParamId::o2Wave]);
    const int subMode = (int)std::lround(s[ParamId::subWave]);
    const bool computeSub = (subGain > 0.0002f);
    const bool computeO2 = (o2Gain > 0.0002f);
    const bool computeNoise = (noiseGain > 0.0002f);
    const bool slope24 = ((int)std::lround(s[ParamId::filtSlope]) > 0);
    const bool envSrcIsAmp = ((int)std::lround(s[ParamId::filtEnvSrc]) > 0);
    const bool useGateVca = ((int)std::lround(s[ParamId::vcaMode]) == 1);
    const double envPolSign =
        ((int)std::lround(s[ParamId::filtEnvPol]) == 0) ? 1.0 : -1.0;

    const double octSemi1 =
        (double)std::lround(s[ParamId::o1Oct]) * DspConfig::kSemitonesPerOctave;
    const double octSemi2 =
        (double)std::lround(s[ParamId::o2Oct]) * DspConfig::kSemitonesPerOctave +
        (double)std::lround(s[ParamId::o2Semi]);

    const double wander =
        std::sin((globalMod.wanderPhase + (double)u.voiceCardIndex * 0.13) *
                 juce::MathConstants<double>::twoPi);
    const double vintagePitchCents =
        vintageAmt * (u.cardPitchTrimCents + 1.2 * wander);

    const double ratio1 = std::pow(
        2.0, (octSemi1 * 100.0 + (double)s[ParamId::o1Detune] +
              (double)u.detuneCents + vintagePitchCents) /
                 DspConfig::kCentsPerOctave);
    const double ratio2 = std::pow(
        2.0, (octSemi2 * 100.0 + (double)s[ParamId::o2Detune] +
              (double)u.detuneCents + vintagePitchCents) /
                 DspConfig::kCentsPerOctave);

    const double bendSemis =
        (double)pitchBend.load(std::memory_order_relaxed) *
        (double)std::lround(s[ParamId::bendRange]);
    const double bendMul =
        std::pow(2.0, bendSemis / DspConfig::kSemitonesPerOctave);

    if (u.gliding) {
      const double glideBlockCoef = std::exp(
          -(double)subSamples /
          juce::jmax(1.0, (double)s[ParamId::glideTime] *
                              DspConfig::kGlideTauScale * sr));
      u.glideFreq = u.glideTarget + (u.glideFreq - u.glideTarget) * glideBlockCoef;
      if (std::abs(u.glideFreq - u.glideTarget) < u.glideTarget * 0.0005) {
        u.glideFreq = u.glideTarget;
        u.gliding = false;
      }
    }

    const double currentNoteFreq =
        (u.gliding ? u.glideFreq : u.baseFreq) * bendMul;

    const double mod = (double)modWheel.load(std::memory_order_relaxed);
    const double at = (double)aftertouch.load(std::memory_order_relaxed);
    const double lfoSample = (double)lfoScratch[(size_t)lfoOffset];

    const double lfoDelaySec = (double)s[ParamId::lfoDelay];
    double lfoFade = 1.0;
    if (lfoDelaySec > 0.001) {
      if (u.ageSeconds < lfoDelaySec)
        lfoFade = 0.0;
      else
        lfoFade = juce::jlimit(0.0, 1.0,
                               (u.ageSeconds - lfoDelaySec) /
                                   DspConfig::kLfoFadeRampSeconds);
    }

    const double delayedLfo = lfoSample * lfoFade;
    const double totalPitchLfoCents =
        (double)s[ParamId::lfoToPitch] * delayedLfo +
        (mod * DspConfig::kModWheelVibratoCents +
         at * DspConfig::kAftertouchVibratoCents) *
            lfoSample;
    const double pitchModMul =
        std::pow(2.0, totalPitchLfoCents / DspConfig::kCentsPerOctave);

    const double dt1 = juce::jlimit(
        DspConfig::kMinOscFreqHz / sr, DspConfig::kMaxNyquistRatio,
        (currentNoteFreq * ratio1 * pitchModMul) / sr);
    const double dt2 = juce::jlimit(
        DspConfig::kMinOscFreqHz / sr, DspConfig::kMaxNyquistRatio,
        (currentNoteFreq * ratio2 * pitchModMul) / sr);

    const int pwmSrc = (int)std::lround(s[ParamId::pwmSource]);
    double pulseWidth = (double)pwManual;
    if (pwmSrc == 1) {
      const double center = juce::jlimit(DspConfig::kPwmCenterMin,
                                         DspConfig::kPwmCenterMax,
                                         (double)pwManual);
      pulseWidth = center + (double)s[ParamId::pwmDepth] *
                                DspConfig::kPwmMaxModSpan * delayedLfo;
    } else if (pwmSrc == 2) {
      const double pwmVal =
          std::sin(u.pwmPhase * juce::MathConstants<double>::twoPi);
      u.pwmPhase += ((double)s[ParamId::pwmRate] * (double)subSamples) / sr;
      if (u.pwmPhase >= 1.0)
        u.pwmPhase -= std::floor(u.pwmPhase);
      const double center = juce::jlimit(DspConfig::kPwmCenterMin,
                                         DspConfig::kPwmCenterMax,
                                         (double)pwManual);
      pulseWidth = center + (double)s[ParamId::pwmDepth] *
                                DspConfig::kPwmMaxModSpan * pwmVal;
    }
    pulseWidth = juce::jlimit(DspConfig::kMinPulseWidth,
                              DspConfig::kMaxPulseWidth, pulseWidth);

    const double velCurve = std::pow((double)u.velocity, 1.35);
    const double ampVelGain =
        (1.0 - (double)s[ParamId::velToAmp]) +
        (double)s[ParamId::velToAmp] * velCurve;
    const double filtVelGain =
        (1.0 - (double)s[ParamId::velToFilt]) +
        (double)s[ParamId::velToFilt] * velCurve;

    const double noteOctavesFromC4 =
        std::log2(juce::jmax(8.0, currentNoteFreq) / 261.6255653);
    const double keyTrackOct =
        (double)s[ParamId::keyTrack] * noteOctavesFromC4;

    const double envOctScale = envPolSign * (double)s[ParamId::filtEnvAmt] *
                               DspConfig::kFilterEnvMaxOctaves * filtVelGain;
    const double lfoFilterOct =
        (double)s[ParamId::lfoToFilt] * DspConfig::kLfoFilterMaxOctaves *
            delayedLfo +
        mod * DspConfig::kModWheelFilterOctaves +
        at * DspConfig::kAftertouchFilterOctaves;
    const double vintageCutOct = vintageAmt * u.cardCutoffTrimOct;
    const double baseCutHz =
        (double)smoothedCutoff *
        std::pow(2.0, keyTrackOct + lfoFilterOct + vintageCutOct);

    u.ladder.setResonance((double)smoothedReso);

    double ampEnvBuf[DspConfig::kControlRateStep];
    double gateEnvBuf[DspConfig::kControlRateStep];
    double filtEnvBuf[DspConfig::kControlRateStep];
    const double fEnvInit = envSrcIsAmp ? u.ampEnv.level : u.filtEnv.level;
    for (int i = 0; i < subSamples; ++i) {
      ampEnvBuf[i] = u.ampEnv.next();
      gateEnvBuf[i] = u.gateEnv.next();
      filtEnvBuf[i] = envSrcIsAmp ? ampEnvBuf[i] : u.filtEnv.next();
    }

    double curG = Dsp::OtaLadderFilter::computeG(
        baseCutHz * std::pow(2.0, envOctScale * fEnvInit), sr);
    double gStep = 0.0;

    const double tremDepth = (double)s[ParamId::lfoToAmp];
    const float gainL = u.panL * DspConfig::kVoiceHeadroom;
    const float gainR = u.panR * DspConfig::kVoiceHeadroom;
    const double invSr = 1.0 / sr;

    for (int i = 0; i < subSamples; ++i) {
      if ((i & 3) == 0) {
        const int span = juce::jmin(4, subSamples - i);
        const double targetEnv = filtEnvBuf[i + span - 1];
        const double targetG = Dsp::OtaLadderFilter::computeG(
            baseCutHz * std::pow(2.0, envOctScale * targetEnv), sr);
        gStep = (targetG - curG) / (double)span;
      }
      curG += gStep;
      u.ladder.setG(curG);

      const auto dco1 =
          u.osc1.nextWithSub(dt1, w1, pulseWidth, subMode, computeSub);
      double mix = dco1.mainSample * (double)o1Gain +
                   dco1.subSample * (double)subGain;

      if (computeO2)
        mix += u.osc2.nextMainOnly(dt2, w2, pulseWidth) * (double)o2Gain;
      if (computeNoise)
        mix += u.noiseGen.next() * (double)noiseGain * 0.55;

      const double vcaEnv = useGateVca ? gateEnvBuf[i] : ampEnvBuf[i];

      const double hpfOut = u.hpf.process(mix);
      const double vcfOut = u.ladder.process(hpfOut, slope24);

      const double lfoNow =
          (double)lfoScratch[(size_t)(lfoOffset + i)] * lfoFade;
      const double tremolo = 1.0 - tremDepth * 0.5 * (1.0 - lfoNow);

      double stealMul = 1.0;
      if (u.stealFadeRemaining > 0) {
        stealMul = (double)u.stealFadeRemaining / (double)u.stealFadeTotal;
        --u.stealFadeRemaining;
        if (u.stealFadeRemaining <= 0)
          u.active = false;
      }

      const double totalAmp = vcaEnv * ampVelGain * tremolo * stealMul;
      u.lastOutputLevel = totalAmp;

      outL[i] += (float)(vcfOut * totalAmp) * gainL;
      outR[i] += (float)(vcfOut * totalAmp) * gainR;
      u.ageSeconds += invSr;
      if (!u.active)
        break;
    }
  }

  void processEffectsAndMaster(const FanfareSynth::ParamSnapshot &s,
                               int numSamples, double sr) noexcept {
    using namespace FanfareSynth;
    float *L = synthBuffer.getWritePointer(0);
    float *R = synthBuffer.getWritePointer(1);

    chorus.process(L, R, numSamples, (int)std::lround(s[ParamId::choMode]),
                   s[ParamId::choRate], s[ParamId::choDepth],
                   smoothers[SmChorusMix]);

    delay.process(L, R, numSamples, s[ParamId::dlyTime], s[ParamId::dlyFb],
                  s[ParamId::dlyDamp], smoothers[SmDelayMix]);

    const float revWet = smoothers[SmReverbMix].advanceBlock(numSamples);
    const int rType = juce::jlimit(0, 2, (int)std::lround(s[ParamId::revType]));
    reverb.processStereo(L, R, numSamples, rType, s[ParamId::revSize],
                         s[ParamId::revDamp], revWet, 1.0f - 0.28f * revWet);
    (void)sr;

    for (int i = 0; i < numSamples; ++i) {
      const float vol = smoothers[SmVolume].next();
      L[i] = Dsp::fastTanhf(L[i]) * vol;
      R[i] = Dsp::fastTanhf(R[i]) * vol;
    }
  }

  // ===== Members =============================================================

  std::array<std::atomic<float>, kNumParams> p{};
  std::array<std::atomic<int>, kNumParams> ccMappings{};
  std::array<FanfareSynth::Dsp::OnePoleSmoother, kNumSmoothers> smoothers{};
  std::atomic<bool> snapSmoothersPending{true};

  mutable juce::SpinLock metadataLock;
  juce::String currentPresetName{
      FanfareSynth::StateSchema::kDefaultInitPresetName};
  std::atomic<bool> presetDirty{false};

  std::atomic<float> pitchBend{0.0f};
  std::atomic<float> modWheel{0.0f};
  std::atomic<float> aftertouch{0.0f};
  std::atomic<float> peakOutL{0.0f};
  std::atomic<float> peakOutR{0.0f};
  std::atomic<int> activeVoicesCount{0};

  std::atomic<bool> panicPending{false};
  juce::SpinLock uiMidiLock;
  juce::MidiBuffer uiMidiQueue;

  bool sustainOn = false;
  bool glideStarted = false;
  std::vector<int> monoStack;
  std::vector<int> monoUnits;
  std::vector<int> sustainedNotes;

  std::array<FanfareSynth::SynthVoiceUnit,
             FanfareSynth::DspConfig::kMaxPlayableVoices>
      units{};
  std::array<FanfareSynth::SynthVoiceUnit,
             FanfareSynth::DspConfig::kStealTailSlots>
      stealTails{};
  FanfareSynth::GlobalModulationState globalMod;

  juce::uint64 globalTick = 0;
  double currentSampleRate = FanfareSynth::DspConfig::kDefaultSampleRate;

  juce::AudioBuffer<float> synthBuffer;
  std::vector<float> lfoScratch;

  FanfareSynth::Dsp::JunoChorus chorus;
  FanfareSynth::Dsp::PingPongDelay delay;
  FanfareDSP::LushReverb reverb;
};
