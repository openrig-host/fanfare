#pragma once

// JUCE-facing wrapper around the FanfareFm engine, mirroring the surface that
// RackSlot expects from FanfareSynthProcessor: prepare/processBlock with a
// MidiBuffer, getState/applyState as one JSON var, peaks, panic, UI note
// injection, and curated vintage preset library metadata.

#include <JuceHeader.h>
#include <array>
#include <atomic>
#include <bitset>
#include <cmath>
#include <cstring>
#include <map>
#include <vector>
#include "../../FanfareConstants.h"
#include "../../LushReverb.h"
#include "../dsp/JunoChorus.h"
#include "../dsp/PingPongDelay.h"
#include "../dsp/SynthDspMath.h"
#include "FmEngine.h"
#include "FmPresets.h"

class FmSynthProcessor {
public:
  enum FmFx : int {
    fxChoMode = 0,
    fxChoRate,
    fxChoDepth,
    fxChoMix,
    fxDlyTime,
    fxDlyFb,
    fxDlyDamp,
    fxDlyMix,
    fxRevType,
    fxRevSize,
    fxRevDamp,
    fxRevMix,
    kNumFmFx
  };

  struct PresetEntry {
    juce::String name;
    juce::String category;
    juce::String provenance;
    FanfareFm::FmPatch patch{};
    std::array<float, kNumFmFx> fxValues{};
    bool hasCuratedFx = false;
    bool isUser = false;
    bool isImported = false;
    juce::File userFile;
  };

  std::atomic<bool> enabled{false};

  enum FmCcParam : int {
    ccParamVolume = 0,
    ccParamFeedback,
    ccParamAlgorithm,
    ccParamLfoRate,
    ccParamLfoPmd,
    ccParamLfoAmd,
    ccParamTranspose,
    ccParamBendRange,
    kNumFmCcParams
  };

  static int ccParamIndexForId(const juce::String &id) {
    if (id == "volume") return ccParamVolume;
    if (id == "feedback") return ccParamFeedback;
    if (id == "algorithm") return ccParamAlgorithm;
    if (id == "lfoRate") return ccParamLfoRate;
    if (id == "lfoPmd") return ccParamLfoPmd;
    if (id == "lfoAmd") return ccParamLfoAmd;
    if (id == "transpose") return ccParamTranspose;
    if (id == "bendRange") return ccParamBendRange;
    return -1;
  }

  FmSynthProcessor() {
    scratchInt.assign((size_t)maxBlock, 0);
    fxBuf.assign((size_t)2 * maxBlock, 0.0f);
    uiMidiQueue.ensureSize(2048);
    mergedMidiScratch.ensureSize(4096);
    for (int i = 0; i < (int)kNumFmFx; ++i)
      fx[(size_t)i].store(fxDefault(i), std::memory_order_relaxed);
    for (int i = 0; i < (int)kNumFmCcParams; ++i)
      ccParamAtomics[(size_t)i].store(-1, std::memory_order_relaxed);
    loadDefaultFactoryVoice();
  }

  void prepare(double sampleRate, int maxBlockSize = 8192) {
    currentSampleRate = sampleRate;
    maxBlock = juce::jmax(64, maxBlockSize);
    engine_.prepare(sampleRate);
    scratchInt.assign((size_t)maxBlock, 0);
    fxBuf.assign((size_t)2 * maxBlock, 0.0f);
    uiMidiQueue.ensureSize(2048);
    mergedMidiScratch.ensureSize(4096);
    chorus_.prepare(sampleRate);
    delay_.prepare(sampleRate, fx[fxDlyTime].load(std::memory_order_relaxed));
    reverb_.prepare(sampleRate);
    smChoMix.prepare(sampleRate, 0.006, fx[fxChoMix].load(std::memory_order_relaxed));
    smDlyMix.prepare(sampleRate, 0.006, fx[fxDlyMix].load(std::memory_order_relaxed));
    smRevMix.prepare(sampleRate, 0.006, fx[fxRevMix].load(std::memory_order_relaxed));
    smVol.prepare(sampleRate, 0.006, volume.load(std::memory_order_relaxed));
    reset();
  }

  void reset() {
    engine_.reset();
    chorus_.reset();
    delay_.reset();
    reverb_.reset();
    peakOutL.store(0.0f);
    peakOutR.store(0.0f);
    activeVoicesCount.store(0);
  }

  void panicNow() {
    engine_.panic();
    activeVoicesCount.store(0);
  }

  void allNotesOff() { engine_.allNotesOff(); }
  void requestPanic() { panicPending.store(true); }

  int getActiveVoices() const { return activeVoicesCount.load(); }
  float getOutputPeakL() const { return peakOutL.load(); }
  float getOutputPeakR() const { return peakOutR.load(); }

  void injectNote(int note, bool on, float vel = 0.85f) {
    juce::MidiMessage m =
        on ? juce::MidiMessage::noteOn(
                 1, note,
                 (juce::uint8)juce::jlimit(1, 127, (int)(vel * 127.0f)))
           : juce::MidiMessage::noteOff(1, note);
    juce::SpinLock::ScopedLockType sl(uiMidiLock);
    uiMidiQueue.addEvent(m, 0);
  }

  void setUiPitchBend(float bipolar) {
    engine_.setPitchBend(8192 + (int)(bipolar * 8191.0f));
  }
  void setUiModWheel(float norm) { engine_.setModWheel((int)(norm * 127.0f)); }

  FanfareFm::FmEngine &engine() { return engine_; }
  const FanfareFm::FmEngine &engine() const { return engine_; }

  int getMode() const { return (int)engine_.mode(); }
  void setMode(int m) {
    engine_.setMode((FanfareFm::FmMode)juce::jlimit(0, 4, m));
    markDirty();
  }

  float getVolume() const { return volume.load(); }
  void setVolume(float v) { volume.store(juce::jlimit(0.0f, 1.0f, v)); }

  static float fxDefault(int i) {
    static const float kDef[kNumFmFx] = {0.0f, 0.65f, 0.45f, 0.30f, 0.34f,
                                         0.28f, 0.42f, 0.0f,  1.0f,  0.52f,
                                         0.42f, 0.18f};
    return kDef[juce::jlimit(0, (int)kNumFmFx - 1, i)];
  }
  float getFx(int i) const { return fx[(size_t)i].load(); }
  void setFx(int i, float v) { fx[(size_t)i].store(v); }

  juce::String getCurrentPresetName() const { return presetName; }
  juce::String getCurrentPresetProvenance() const { return presetProvenance; }
  juce::String getCurrentPresetCategory() const { return presetCategory; }
  int getCurrentPresetIndex() const { return currentPresetIndex; }

  void setPresetMetadata(const juce::String &name, bool dirty,
                         const juce::String &prov = {},
                         const juce::String &cat = {}, int idx = -1) {
    presetName = name;
    if (prov.isNotEmpty())
      presetProvenance = prov;
    if (cat.isNotEmpty())
      presetCategory = cat;
    if (idx >= -1)
      currentPresetIndex = idx;
    presetDirty.store(dirty);
  }
  bool isPresetDirty() const { return presetDirty.load(); }
  void markDirty() { presetDirty.store(true); }
  void setPresetDirty(bool d) { presetDirty.store(d); }

  static juce::File userPresetDir() {
    return FanfareConstants::getAppDirectory().getChildFile("fm-presets");
  }

  void refreshPresetCatalog() {
    catalog.clear();

    auto dir = userPresetDir();
    if (dir.isDirectory()) {
      auto files = dir.findChildFiles(juce::File::findFiles, false, "*.json");
      files.sort();
      for (const auto &f : files) {
        auto v = juce::JSON::parse(f);
        if (!v.isObject())
          continue;
        auto stateVar = v.getProperty("state", juce::var());
        PresetEntry e;
        if (!patchFromState(stateVar.getProperty("patch", juce::var()), e.patch))
          continue;
        e.isUser = true;
        e.userFile = f;
        e.name = v.getProperty("presetName", f.getFileNameWithoutExtension())
                     .toString();
        e.category = "User Presets";
        e.provenance = "USER";
        for (int k = 0; k < (int)kNumFmFx; ++k)
          e.fxValues[(size_t)k] = fxDefault(k);
        if (auto *stObj = stateVar.getDynamicObject()) {
          if (auto *fxObj = stObj->getProperty("fx").getDynamicObject()) {
            for (int k = 0; k < (int)kNumFmFx; ++k) {
              if (fxObj->hasProperty(fxKeyName(k)))
                e.fxValues[(size_t)k] =
                    (float)(double)fxObj->getProperty(fxKeyName(k));
            }
            e.hasCuratedFx = true;
          }
        }
        catalog.push_back(e);
      }
    }

    int n = 0;
    const FanfareFm::FmFactoryPreset *list = FanfareFm::factoryFmPresets(n);
    for (int i = 0; i < n; ++i) {
      PresetEntry e;
      e.name = juce::String::fromUTF8(list[i].name);
      e.category = juce::String::fromUTF8(list[i].category);
      e.provenance = juce::String::fromUTF8(list[i].provenance);
      std::memcpy(e.patch.d, list[i].patchBytes, FanfareFm::kPatchSize);
      for (int k = 0; k < (int)kNumFmFx; ++k)
        e.fxValues[(size_t)k] = list[i].fx[k];
      e.hasCuratedFx = true;
      catalog.push_back(e);
    }

    for (size_t i = 0; i < importedBank_.size(); ++i) {
      PresetEntry e = importedBank_[i];
      e.isImported = true;
      e.category = "Imported Sysex";
      if (e.provenance.isEmpty())
        e.provenance = "SYX";
      catalog.push_back(e);
    }

    syncCatalogIndex();
  }

  const std::vector<PresetEntry> &getPresetCatalog() const { return catalog; }

  void loadPresetAtIndex(int idx) {
    if (idx < 0 || idx >= (int)catalog.size())
      return;
    loadPreset(catalog[(size_t)idx], idx);
  }

  void loadPreset(const PresetEntry &e, int explicitIndex = -1) {
    if (!e.isUser && !e.isImported) {
      const auto curMode = engine_.mode();
      if (FanfareFm::fmModeNumOps(curMode) == 4) {
        engine_.setMode(FanfareFm::fmModeIsDual(curMode)
                            ? FanfareFm::FmMode::Dual6
                            : FanfareFm::FmMode::Dx7);
      }
    }
    engine_.loadPatchRaw(e.patch);
    if (e.hasCuratedFx) {
      for (int i = 0; i < (int)kNumFmFx; ++i)
        fx[(size_t)i].store(e.fxValues[(size_t)i], std::memory_order_relaxed);
    }
    int resolvedIdx = explicitIndex;
    if (resolvedIdx < 0) {
      for (int i = 0; i < (int)catalog.size(); ++i) {
        if (catalog[(size_t)i].name == e.name &&
            catalog[(size_t)i].provenance == e.provenance) {
          resolvedIdx = i;
          break;
        }
      }
    }
    setPresetMetadata(e.name, false, e.provenance, e.category, resolvedIdx);
  }

  bool saveUserPreset(const juce::String &name) {
    auto dir = userPresetDir();
    if (!dir.exists() && !dir.createDirectory())
      return false;
    setPresetMetadata(name, false, "USER", "User Presets", -1);
    auto file = dir.getChildFile(juce::File::createLegalFileName(name) + ".json");
    auto *obj = new juce::DynamicObject();
    obj->setProperty("presetName", name);
    obj->setProperty("state", getState());
    return file.replaceWithText(juce::JSON::toString(juce::var(obj)));
  }

  bool deleteUserPresetFile(const juce::File &f) { return f.deleteFile(); }

  int importSysexFile(const juce::File &f, juce::String &err) {
    juce::FileInputStream fis(f);
    if (!fis.openedOk()) {
      err = "Could not open file";
      return 0;
    }
    const juce::int64 len = fis.getTotalLength();
    if (len <= 0 || len > 1024 * 1024) {
      err = "Not a sysex file";
      return 0;
    }
    juce::MemoryBlock mb((size_t)len);
    if (fis.read(mb.getData(), (size_t)len) != (size_t)len) {
      err = "Read failed";
      return 0;
    }
    int ckErr = 0, skipped4 = 0;
    auto patches = FanfareFm::parseSysex((const uint8_t *)mb.getData(),
                                         (size_t)len, &ckErr, &skipped4);
    if (patches.empty()) {
      err = skipped4 > 0
                ? "Native 4-op dump: all DX100 and TX81Z factory banks are "
                  "already built into the preset browser"
                : "No Yamaha FM voice data found";
      return 0;
    }
    importedBank_.clear();
    for (auto &p : patches) {
      PresetEntry e;
      e.patch = p;
      e.name = juce::String(p.name()).trim();
      if (e.name.isEmpty())
        e.name = "Imported Voice";
      e.provenance = "SYX";
      e.category = "Imported Sysex";
      importedBank_.push_back(e);
    }
    if (ckErr > 0)
      err = juce::String(ckErr) + " checksum warning(s); imported anyway";
    return (int)patches.size();
  }

  const std::vector<PresetEntry> &getImportedBank() const { return importedBank_; }

  void loadImportedPatch(int i) {
    if (i < 0 || i >= (int)importedBank_.size())
      return;
    engine_.loadPatchRaw(importedBank_[(size_t)i].patch);
    setPresetMetadata(importedBank_[(size_t)i].name, true, "SYX",
                      "Imported Sysex", -1);
  }

  juce::var getState() const {
    auto *obj = new juce::DynamicObject();
    obj->setProperty("version", 2);
    obj->setProperty("enabled", enabled.load());
    obj->setProperty("presetName", presetName);
    obj->setProperty("presetProvenance", presetProvenance);
    obj->setProperty("presetCategory", presetCategory);
    obj->setProperty("presetDirty", presetDirty.load());
    obj->setProperty("mode", (int)engine_.mode());
    obj->setProperty("volume", (double)volume.load());
    const auto &p = engine_.patch();
    obj->setProperty("patch", juce::Base64::toBase64(p.d, FanfareFm::kPatchSize));
    auto *fxObj = new juce::DynamicObject();
    for (int i = 0; i < (int)kNumFmFx; ++i)
      fxObj->setProperty(fxKeyName(i), (double)fx[(size_t)i].load());
    obj->setProperty("fx", juce::var(fxObj));
    auto *ccObj = new juce::DynamicObject();
    for (const auto &kv : ccMap_)
      ccObj->setProperty(kv.first, kv.second);
    obj->setProperty("ccMap", juce::var(ccObj));
    return juce::var(obj);
  }

  bool applyState(const juce::var &v, bool preserveEnabledIfMissing = true) {
    auto *obj = v.getDynamicObject();
    if (obj == nullptr)
      return false;
    if (obj->hasProperty("enabled"))
      enabled.store((bool)obj->getProperty("enabled"));
    else if (!preserveEnabledIfMissing)
      enabled.store(false);
    if (obj->hasProperty("presetName"))
      presetName = obj->getProperty("presetName").toString();
    if (obj->hasProperty("presetProvenance"))
      presetProvenance = obj->getProperty("presetProvenance").toString();
    if (obj->hasProperty("presetCategory"))
      presetCategory = obj->getProperty("presetCategory").toString();
    presetDirty.store(obj->hasProperty("presetDirty")
                          ? (bool)obj->getProperty("presetDirty")
                          : false);
    if (obj->hasProperty("mode"))
      engine_.setMode((FanfareFm::FmMode)(int)obj->getProperty("mode"));
    if (obj->hasProperty("volume"))
      volume.store((float)(double)obj->getProperty("volume"));
    FanfareFm::FmPatch p;
    if (patchFromState(obj->getProperty("patch"), p))
      engine_.setPatch(p);
    if (auto *fxObj = obj->getProperty("fx").getDynamicObject()) {
      for (int i = 0; i < (int)kNumFmFx; ++i) {
        if (fxObj->hasProperty(fxKeyName(i)))
          fx[(size_t)i].store((float)(double)fxObj->getProperty(fxKeyName(i)));
      }
    }
    ccMap_.clear();
    for (int i = 0; i < (int)kNumFmCcParams; ++i)
      ccParamAtomics[(size_t)i].store(-1, std::memory_order_relaxed);
    if (auto *cc = obj->getProperty("ccMap").getDynamicObject()) {
      for (const auto &kv : cc->getProperties()) {
        const juce::String key = kv.name.toString();
        const int val = (int)kv.value;
        ccMap_[key] = val;
        const int idx = ccParamIndexForId(key);
        if (idx >= 0)
          ccParamAtomics[(size_t)idx].store(val, std::memory_order_relaxed);
      }
    }
    syncCatalogIndex();
    return true;
  }

  juce::StringArray ccMappableParams() const {
    return {"volume", "feedback", "algorithm", "lfoRate", "lfoPmd", "lfoAmd",
            "transpose", "bendRange"};
  }
  int getCCForParam(const juce::String &id) const {
    const int idx = ccParamIndexForId(id);
    if (idx >= 0)
      return ccParamAtomics[(size_t)idx].load(std::memory_order_relaxed);
    auto it = ccMap_.find(id);
    return it != ccMap_.end() ? it->second : -1;
  }
  void setCCForParam(const juce::String &id, int cc) {
    const int idx = ccParamIndexForId(id);
    if (idx >= 0)
      ccParamAtomics[(size_t)idx].store(cc, std::memory_order_relaxed);
    if (cc < 0)
      ccMap_.erase(id);
    else
      ccMap_[id] = cc;
  }

  void processBlock(juce::AudioBuffer<float> &buffer,
                    const juce::MidiBuffer &midiMessages) {
    juce::ScopedNoDenormals noDenormals;
    const int numSamples = buffer.getNumSamples();
    if (numSamples <= 0)
      return;
    if ((int)scratchInt.size() < numSamples)
      scratchInt.resize((size_t)numSamples, 0);

    if (panicPending.exchange(false, std::memory_order_acq_rel))
      panicNow();

    mergedMidiScratch.clear();
    {
      juce::SpinLock::ScopedTryLockType sl(uiMidiLock);
      if (sl.isLocked() && !uiMidiQueue.isEmpty()) {
        mergedMidiScratch.addEvents(uiMidiQueue, 0, -1, 0);
        uiMidiQueue.clear();
      }
    }
    if (!midiMessages.isEmpty())
      mergedMidiScratch.addEvents(midiMessages, 0, -1, 0);

    for (const auto meta : mergedMidiScratch) {
      const auto msg = meta.getMessage();
      if (msg.isNoteOn() && msg.getVelocity() > 0) {
        const int note = juce::jlimit(0, 127, msg.getNoteNumber());
        if (sustainOn.load(std::memory_order_relaxed))
          heldNotes.set((size_t)note, true);
        engine_.noteOn(note, msg.getVelocity());
      } else if (msg.isNoteOff() || (msg.isNoteOn() && msg.getVelocity() == 0)) {
        const int note = juce::jlimit(0, 127, msg.getNoteNumber());
        if (sustainOn.load(std::memory_order_relaxed)) {
          heldNotes.set((size_t)note, true);
        } else {
          heldNotes.set((size_t)note, false);
          engine_.noteOff(note);
        }
      } else if (msg.isController()) {
        handleCC(msg.getControllerNumber(), msg.getControllerValue());
      } else if (msg.isPitchWheel()) {
        engine_.setPitchBend(msg.getPitchWheelValue());
      } else if (msg.isAllNotesOff() || msg.isAllSoundOff()) {
        engine_.allNotesOff();
        heldNotes.reset();
      }
    }

    engine_.render(scratchInt.data(), numSamples);
    const int32_t *out = scratchInt.data();

    if ((int)fxBuf.size() < 2 * numSamples)
      fxBuf.resize((size_t)2 * numSamples);
    float *fxL = fxBuf.data();
    float *fxR = fxBuf.data() + numSamples;
    const float pre = engine_.outScaleForCurrentMode();
    for (int i = 0; i < numSamples; ++i) {
      const float s = (float)out[i] * pre;
      fxL[i] = s;
      fxR[i] = s;
    }

    smChoMix.setTarget(fx[fxChoMix].load(std::memory_order_relaxed));
    chorus_.process(
        fxL, fxR, numSamples,
        (int)std::lround(fx[fxChoMode].load(std::memory_order_relaxed)),
        fx[fxChoRate].load(std::memory_order_relaxed),
        fx[fxChoDepth].load(std::memory_order_relaxed), smChoMix);

    smDlyMix.setTarget(fx[fxDlyMix].load(std::memory_order_relaxed));
    delay_.process(fxL, fxR, numSamples,
                   fx[fxDlyTime].load(std::memory_order_relaxed),
                   fx[fxDlyFb].load(std::memory_order_relaxed),
                   fx[fxDlyDamp].load(std::memory_order_relaxed), smDlyMix);

    smRevMix.setTarget(fx[fxRevMix].load(std::memory_order_relaxed));
    const float revWet = smRevMix.advanceBlock(numSamples);
    reverb_.processStereo(
        fxL, fxR, numSamples,
        juce::jlimit(
            0, 2,
            (int)std::lround(fx[fxRevType].load(std::memory_order_relaxed))),
        fx[fxRevSize].load(std::memory_order_relaxed),
        fx[fxRevDamp].load(std::memory_order_relaxed), revWet,
        1.0f - 0.28f * revWet);

    smVol.setTarget(volume.load(std::memory_order_relaxed));
    float blockPeakL = 0.0f;
    float blockPeakR = 0.0f;
    for (int i = 0; i < numSamples; ++i) {
      const float v = smVol.next();
      float l = softCeilingLimit(fxL[i] * v);
      float r = softCeilingLimit(fxR[i] * v);
      if (!std::isfinite(l))
        l = 0.0f;
      if (!std::isfinite(r))
        r = 0.0f;
      blockPeakL = juce::jmax(blockPeakL, std::abs(l));
      blockPeakR = juce::jmax(blockPeakR, std::abs(r));
      if (buffer.getNumChannels() > 0)
        buffer.addSample(0, i, l);
      if (buffer.getNumChannels() > 1)
        buffer.addSample(1, i, r);
    }
    const float decL = peakOutL.load(std::memory_order_relaxed) * 0.86f;
    const float decR = peakOutR.load(std::memory_order_relaxed) * 0.86f;
    peakOutL.store(juce::jmax(blockPeakL, decL), std::memory_order_relaxed);
    peakOutR.store(juce::jmax(blockPeakR, decR), std::memory_order_relaxed);
    activeVoicesCount.store(engine_.activeVoices(), std::memory_order_relaxed);
  }

private:
  // Transparent ceiling limiter: 100% linear below 0.85f (-1.4 dBFS) so FM
  // bell/tine partials suffer zero harmonic intermodulation, smoothly rounding
  // extreme multi-voice transients above 0.85f asymptotically toward 1.0f.
  static inline float softCeilingLimit(float x) noexcept {
    constexpr float kKnee = 0.85f;
    constexpr float kHeadroom = 1.0f - kKnee;
    const float ax = std::abs(x);
    if (ax <= kKnee)
      return x;
    const float shaped =
        kKnee + kHeadroom * FanfareSynth::Dsp::fastTanhf((ax - kKnee) / kHeadroom);
    return x < 0.0f ? -shaped : shaped;
  }

  static const char *fxKeyName(int idx) noexcept {
    static const char *kFxNames[kNumFmFx] = {"choMode",  "choRate", "choDepth",
                                             "choMix",   "dlyTime", "dlyFb",
                                             "dlyDamp",  "dlyMix",  "revType",
                                             "revSize",  "revDamp", "revMix"};
    return kFxNames[juce::jlimit(0, (int)kNumFmFx - 1, idx)];
  }

  void loadDefaultFactoryVoice() {
    int count = 0;
    const FanfareFm::FmFactoryPreset *list = FanfareFm::factoryFmPresets(count);
    if (count <= 0 || list == nullptr)
      return;
    int chosen = 0;
    for (int i = 0; i < count; ++i) {
      if (std::strcmp(list[i].name, "E.PIANO 1") == 0 &&
          std::strcmp(list[i].provenance, "DX7") == 0) {
        chosen = i;
        break;
      }
    }
    FanfareFm::FmPatch p{};
    std::memcpy(p.d, list[chosen].patchBytes, FanfareFm::kPatchSize);
    engine_.loadPatchRaw(p);
    for (int k = 0; k < (int)kNumFmFx; ++k)
      fx[(size_t)k].store(list[chosen].fx[k], std::memory_order_relaxed);
    presetName = juce::String::fromUTF8(list[chosen].name);
    presetProvenance = juce::String::fromUTF8(list[chosen].provenance);
    presetCategory = juce::String::fromUTF8(list[chosen].category);
    currentPresetIndex = chosen;
    presetDirty.store(false, std::memory_order_relaxed);
  }

  void syncCatalogIndex() {
    currentPresetIndex = -1;
    for (int i = 0; i < (int)catalog.size(); ++i) {
      if (catalog[(size_t)i].name == presetName &&
          (presetProvenance.isEmpty() ||
           catalog[(size_t)i].provenance == presetProvenance)) {
        currentPresetIndex = i;
        if (presetProvenance.isEmpty())
          presetProvenance = catalog[(size_t)i].provenance;
        if (presetCategory.isEmpty())
          presetCategory = catalog[(size_t)i].category;
        return;
      }
    }
    for (int i = 0; i < (int)catalog.size(); ++i) {
      if (catalog[(size_t)i].name == presetName) {
        currentPresetIndex = i;
        return;
      }
    }
  }

  static bool patchFromState(const juce::var &v, FanfareFm::FmPatch &p) {
    if (!v.isString())
      return false;
    juce::MemoryOutputStream mos;
    if (!juce::Base64::convertFromBase64(mos, v.toString()))
      return false;
    const juce::MemoryBlock mb = mos.getMemoryBlock();
    if (mb.getSize() != (size_t)FanfareFm::kPatchSize)
      return false;
    std::memcpy(p.d, mb.getData(), FanfareFm::kPatchSize);
    return true;
  }

  void handleCC(int cc, int value) {
    switch (cc) {
    case 1:
      engine_.setModWheel(value);
      break;
    case 7:
      setVolume(value / 127.0f);
      break;
    case 64:
      sustainOn.store(value >= 64, std::memory_order_relaxed);
      if (value < 64) {
        for (size_t n = 0; n < 128; ++n) {
          if (heldNotes.test(n))
            engine_.noteOff((int)n);
        }
        heldNotes.reset();
      }
      break;
    case 120:
    case 123:
      engine_.allNotesOff();
      heldNotes.reset();
      break;
    default:
      break;
    }
    const float norm = value / 127.0f;
    bool patchModified = false;
    auto &p = engine_.patch();
    for (int i = 0; i < (int)kNumFmCcParams; ++i) {
      if (ccParamAtomics[(size_t)i].load(std::memory_order_relaxed) != cc)
        continue;
      switch (i) {
      case ccParamVolume:
        setVolume(norm);
        break;
      case ccParamFeedback:
        p.d[135] = (uint8_t)(norm * 7.0f);
        patchModified = true;
        break;
      case ccParamAlgorithm: {
        const int mx = FanfareFm::fmModeMaxAlg(engine_.mode());
        p.d[134] = (uint8_t)juce::jlimit(0, mx - 1, (int)(norm * mx));
        patchModified = true;
        break;
      }
      case ccParamLfoRate:
        p.d[137] = (uint8_t)(norm * 99.0f);
        patchModified = true;
        break;
      case ccParamLfoPmd:
        p.d[139] = (uint8_t)(norm * 99.0f);
        patchModified = true;
        break;
      case ccParamLfoAmd:
        p.d[140] = (uint8_t)(norm * 99.0f);
        patchModified = true;
        break;
      case ccParamTranspose:
        p.d[144] = (uint8_t)juce::jlimit(0, 48, (int)(norm * 48.0f));
        patchModified = true;
        break;
      default:
        break;
      }
    }
    if (patchModified)
      engine_.setPatch(p);
  }

  FanfareFm::FmEngine engine_;
  double currentSampleRate = 44100.0;
  int maxBlock = 8192;
  std::vector<int32_t> scratchInt;
  std::vector<float> fxBuf;
  std::array<std::atomic<float>, kNumFmFx> fx{};
  std::array<std::atomic<int>, kNumFmCcParams> ccParamAtomics{};
  FanfareSynth::Dsp::JunoChorus chorus_;
  FanfareSynth::Dsp::PingPongDelay delay_;
  FanfareDSP::LushReverb reverb_;
  FanfareSynth::Dsp::OnePoleSmoother smChoMix, smDlyMix, smRevMix, smVol;
  std::atomic<float> peakOutL{0.0f}, peakOutR{0.0f};
  std::atomic<int> activeVoicesCount{0};
  std::atomic<bool> panicPending{false};
  std::atomic<bool> sustainOn{false};
  std::atomic<float> volume{0.8f};
  juce::SpinLock uiMidiLock;
  juce::MidiBuffer uiMidiQueue;
  juce::MidiBuffer mergedMidiScratch;
  juce::String presetName{"E.PIANO 1"};
  juce::String presetProvenance{"DX7"};
  juce::String presetCategory{"Electric Piano & Clav"};
  int currentPresetIndex = -1;
  std::atomic<bool> presetDirty{false};
  std::vector<PresetEntry> catalog;
  std::vector<PresetEntry> importedBank_;
  std::map<juce::String, int> ccMap_;
  std::bitset<128> heldNotes;
};
