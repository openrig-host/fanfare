#pragma once

#include <JuceHeader.h>
#include <array>
#include <atomic>
#include <bitset>
#include <cmath>
#include <vector>

#include "B3TonewheelEngine.h"
#include "B3ScannerChorus.h"
#include "B3Preamp.h"
#include "B3Leslie122.h"
#include "B3MasterFx.h"
#include "../../ChannelStripProcessor.h"
#include "../../FanfareConstants.h"
#include "../../Logger.h"

// Top-level B3 organ processor: tonewheel engine -> scanner chorus ->
// AO-28 preamp (swell + 2x tube drive) -> Stereo Leslie 122 -> volume.
// Mirrors the RackSlot interface surface of FmSynthProcessor.

class B3OrganProcessor {
public:
  std::atomic<bool> enabled{false};

  struct PresetEntry {
    juce::String name;
    juce::String category;
    juce::String provenance;
    bool isUser = false;
    juce::File file;
  };

  enum CcProfile : int {
    CcAuto = 0,     // CK88 (77-83,85-86) + Faders (21-29) + Classic (12-20)
    CcCk88 = 1,     // Yamaha CK88 only (77-83,85-86)
    CcFaders = 2,   // Faders (21-29)
    CcSound = 3,    // Drawbar/controller bank (70-78)
    CcClassic = 4,  // Classic clonewheel (12-20)
    CcCustom = 5    // User-learned named drawbar CC map
  };

  struct DrawbarCcMap {
    juce::String name;
    std::array<int, 9> ccs{{-1, -1, -1, -1, -1, -1, -1, -1, -1}};
    bool invert = false;
  };

  enum ExtSlot : int {
    ExtSat = 0,     // External Saturation / Compressor VST3 (pre-Leslie)
    ExtLeslie = 1,  // External Rotary Speaker VST3 (replaces native Leslie)
    kNumExtSlots = 2
  };

  B3OrganProcessor() {
    // C++17 std::atomic default-init leaves indeterminate values; seed the
    // custom drawbar-CC slots to "unassigned" before any map could be applied.
    for (auto &a : customDrawbarCc)
      a.store(-1, std::memory_order_relaxed);
    uiMidiQueue.ensureSize(2048);
    mergedMidi.ensureSize(4096);
    loadPresetByName("vibe coda");
    presetDirty.store(false);
    refreshPresetCatalog();
    refreshCustomCcMaps();
  }

  void prepare(double sampleRate, int maxBlockSize = 8192) {
    sr = sampleRate > 8000.0 ? sampleRate : 44100.0;
    maxBlock = juce::jmax(1, maxBlockSize);
    wheels.prepare(sr);
    scanner.prepare(sr);
    preamp.prepare(sr);
    leslie.prepare(sr);
    masterFx.prepare(sr);
    const int scratchLen = juce::jmax(maxBlock, 8192);
    scratchCapacity = scratchLen;
    woodRoomIr.prepare(sr, maxBlock);
    woodRoomIr.loadPresetByIndex(0); // Built-In: Studio Wood Room (0.55s)
    woodIrScratch.setSize(2, scratchLen, false, false, true);
    // Reserve 2x headroom so the defensive resize in processBlock stays within
    // capacity and never mallocs on the audio thread for realistic block sizes.
    scratch.reserve((size_t)scratchLen * 2);
    scratchR.reserve((size_t)scratchLen * 2);
    scratchPerc.reserve((size_t)scratchLen * 2);
    scratch.assign((size_t)scratchLen, 0.0f);
    scratchR.assign((size_t)scratchLen, 0.0f);
    scratchPerc.assign((size_t)scratchLen, 0.0f);
    extStereoBuf.setSize(extBufChannels, scratchLen, false, false, true);
    extMidiScratch.ensureSize(2048);
    for (int i = 0; i < kNumExtSlots; ++i) {
      juce::SpinLock::ScopedLockType sl(extLock);
      if (extSlots[i].plugin != nullptr)
        extSlots[i].plugin->prepareToPlay(sr, maxBlock);
    }
    applyAllParams();
    reset();
  }

  void reset() {
    physHeld.reset();
    sustHeld.reset();
    pedalDown = false;
    wheels.reset();
    scanner.reset();
    preamp.reset();
    leslie.reset();
    masterFx.reset();
    woodRoomIr.reset();
    volSmoothed = volume.load();
    peakL.store(0.0f);
    peakR.store(0.0f);
  }

  void panicNow() {
    physHeld.reset();
    sustHeld.reset();
    pedalDown = false;
    wheels.panic();
  }

  void allNotesOff() { requestPanic(); }
  void requestPanic() { panicPending.store(true, std::memory_order_release); }
  void allNotesOffPendingFlag() { allNotesOffPending.store(true, std::memory_order_release); }

  int getActiveVoices() const { return wheels.activeVoices(); }
  float getOutputPeakL() const { return peakL.load(std::memory_order_relaxed); }
  float getOutputPeakR() const { return peakR.load(std::memory_order_relaxed); }
  float getHornAngle() const { return leslie.getHornAngle(); }
  float getDrumAngle() const { return leslie.getDrumAngle(); }
  float getHornAngleB() const { return leslie.getHornAngleB(); }
  float getDrumAngleB() const { return leslie.getDrumAngleB(); }

  void injectNote(int note, bool on, float vel = 0.85f) {
    const int n = juce::jlimit(0, 127, note);
    juce::MidiMessage m = on
        ? juce::MidiMessage::noteOn(1, n,
              (juce::uint8)juce::jlimit(1, 127, (int)(vel * 127.0f)))
        : juce::MidiMessage::noteOff(1, n);
    juce::SpinLock::ScopedLockType sl(uiMidiLock);
    uiMidiQueue.addEvent(m, 0);
  }

  void setUiModWheel(float norm) {
    uiModWheelPending.store(juce::jlimit(0, 127, (int)std::lround(norm * 127.0f)),
                            std::memory_order_release);
  }

  void setUiPitchBend(float bipolar) {
    uiPitchBendPending.store(8192 + (int)(bipolar * 8191.0f),
                             std::memory_order_release);
  }

  juce::String getRegistrationString() const {
    juce::String s;
    for (int i = 0; i < 9; ++i) {
      if (i == 3 || i == 6) // Hammond 3+3+3 registration grouping
        s << ' ';
      s << juce::String(juce::jlimit(0, 8, (int)std::lround(getDrawbar(i) * 8.0f)));
    }
    return s;
  }

  // ---- Presets ----
  juce::String getCurrentPresetName() const { return presetName; }
  juce::String getCurrentPresetProvenance() const { return "B3"; }
  juce::String getCurrentPresetCategory() const { return "Organ"; }
  int getCurrentPresetIndex() const { return currentPresetIndex; }
  bool isPresetDirty() const { return presetDirty.load(); }
  void markDirty() { presetDirty.store(true); }
  void setPresetDirty(bool d) { presetDirty.store(d); }

  const std::vector<PresetEntry> &getPresetCatalog() const { return catalog; }

  void refreshPresetCatalog() {
    catalog.clear();
    auto dir = userPresetDir();
    if (dir.isDirectory()) {
      auto files = dir.findChildFiles(juce::File::findFiles, false, "*.json");
      files.sort();
      for (const auto &f : files) {
        PresetEntry e;
        e.name = f.getFileNameWithoutExtension();
        e.category = "User";
        e.provenance = "USER";
        e.isUser = true;
        e.file = f;
        catalog.push_back(e);
      }
    }
    for (const char *n : kFactoryPresetNames) {
      PresetEntry e;
      e.name = n;
      e.category = "Factory";
      e.provenance = "B3";
      catalog.push_back(e);
    }
    syncCatalogIndex();
  }

  void loadPresetAtIndex(int idx) {
    if (idx < 0 || idx >= (int)catalog.size())
      return;
    const auto &e = catalog[(size_t)idx];
    if (e.isUser && e.file.existsAsFile()) {
      auto v = juce::JSON::parse(e.file);
      if (v.isObject()) {
        auto state = v.getProperty("state", juce::var());
        if (state.isObject()) {
          applyState(state, true);
          presetName = v.getProperty("presetName", e.name).toString();
          presetDirty.store(false);
          syncCatalogIndex();
          return;
        }
      }
    }
    loadPresetByName(e.name);
  }

  void loadPresetByName(const juce::String &name);

  static juce::File userPresetDir() {
    return FanfareConstants::getAppDirectory().getChildFile("b3-presets");
  }

  bool saveUserPreset(const juce::String &name) {
    auto dir = userPresetDir();
    if (!dir.isDirectory() && !dir.createDirectory())
      return false;
    auto *obj = new juce::DynamicObject();
    obj->setProperty("presetName", name);
    obj->setProperty("state", getState());
    auto file = dir.getChildFile(juce::File::createLegalFileName(name) + ".json");
    presetName = name;
    presetDirty.store(false);
    return file.replaceWithText(juce::JSON::toString(juce::var(obj)));
  }

  // ---- State ----
  juce::var getState() const;
  void applyState(const juce::var &v, bool preserveEnabledIfMissing = true);
  juce::var toVar() const { return getState(); }
  void fromVar(const juce::var &v) { applyState(v); }

  // ---- Audio ----
  void processBlock(juce::AudioBuffer<float> &buffer,
                    const juce::MidiBuffer &midiMessages);

  // ---- Parameter setters (called from UI / MIDI) ----
  void setDrawbar(int idx, float v) {
    drawbar[(size_t)juce::jlimit(0, 8, idx)].store(
        juce::jlimit(0.0f, 1.0f, v));
    presetDirty.store(true);
    paramsDirty.store(true);
  }
  float getDrawbar(int idx) const {
    return drawbar[(size_t)juce::jlimit(0, 8, idx)].load();
  }

  void setPercOn(bool v)    { percOn.store(v);    presetDirty.store(true); paramsDirty.store(true); }
  void setPercSoft(bool v)  { percSoft.store(v);  presetDirty.store(true); paramsDirty.store(true); }
  void setPercFast(bool v)  { percFast.store(v);  presetDirty.store(true); paramsDirty.store(true); }
  void setPercThird(bool v) { percThird.store(v); presetDirty.store(true); paramsDirty.store(true); }
  void setScannerMode(int v){ scannerMode.store(juce::jlimit(0, 6, v)); presetDirty.store(true); paramsDirty.store(true); }
  void setDrive(float v)    { drive.store(juce::jlimit(0.0f, 1.72f, v));  presetDirty.store(true); paramsDirty.store(true); }
  void setTone(float v)     { tone.store(juce::jlimit(0.0f, 1.0f, v));   presetDirty.store(true); paramsDirty.store(true); }
  void setSwell(float v)    { swell.store(juce::jlimit(0.0f, 1.0f, v)); }
  void setVolume(float v)   { volume.store(juce::jlimit(0.0f, 1.0f, v)); }
  void setClickDepth(float v) { clickDepth.store(juce::jlimit(0.0f, 1.0f, v)); presetDirty.store(true); paramsDirty.store(true); }
  void setLeakage(float v)    { leakage.store(juce::jlimit(0.0f, 0.066f, v));   presetDirty.store(true); paramsDirty.store(true); }
  void setLeslieMode(int v)   {
    const int m = juce::jlimit(0, 2, v);
    leslieMode.store(m);
    uiLeslieModePending.store(m, std::memory_order_release);
    // Brake has no standard CC: forward CC1=0 (stop/slow) to external rotary
    // plugins so Brake isn't read as Fast (>=64). The native Leslie receives
    // the true Brake mode via uiLeslieModePending above.
    uiModWheelPending.store(m == 2 ? 127 : 0, std::memory_order_release);
    presetDirty.store(true);
    paramsDirty.store(true);
  }
  void setLeslieBypass(bool v){ leslieBypass.store(v); presetDirty.store(true); paramsDirty.store(true); }
  void setLeslieDual(bool v)  { leslieDual.store(v); presetDirty.store(true); paramsDirty.store(true); }
  void setLeslieCab(int v)    { leslieCab.store(juce::jlimit(0, 3, v)); presetDirty.store(true); paramsDirty.store(true); }
  void setScannerDepth(float v) { scannerDepth.store(juce::jlimit(0.0f, 1.0f, v)); presetDirty.store(true); paramsDirty.store(true); }
  void setRoomMix(float v)      { roomMix.store(juce::jlimit(0.0f, 1.0f, v)); presetDirty.store(true); paramsDirty.store(true); }
  void setLeslieWarmth(float v) { leslieWarmth.store(juce::jlimit(0.0f, 1.0f, v)); presetDirty.store(true); paramsDirty.store(true); }
  void setLeslieChorus(float v) { leslieChorus.store(juce::jlimit(0.0f, 1.0f, v)); presetDirty.store(true); paramsDirty.store(true); }
  void setMicSpread(float v)  { micSpread.store(juce::jlimit(0.0f, 1.0f, v)); presetDirty.store(true); paramsDirty.store(true); }
  void setEqLowDb(float v)    { eqLowDb.store(juce::jlimit(-9.0f, 9.0f, v)); presetDirty.store(true); paramsDirty.store(true); }
  void setEqMidDb(float v)    { eqMidDb.store(juce::jlimit(-9.0f, 9.0f, v)); presetDirty.store(true); paramsDirty.store(true); }
  void setEqHighDb(float v)   { eqHighDb.store(juce::jlimit(-9.0f, 9.0f, v)); presetDirty.store(true); paramsDirty.store(true); }
  void setCompAmount(float v) { compAmount.store(juce::jlimit(0.0f, 1.0f, v)); presetDirty.store(true); paramsDirty.store(true); }
  void setReverbMix(float v)  { reverbMix.store(juce::jlimit(0.0f, 1.0f, v)); presetDirty.store(true); paramsDirty.store(true); }
  void setDelayMix(float v)   { delayMix.store(juce::jlimit(0.0f, 1.0f, v)); presetDirty.store(true); paramsDirty.store(true); }
  void setInvertDrawbars(bool v)        { invertDrawbars.store(v); }
  void setOctaveUp(bool v)              { octaveUp.store(v); allNotesOffPendingFlag(); }
  void setSustainControlsLeslie(bool v) { sustainControlsLeslie.store(v); }
  void setCcProfile(int v)              { ccProfile.store(juce::jlimit(0, (int)CcCustom, v)); }

  static juce::File customCcMapsFile() {
    return FanfareConstants::getAppDirectory().getChildFile("b3-cc-maps.json");
  }

  void refreshCustomCcMaps() {
    customCcMaps.clear();
    auto file = customCcMapsFile();
    if (file.existsAsFile()) {
      auto v = juce::JSON::parse(file);
      if (auto *arr = v.getArray()) {
        for (const auto &item : *arr) {
          if (auto *obj = item.getDynamicObject()) {
            DrawbarCcMap m;
            m.name = obj->getProperty("name").toString().trim();
            m.invert = (bool)obj->getProperty("invert");
            if (auto *ccArr = obj->getProperty("ccs").getArray()) {
              for (int i = 0; i < 9 && i < ccArr->size(); ++i) {
                const int c = (int)(*ccArr)[i];
                m.ccs[(size_t)i] = (c >= 0 && c <= 127) ? c : -1;
              }
            }
            if (m.name.isNotEmpty())
              customCcMaps.push_back(m);
          }
        }
      }
    }
    if (ccProfile.load() == CcCustom && customCcMapName.isNotEmpty())
      applyCustomCcMapByName(customCcMapName, false);
  }

  const std::vector<DrawbarCcMap> &getCustomCcMaps() const { return customCcMaps; }
  const juce::String &getCustomCcMapName() const { return customCcMapName; }

  bool selectCustomCcMap(const juce::String &name) {
    return applyCustomCcMapByName(name, true);
  }

  bool saveOrUpdateCustomCcMap(const DrawbarCcMap &entry) {
    if (entry.name.trim().isEmpty())
      return false;
    refreshCustomCcMaps();
    DrawbarCcMap clean = entry;
    clean.name = clean.name.trim();
    bool replaced = false;
    for (auto &m : customCcMaps) {
      if (m.name.equalsIgnoreCase(clean.name)) {
        m = clean;
        replaced = true;
        break;
      }
    }
    if (!replaced)
      customCcMaps.push_back(clean);
    if (!writeCustomCcMapsFile())
      return false;
    return applyCustomCcMapByName(clean.name, true);
  }

  bool deleteCustomCcMap(const juce::String &name) {
    refreshCustomCcMaps();
    const auto oldSize = customCcMaps.size();
    customCcMaps.erase(
        std::remove_if(customCcMaps.begin(), customCcMaps.end(),
                       [&](const DrawbarCcMap &m) { return m.name.equalsIgnoreCase(name); }),
        customCcMaps.end());
    if (customCcMaps.size() == oldSize)
      return false;
    writeCustomCcMapsFile();
    if (customCcMapName.equalsIgnoreCase(name)) {
      customCcMapName.clear();
      for (auto &a : customDrawbarCc)
        a.store(-1);
      ccProfile.store(CcAuto);
    }
    return true;
  }

  uint32_t getLastCcSeq() const { return lastCcSeq.load(std::memory_order_acquire); }
  int getLastCcNum() const { return lastCcNum.load(std::memory_order_relaxed); }

  // ---- External VST3 insert slots (ExtSat = 0, ExtLeslie = 1) ----
  std::unique_ptr<juce::AudioPluginInstance> swapExtPlugin(
      int extSlot, std::unique_ptr<juce::AudioPluginInstance> newPlugin,
      const juce::String &path, const juce::String &name) {
    if (extSlot < 0 || extSlot >= kNumExtSlots)
      return newPlugin;
    if (newPlugin != nullptr) {
      newPlugin->setRateAndBufferSizeDetails(sr, maxBlock);
      newPlugin->prepareToPlay(sr, maxBlock);
    }
    juce::SpinLock::ScopedLockType sl(extLock);
    // Pre-size the RT insert buffer for this plugin's channel count here on the
    // message thread so runExtSlot never has to grow it on the audio thread.
    if (newPlugin != nullptr) {
      const int nCh = juce::jmax(2, juce::jmax(newPlugin->getTotalNumInputChannels(),
                                               newPlugin->getTotalNumOutputChannels()));
      if (nCh > extBufChannels || extStereoBuf.getNumSamples() < scratchCapacity) {
        extBufChannels = juce::jmax(extBufChannels, nCh);
        extStereoBuf.setSize(extBufChannels, scratchCapacity, false, false, true);
      }
    }
    auto old = std::move(extSlots[extSlot].plugin);
    extSlots[extSlot].plugin = std::move(newPlugin);
    extSlots[extSlot].path = path;
    extSlots[extSlot].name = name;
    extSlots[extSlot].pendingPath.clear();
    extSlots[extSlot].pendingName.clear();
    extSlots[extSlot].pendingStateB64.clear();
    return old;
  }

  juce::AudioPluginInstance *getExtPluginInstance(int extSlot) const {
    if (extSlot < 0 || extSlot >= kNumExtSlots)
      return nullptr;
    juce::SpinLock::ScopedLockType sl(extLock);
    return extSlots[extSlot].plugin.get();
  }

  juce::String getExtPluginName(int extSlot) const {
    if (extSlot < 0 || extSlot >= kNumExtSlots)
      return {};
    juce::SpinLock::ScopedLockType sl(extLock);
    if (extSlots[extSlot].plugin != nullptr)
      return extSlots[extSlot].name.isNotEmpty()
                 ? extSlots[extSlot].name
                 : extSlots[extSlot].plugin->getName();
    return {};
  }

  juce::String getExtPluginPath(int extSlot) const {
    if (extSlot < 0 || extSlot >= kNumExtSlots)
      return {};
    juce::SpinLock::ScopedLockType sl(extLock);
    return extSlots[extSlot].path;
  }

  bool isExtPluginBypassed(int extSlot) const {
    return (extSlot >= 0 && extSlot < kNumExtSlots)
               ? extSlots[extSlot].bypassed.load(std::memory_order_relaxed)
               : false;
  }

  void setExtPluginBypassed(int extSlot, bool b) {
    if (extSlot >= 0 && extSlot < kNumExtSlots)
      extSlots[extSlot].bypassed.store(b, std::memory_order_relaxed);
  }

  bool hasPendingExtPlugin(int extSlot) const {
    if (extSlot < 0 || extSlot >= kNumExtSlots)
      return false;
    juce::SpinLock::ScopedLockType sl(extLock);
    return extSlots[extSlot].pendingPath.isNotEmpty();
  }

  bool consumePendingExtPlugin(int extSlot, juce::String &outPath,
                               juce::String &outName, juce::String &outB64) {
    if (extSlot < 0 || extSlot >= kNumExtSlots)
      return false;
    juce::SpinLock::ScopedLockType sl(extLock);
    if (extSlots[extSlot].pendingPath.isEmpty())
      return false;
    outPath = extSlots[extSlot].pendingPath;
    outName = extSlots[extSlot].pendingName;
    outB64 = extSlots[extSlot].pendingStateB64;
    extSlots[extSlot].pendingPath.clear();
    extSlots[extSlot].pendingName.clear();
    extSlots[extSlot].pendingStateB64.clear();
    return true;
  }

  static int mapCCToDrawbarIndex(int cc, int profile) {
    // Yamaha CK88 drawbar CCs: 77, 78, 79, 80, 81, 82, 83, 85, 86
    if (profile == CcAuto || profile == CcCk88) {
      switch (cc) {
      case FanfareConstants::kCk88Drawbar16CC:  return 0;
      case FanfareConstants::kCk88Drawbar513CC: return 1;
      case FanfareConstants::kCk88Drawbar8CC:   return 2;
      case FanfareConstants::kCk88Drawbar4CC:   return 3;
      case FanfareConstants::kCk88Drawbar223CC: return 4;
      case FanfareConstants::kCk88Drawbar2CC:   return 5;
      case FanfareConstants::kCk88Drawbar135CC: return 6;
      case FanfareConstants::kCk88Drawbar113CC: return 7;
      case FanfareConstants::kCk88Drawbar1CC:   return 8;
      default: break;
      }
    }
    if ((profile == CcAuto || profile == CcFaders) && cc >= 21 && cc <= 29)
      return cc - 21;
    if ((profile == CcAuto || profile == CcClassic) && cc >= 12 && cc <= 20)
      return cc - 12;
    if (profile == CcSound && cc >= 70 && cc <= 78)
      return cc - 70;
    return -1;
  }

  // Read accessors for UI
  std::atomic<bool> percOn{true};
  std::atomic<bool> percSoft{true};
  std::atomic<bool> percFast{true};
  std::atomic<bool> percThird{false};
  std::atomic<int>  scannerMode{4};
  std::atomic<float> drive{0.86f};
  std::atomic<float> tone{0.88f};
  std::atomic<float> swell{0.92f};
  std::atomic<float> volume{0.60f};
  std::atomic<float> clickDepth{0.90f};
  std::atomic<float> leakage{0.033f};
  std::atomic<int>  leslieMode{0};
  std::atomic<bool> leslieBypass{false};
  std::atomic<bool> leslieDual{true};
  std::atomic<int> leslieCab{0};
  std::atomic<float> scannerDepth{0.28f};
  std::atomic<float> roomMix{0.84f};
  std::atomic<float> leslieWarmth{0.80f};
  std::atomic<float> leslieChorus{0.40f};
  std::atomic<float> micSpread{0.85f};
  std::atomic<float> eqLowDb{1.5f};
  std::atomic<float> eqMidDb{0.0f};
  std::atomic<float> eqHighDb{-0.5f};
  std::atomic<float> compAmount{0.28f};
  std::atomic<float> reverbMix{0.20f};
  std::atomic<float> delayMix{0.22f};
  std::atomic<bool> invertDrawbars{false};
  std::atomic<bool> octaveUp{false};
  std::atomic<bool> sustainControlsLeslie{false};
  std::atomic<int>  ccProfile{CcAuto};
  std::array<std::atomic<int>, 9> customDrawbarCc{};
  std::atomic<uint32_t> lastCcSeq{0};
  std::atomic<int> lastCcNum{-1};

private:
  std::vector<DrawbarCcMap> customCcMaps;
  juce::String customCcMapName;

  bool writeCustomCcMapsFile() const {
    auto file = customCcMapsFile();
    auto dir = file.getParentDirectory();
    if (!dir.isDirectory() && !dir.createDirectory())
      return false;
    juce::Array<juce::var> arr;
    for (const auto &m : customCcMaps) {
      auto *obj = new juce::DynamicObject();
      obj->setProperty("name", m.name);
      obj->setProperty("invert", m.invert);
      juce::Array<juce::var> ccArr;
      for (int i = 0; i < 9; ++i)
        ccArr.add(m.ccs[(size_t)i]);
      obj->setProperty("ccs", ccArr);
      arr.add(juce::var(obj));
    }
    return file.replaceWithText(juce::JSON::toString(juce::var(arr)));
  }

  bool applyCustomCcMapByName(const juce::String &name, bool applyInvert) {
    for (const auto &m : customCcMaps) {
      if (m.name.equalsIgnoreCase(name)) {
        customCcMapName = m.name;
        for (int i = 0; i < 9; ++i)
          customDrawbarCc[(size_t)i].store(m.ccs[(size_t)i]);
        ccProfile.store(CcCustom);
        if (applyInvert)
          invertDrawbars.store(m.invert);
        return true;
      }
    }
    return false;
  }
  static inline constexpr const char *kFactoryPresetNames[] = {
      "vibe coda",
      "Jimmy Jazz 888000000",
      "Full Gospel 888888888",
      "Green Onions",
      "Whiter Shade",
      "Highway Star Grit",
      "Whipping Post",
      "Gimme Some Lovin'",
      "Soul Sacrifice",
      "Joey D Bebop",
      "Reggae Bubble",
      "Midnight Ballad",
      "Cathedral Majesty"};

  double sr = 44100.0;
  int maxBlock = 8192;
  int scratchCapacity = 8192;
  int extBufChannels = 2;
  int idleSilenceSamples = 0;

  B3::TonewheelEngine wheels;
  B3::ScannerChorus scanner;
  B3::Preamp preamp;
  B3::Leslie122 leslie;
  B3::MasterFx masterFx;
  FanfareDSP::ConvolutionReverb woodRoomIr;
  juce::AudioBuffer<float> woodIrScratch;

  struct ExtPluginSlot {
    std::unique_ptr<juce::AudioPluginInstance> plugin;
    juce::String path, name;
    std::atomic<bool> bypassed{false};
    juce::String pendingPath, pendingName, pendingStateB64;
  };
  mutable juce::SpinLock extLock;
  std::array<ExtPluginSlot, kNumExtSlots> extSlots;
  juce::AudioBuffer<float> extStereoBuf;
  juce::MidiBuffer extMidiScratch;

  std::array<std::atomic<float>, 9> drawbar{};
  std::atomic<bool> paramsDirty{false};
  std::atomic<bool> panicPending{false};
  std::atomic<bool> allNotesOffPending{false};
  std::atomic<int> uiLeslieModePending{-1};
  std::atomic<int> uiModWheelPending{-1};
  std::atomic<int> uiPitchBendPending{-1};
  std::atomic<float> peakL{0.0f}, peakR{0.0f};

  float volSmoothed = 0.82f;
  std::bitset<128> physHeld;
  std::bitset<128> sustHeld;
  bool pedalDown = false;

  std::vector<float> scratch, scratchR, scratchPerc;

  juce::SpinLock uiMidiLock;
  juce::MidiBuffer uiMidiQueue;
  juce::MidiBuffer mergedMidi;

  juce::String presetName{"vibe coda"};
  int currentPresetIndex = 0;
  std::atomic<bool> presetDirty{false};
  std::vector<PresetEntry> catalog;

  void syncCatalogIndex() {
    currentPresetIndex = -1;
    for (size_t i = 0; i < catalog.size(); ++i) {
      if (catalog[i].name == presetName) {
        currentPresetIndex = (int)i;
        return;
      }
    }
  }

  void applyAllParams() {
    wheels.setPercussion(percOn.load(), percSoft.load(), percFast.load(),
                         percThird.load());
    wheels.setLeakage(leakage.load());
    wheels.setClickDepth(clickDepth.load());
    for (int i = 0; i < 9; ++i)
      wheels.setDrawbar(i, drawbar[(size_t)i].load());
    scanner.setMode(scannerMode.load());
    scanner.setDepth(scannerDepth.load());
    preamp.setDrive(drive.load());
    preamp.setTone(tone.load());
    preamp.setSwell(swell.load());
    leslie.setMode(leslieMode.load());
    leslie.setCabinetType(B3::Leslie122::Cab147Closed); // fixed voicing
    leslie.setBypass(leslieBypass.load());
    leslie.setDualMode(leslieDual.load());
    leslie.setMicSpread(micSpread.load());
    leslie.setRoomMix(roomMix.load());
    leslie.setWarmth(leslieWarmth.load());
    leslie.setChorusMix(leslieChorus.load());
    masterFx.setEq(eqLowDb.load(), eqMidDb.load(), eqHighDb.load());
    masterFx.setComp(compAmount.load());
    masterFx.setReverb(reverbMix.load());
    masterFx.setDelayMix(delayMix.load());
  }

  void handleMidiMessage(const juce::MidiMessage &msg) {
    const int octShift = octaveUp.load(std::memory_order_relaxed) ? 12 : 0;
    if (msg.isNoteOn() && msg.getVelocity() > 0) {
      const int note = msg.getNoteNumber() + octShift;
      if (note >= 0 && note <= 127) {
        physHeld.set((size_t)note, true);
        sustHeld.reset((size_t)note);
        wheels.noteOn(note, msg.getFloatVelocity());
      }
    } else if (msg.isNoteOff() || (msg.isNoteOn() && msg.getVelocity() == 0)) {
      const int rawNote = msg.getNoteNumber();
      for (int candidate : {rawNote + octShift, rawNote, rawNote + 12}) {
        if (candidate < 0 || candidate > 127)
          continue;
        const bool wasHeld = physHeld.test((size_t)candidate) || sustHeld.test((size_t)candidate);
        if (candidate != rawNote + octShift && !wasHeld)
          continue;
        physHeld.reset((size_t)candidate);
        if (pedalDown && !sustainControlsLeslie.load(std::memory_order_relaxed)) {
          sustHeld.set((size_t)candidate, true);
        } else {
          sustHeld.reset((size_t)candidate);
          wheels.noteOff(candidate);
        }
      }
    } else if (msg.isController()) {
      const int cc = msg.getControllerNumber();
      const int val = msg.getControllerValue();
      const float norm = juce::jlimit(0.0f, 1.0f, (float)val / 127.0f);
      lastCcNum.store(cc, std::memory_order_relaxed);
      lastCcSeq.fetch_add(1, std::memory_order_release);

      if (cc == 11) {
        swell.store(norm, std::memory_order_relaxed);
        preamp.setSwell(norm);
      } else if (cc == 7) {
        volume.store(norm, std::memory_order_relaxed);
      } else if (cc == 1 || cc == FanfareConstants::kCk88ModWheelCC) {
        const int mode = val >= 64 ? B3::Leslie122::Fast : B3::Leslie122::Slow;
        leslieMode.store(mode, std::memory_order_relaxed);
        leslie.setMode(mode);
      } else if (cc == FanfareConstants::kCk88SustainCC) {
        if (sustainControlsLeslie.load(std::memory_order_relaxed)) {
          const int mode = val >= 64 ? B3::Leslie122::Fast : B3::Leslie122::Slow;
          leslieMode.store(mode, std::memory_order_relaxed);
          leslie.setMode(mode);
        } else {
          const bool down = (val >= 64);
          if (pedalDown && !down) {
            for (int n = 0; n < 128; ++n) {
              if (sustHeld.test((size_t)n)) {
                sustHeld.reset((size_t)n);
                if (!physHeld.test((size_t)n))
                  wheels.noteOff(n);
              }
            }
          }
          pedalDown = down;
        }
      } else {
        const int prof = ccProfile.load(std::memory_order_relaxed);
        int dbIdx = -1;
        if (prof == CcCustom) {
          for (int i = 0; i < 9; ++i) {
            if (customDrawbarCc[(size_t)i].load(std::memory_order_relaxed) == cc) {
              dbIdx = i;
              break;
            }
          }
        } else {
          dbIdx = mapCCToDrawbarIndex(cc, prof);
        }
        if (dbIdx >= 0) {
          const float dbNorm =
              invertDrawbars.load(std::memory_order_relaxed)
                  ? juce::jlimit(0.0f, 1.0f, (float)(127 - val) / 127.0f)
                  : norm;
          drawbar[(size_t)dbIdx].store(dbNorm, std::memory_order_relaxed);
          wheels.setDrawbar(dbIdx, dbNorm);
          presetDirty.store(true, std::memory_order_relaxed);
        }
      }
    } else if (msg.isAllNotesOff() || msg.isAllSoundOff()) {
      panicNow();
    }
  }

  static float softCeiling(float x) {
    constexpr float kKnee = 0.88f;
    constexpr float kHeadroom = 1.0f - kKnee;
    const float ax = std::abs(x);
    if (ax <= kKnee)
      return x;
    const float shaped = kKnee + kHeadroom * std::tanh((ax - kKnee) / kHeadroom);
    return x < 0.0f ? -shaped : shaped;
  }
};

// ---- inline implementations ----

inline void B3OrganProcessor::loadPresetByName(const juce::String &name) {
  struct B3PresetSpec {
    const char *name;
    int d[9];
    bool percOn, percSoft, percFast, percThird;
    int scanner;
    float drive, tone, click, leakage;
    int leslieMode;
    bool dual;
    float spread;
  };

  static const B3PresetSpec kPresets[] = {
      {"vibe coda",              {7,8,6,1,2,1,0,0,7}, true,  true,  true,  false, 4, 0.86f, 0.88f, 0.90f, 0.033f, 0, true, 0.85f},
      {"Jimmy Jazz 888000000",   {8,8,8,0,0,0,0,0,0}, true,  true,  true,  true,  6, 0.22f, 0.64f, 0.58f, 0.020f, 0, false, 0.68f},
      {"Full Gospel 888888888",  {8,8,8,8,8,8,8,8,8}, false, true,  false, true,  6, 0.44f, 0.72f, 0.50f, 0.024f, 2, true,  0.78f},
      {"Green Onions",           {8,8,8,8,0,0,0,0,0}, true,  false, true,  false, 4, 0.34f, 0.66f, 0.55f, 0.020f, 0, false, 0.58f},
      {"Whiter Shade",           {6,8,8,6,0,0,0,0,0}, true,  true,  false, false, 5, 0.16f, 0.58f, 0.34f, 0.016f, 0, true,  0.72f},
      {"Highway Star Grit",      {8,8,8,8,0,0,0,0,0}, true,  false, true,  false, 6, 0.86f, 0.76f, 0.68f, 0.028f, 2, true,  0.75f},
      {"Whipping Post",          {8,8,8,6,4,2,0,0,0}, true,  true,  true,  true,  6, 0.64f, 0.70f, 0.56f, 0.024f, 2, true,  0.74f},
      {"Gimme Some Lovin'",      {8,8,8,8,8,8,0,0,0}, false, true,  false, true,  6, 0.70f, 0.74f, 0.52f, 0.024f, 2, true,  0.72f},
      {"Soul Sacrifice",         {8,8,8,0,0,0,0,0,8}, false, true,  false, true,  6, 0.56f, 0.72f, 0.58f, 0.022f, 2, true,  0.76f},
      {"Joey D Bebop",           {8,3,8,0,0,0,0,0,0}, true,  false, true,  true,  6, 0.28f, 0.68f, 0.68f, 0.020f, 0, false, 0.65f},
      {"Reggae Bubble",          {8,0,8,0,0,0,0,0,0}, true,  true,  true,  false, 0, 0.22f, 0.46f, 0.64f, 0.014f, 1, false, 0.45f},
      {"Midnight Ballad",        {8,4,8,0,0,0,0,0,0}, true,  true,  false, true,  6, 0.14f, 0.54f, 0.35f, 0.016f, 0, true,  0.78f},
      {"Cathedral Majesty",      {8,6,8,8,6,8,4,4,6}, false, true,  false, true,  5, 0.22f, 0.68f, 0.28f, 0.018f, 0, true,  0.82f},
      // Legacy aliases for backward compatibility with older saved states
      {"Rock Drive",             {8,8,8,8,0,0,0,0,0}, true,  false, true,  false, 6, 0.86f, 0.76f, 0.68f, 0.028f, 2, true,  0.75f},
      {"Squabble",               {6,8,8,4,6,4,2,4,2}, false, true,  false, true,  6, 0.38f, 0.68f, 0.52f, 0.018f, 0, false, 0.65f},
  };

  for (const auto &p : kPresets) {
    if (name == p.name) {
      for (int i = 0; i < 9; ++i)
        drawbar[(size_t)i].store((float)p.d[i] / 8.0f);
      percOn.store(p.percOn);
      percSoft.store(p.percSoft);
      percFast.store(p.percFast);
      percThird.store(p.percThird);
      scannerMode.store(p.scanner);
      drive.store(p.drive);
      tone.store(p.tone);
      clickDepth.store(p.click);
      leakage.store(p.leakage);
      leslieMode.store(p.leslieMode);
      leslieBypass.store(false);
      leslieDual.store(p.dual);
      micSpread.store(p.spread);
      break;
    }
  }
  presetName = (name == "Rock Drive") ? juce::String("Highway Star Grit") : name;
  presetDirty.store(false);
  paramsDirty.store(true);
  syncCatalogIndex();
}

inline juce::var B3OrganProcessor::getState() const {
  auto *o = new juce::DynamicObject();
  o->setProperty("version", 3);
  o->setProperty("enabled", enabled.load());
  o->setProperty("presetName", presetName);
  o->setProperty("volume", (double)volume.load());
  o->setProperty("swell", (double)swell.load());
  o->setProperty("drive", (double)drive.load());
  o->setProperty("tone", (double)tone.load());
  o->setProperty("click", (double)clickDepth.load());
  o->setProperty("leakage", (double)leakage.load());
  o->setProperty("scannerMode", scannerMode.load());
  o->setProperty("percOn", percOn.load());
  o->setProperty("percSoft", percSoft.load());
  o->setProperty("percFast", percFast.load());
  o->setProperty("percThird", percThird.load());
  o->setProperty("leslieMode", leslieMode.load());
  o->setProperty("leslieBypass", leslieBypass.load());
  o->setProperty("leslieDual", leslieDual.load());
  o->setProperty("leslieCab", leslieCab.load());
  o->setProperty("scannerDepth", (double)scannerDepth.load());
  o->setProperty("roomMix", (double)roomMix.load());
  o->setProperty("leslieWarmth", (double)leslieWarmth.load());
  o->setProperty("leslieChorus", (double)leslieChorus.load());
  o->setProperty("micSpread", (double)micSpread.load());
  o->setProperty("eqLowDb", (double)eqLowDb.load());
  o->setProperty("eqMidDb", (double)eqMidDb.load());
  o->setProperty("eqHighDb", (double)eqHighDb.load());
  o->setProperty("compAmount", (double)compAmount.load());
  o->setProperty("reverbMix", (double)reverbMix.load());
  o->setProperty("delayMix", (double)delayMix.load());
  o->setProperty("invertDrawbars", invertDrawbars.load());
  o->setProperty("octaveUp", octaveUp.load());
  o->setProperty("sustainControlsLeslie", sustainControlsLeslie.load());
  o->setProperty("ccProfile", ccProfile.load());
  if (customCcMapName.isNotEmpty())
    o->setProperty("customCcMapName", customCcMapName);
  // Snapshot ext-slot metadata under the lock, then call the (potentially slow)
  // plugin getStateInformation OUTSIDE extLock. Ext plugins are only swapped on
  // the message thread -- the same thread that saves state -- so the raw pointers
  // stay valid here, and the audio thread's runExtSlot trylock never fails during
  // a save (which would otherwise momentarily substitute the native Leslie).
  juce::AudioPluginInstance *extPlug[kNumExtSlots];
  juce::String extPath[kNumExtSlots], extName[kNumExtSlots];
  juce::String extPendPath[kNumExtSlots], extPendName[kNumExtSlots], extPendB64[kNumExtSlots];
  bool extByp[kNumExtSlots];
  {
    juce::SpinLock::ScopedLockType sl(extLock);
    for (int i = 0; i < kNumExtSlots; ++i) {
      extPlug[i] = extSlots[i].plugin.get();
      extPath[i] = extSlots[i].path;
      extName[i] = extSlots[i].name;
      extByp[i] = extSlots[i].bypassed.load();
      extPendPath[i] = extSlots[i].pendingPath;
      extPendName[i] = extSlots[i].pendingName;
      extPendB64[i] = extSlots[i].pendingStateB64;
    }
  }
  for (int i = 0; i < kNumExtSlots; ++i) {
    const juce::String pfx = (i == ExtSat) ? "extSat" : "extLeslie";
    o->setProperty(pfx + "Bypass", extByp[i]);
    if (extPlug[i] != nullptr) {
      o->setProperty(pfx + "Path", extPath[i]);
      o->setProperty(pfx + "Name", extName[i]);
      juce::MemoryBlock mb;
      extPlug[i]->getStateInformation(mb);
      if (mb.getSize() > 0)
        o->setProperty(pfx + "State", mb.toBase64Encoding());
    } else if (extPendPath[i].isNotEmpty()) {
      o->setProperty(pfx + "Path", extPendPath[i]);
      o->setProperty(pfx + "Name", extPendName[i]);
      if (extPendB64[i].isNotEmpty())
        o->setProperty(pfx + "State", extPendB64[i]);
    }
  }
  auto *db = new juce::DynamicObject();
  for (int i = 0; i < 9; ++i)
    db->setProperty("d" + juce::String(i), (double)drawbar[(size_t)i].load());
  o->setProperty("drawbars", juce::var(db));
  return juce::var(o);
}

inline void B3OrganProcessor::applyState(const juce::var &v,
                                         bool preserveEnabledIfMissing) {
  auto *obj = v.getDynamicObject();
  if (!obj)
    return;
  if (obj->hasProperty("enabled"))
    enabled.store((bool)obj->getProperty("enabled"));
  else if (!preserveEnabledIfMissing)
    enabled.store(false);
  if (obj->hasProperty("presetName"))
    presetName = obj->getProperty("presetName").toString();
  if (obj->hasProperty("volume"))  volume.store((float)(double)obj->getProperty("volume"));
  if (obj->hasProperty("swell"))   swell.store((float)(double)obj->getProperty("swell"));
  if (obj->hasProperty("drive"))   drive.store((float)(double)obj->getProperty("drive"));
  if (obj->hasProperty("tone"))    tone.store((float)(double)obj->getProperty("tone"));
  if (obj->hasProperty("click"))   clickDepth.store((float)(double)obj->getProperty("click"));
  if (obj->hasProperty("leakage")) leakage.store((float)(double)obj->getProperty("leakage"));
  if (obj->hasProperty("scannerMode"))  scannerMode.store((int)obj->getProperty("scannerMode"));
  if (obj->hasProperty("percOn"))       percOn.store((bool)obj->getProperty("percOn"));
  if (obj->hasProperty("percSoft"))     percSoft.store((bool)obj->getProperty("percSoft"));
  if (obj->hasProperty("percFast"))     percFast.store((bool)obj->getProperty("percFast"));
  if (obj->hasProperty("percThird"))    percThird.store((bool)obj->getProperty("percThird"));
  if (obj->hasProperty("leslieMode"))   leslieMode.store((int)obj->getProperty("leslieMode"));
  if (obj->hasProperty("leslieBypass")) leslieBypass.store((bool)obj->getProperty("leslieBypass"));
  if (obj->hasProperty("leslieDual"))   leslieDual.store((bool)obj->getProperty("leslieDual"));
  if (obj->hasProperty("leslieCab"))    leslieCab.store(juce::jlimit(0, 3, (int)obj->getProperty("leslieCab")));
  if (obj->hasProperty("scannerDepth")) scannerDepth.store(juce::jlimit(0.0f, 1.0f, (float)(double)obj->getProperty("scannerDepth")));
  if (obj->hasProperty("leslieWarmth")) leslieWarmth.store(juce::jlimit(0.0f, 1.0f, (float)(double)obj->getProperty("leslieWarmth")));
  if (obj->hasProperty("leslieChorus")) leslieChorus.store(juce::jlimit(0.0f, 1.0f, (float)(double)obj->getProperty("leslieChorus")));
  if (obj->hasProperty("roomMix"))      roomMix.store(juce::jlimit(0.0f, 1.0f, (float)(double)obj->getProperty("roomMix")));
  if (obj->hasProperty("micSpread"))    micSpread.store((float)(double)obj->getProperty("micSpread"));
  if (obj->hasProperty("eqLowDb"))      eqLowDb.store((float)(double)obj->getProperty("eqLowDb"));
  if (obj->hasProperty("eqMidDb"))      eqMidDb.store((float)(double)obj->getProperty("eqMidDb"));
  if (obj->hasProperty("eqHighDb"))     eqHighDb.store((float)(double)obj->getProperty("eqHighDb"));
  if (obj->hasProperty("compAmount"))   compAmount.store((float)(double)obj->getProperty("compAmount"));
  if (obj->hasProperty("reverbMix"))    reverbMix.store((float)(double)obj->getProperty("reverbMix"));
  if (obj->hasProperty("delayMix"))     delayMix.store(juce::jlimit(0.0f, 1.0f, (float)(double)obj->getProperty("delayMix")));
  if (obj->hasProperty("invertDrawbars")) invertDrawbars.store((bool)obj->getProperty("invertDrawbars"));
  if (obj->hasProperty("octaveUp"))       octaveUp.store((bool)obj->getProperty("octaveUp"));
  if (obj->hasProperty("sustainControlsLeslie"))
    sustainControlsLeslie.store((bool)obj->getProperty("sustainControlsLeslie"));
  if (obj->hasProperty("ccProfile"))
    ccProfile.store(juce::jlimit(0, (int)CcCustom, (int)obj->getProperty("ccProfile")));
  if (obj->hasProperty("customCcMapName"))
    customCcMapName = obj->getProperty("customCcMapName").toString();
  // Resolve CcCustom unconditionally: a state with ccProfile=CcCustom but a
  // missing/empty/unresolvable map name must fall back to CcAuto, otherwise the
  // audio thread would match incoming CCs against unset drawbar-CC slots.
  if (ccProfile.load() == CcCustom) {
    bool resolved = false;
    if (customCcMapName.isNotEmpty()) {
      refreshCustomCcMaps();
      resolved = applyCustomCcMapByName(customCcMapName, false);
    }
    if (!resolved)
      ccProfile.store(CcAuto);
  }
  {
    juce::SpinLock::ScopedLockType sl(extLock);
    for (int i = 0; i < kNumExtSlots; ++i) {
      const juce::String pfx = (i == ExtSat) ? "extSat" : "extLeslie";
      if (obj->hasProperty(pfx + "Bypass"))
        extSlots[i].bypassed.store((bool)obj->getProperty(pfx + "Bypass"));
      extSlots[i].pendingPath = obj->getProperty(pfx + "Path").toString();
      extSlots[i].pendingName = obj->getProperty(pfx + "Name").toString();
      extSlots[i].pendingStateB64 = obj->getProperty(pfx + "State").toString();
    }
  }
  if (auto *dbObj = obj->getProperty("drawbars").getDynamicObject()) {
    for (int i = 0; i < 9; ++i) {
      const juce::String key = "d" + juce::String(i);
      if (dbObj->hasProperty(key))
        drawbar[(size_t)i].store((float)(double)dbObj->getProperty(key));
    }
  }
  presetDirty.store(false);
  paramsDirty.store(true);
  syncCatalogIndex();
}

inline void B3OrganProcessor::processBlock(juce::AudioBuffer<float> &buffer,
                                           const juce::MidiBuffer &midiMessages) {
  juce::ScopedNoDenormals noDenormals;
  const int numSamples = buffer.getNumSamples();
  if (numSamples <= 0)
    return;

  if (panicPending.exchange(false, std::memory_order_acq_rel))
    panicNow();
  if (allNotesOffPending.exchange(false, std::memory_order_acq_rel))
    panicNow();

  // Push UI/state parameter changes into DSP modules before rendering
  if (paramsDirty.exchange(false, std::memory_order_acq_rel))
    applyAllParams();

  // Swell is updated per-block (smoothed inside preamp)
  preamp.setSwell(swell.load(std::memory_order_relaxed));

  // Merge UI MIDI with host MIDI
  mergedMidi.clear();
  {
    juce::SpinLock::ScopedTryLockType sl(uiMidiLock);
    if (sl.isLocked() && !uiMidiQueue.isEmpty()) {
      mergedMidi.addEvents(uiMidiQueue, 0, -1, 0);
      uiMidiQueue.clear();
    }
  }
  if (!midiMessages.isEmpty())
    mergedMidi.addEvents(midiMessages, 0, -1, 0);

  // Consume explicit UI Leslie mode selection (Slow=0, Brake=1, Fast=2)
  const int plm = uiLeslieModePending.exchange(-1, std::memory_order_acq_rel);
  if (plm >= 0) {
    leslieMode.store(plm, std::memory_order_relaxed);
    leslie.setMode(plm);
  }

  // Consume UI mod wheel pending
  const int pmw = uiModWheelPending.exchange(-1, std::memory_order_acq_rel);
  if (pmw >= 0 && plm < 0) {
    const int mode = pmw >= 64 ? B3::Leslie122::Fast : B3::Leslie122::Slow;
    leslieMode.store(mode, std::memory_order_relaxed);
    leslie.setMode(mode);
  }
  uiPitchBendPending.exchange(-1, std::memory_order_acq_rel);

  // Automatic idle-silence gate: wake instantaneously on any MIDI/voice/pedal
  // activity; sleep after 2.5s of <-100dB silence so idle B3 strips use ~0% CPU.
  if (!mergedMidi.isEmpty() || plm >= 0 || pmw >= 0 ||
      wheels.activeVoices() > 0 || sustHeld.any() || pedalDown) {
    idleSilenceSamples = 0;
  } else if (idleSilenceSamples >= (int)(sr * 2.5)) {
    peakL.store(peakL.load(std::memory_order_relaxed) * 0.86f,
                std::memory_order_relaxed);
    peakR.store(peakR.load(std::memory_order_relaxed) * 0.86f,
                std::memory_order_relaxed);
    return;
  }

  // Prepare forwarded MIDI (Mod Wheel CC1 + all CCs/PitchBend) for external Leslie VST3
  extMidiScratch.clear();
  if (pmw >= 0)
    extMidiScratch.addEvent(juce::MidiMessage::controllerEvent(1, 1, pmw), 0);
  for (const auto meta : mergedMidi) {
    const auto msg = meta.getMessage();
    if (msg.isController()) {
      const int cc = msg.getControllerNumber();
      const int val = msg.getControllerValue();
      if (cc == 1 || cc == FanfareConstants::kCk88ModWheelCC) {
        extMidiScratch.addEvent(
            juce::MidiMessage::controllerEvent(1, 1, val),
            meta.samplePosition);
        if (msg.getChannel() != 1 && cc == 1)
          extMidiScratch.addEvent(msg, meta.samplePosition);
      } else if (cc == FanfareConstants::kCk88SustainCC &&
                 sustainControlsLeslie.load(std::memory_order_relaxed)) {
        extMidiScratch.addEvent(
            juce::MidiMessage::controllerEvent(1, 1, val),
            meta.samplePosition);
      } else {
        extMidiScratch.addEvent(msg, meta.samplePosition);
      }
    } else if (msg.isPitchWheel() || msg.isChannelPressure() || msg.isAftertouch()) {
      extMidiScratch.addEvent(msg, meta.samplePosition);
    }
  }

  // Ensure scratch buffers
  const int need = numSamples;
  if ((int)scratch.size() < need) {
    scratch.assign((size_t)need, 0.0f);
    scratchR.assign((size_t)need, 0.0f);
    scratchPerc.assign((size_t)need, 0.0f);
  }
  std::fill(scratch.begin(), scratch.begin() + need, 0.0f);
  std::fill(scratchR.begin(), scratchR.begin() + need, 0.0f);
  std::fill(scratchPerc.begin(), scratchPerc.begin() + need, 0.0f);

  // Sample-accurate sub-block rendering across MIDI event offsets:
  // Main drawbars + key-click -> scratch; Harmonic Percussion -> scratchPerc
  int curSample = 0;
  for (const auto meta : mergedMidi) {
    const int eventSample = juce::jlimit(0, need - 1, meta.samplePosition);
    if (eventSample > curSample) {
      wheels.renderSplit(scratch.data() + curSample,
                         scratchPerc.data() + curSample,
                         eventSample - curSample);
      curSample = eventSample;
    }
    handleMidiMessage(meta.getMessage());
  }
  if (curSample < need) {
    wheels.renderSplit(scratch.data() + curSample,
                       scratchPerc.data() + curSample, need - curSample);
  }

  // Authentic B3 routing: Drawbars pass through LC Scanner Vibrato/Chorus;
  // Harmonic Percussion sums into AO-28 Tube Preamp POST-scanner so 2nd/3rd
  // percussion stays punchy, woody, and un-smeared even on C3!
  scanner.process(scratch.data(), need);
  for (int s = 0; s < need; ++s)
    scratch[(size_t)s] += scratchPerc[(size_t)s];

  preamp.process(scratch.data(), need);

  // Split mono preamp output into stereo
  std::copy(scratch.begin(), scratch.begin() + need, scratchR.begin());

  // Helper to run an external stereo VST3 insert safely without blocking RT audio
  auto runExtSlot = [&](int extSlotIdx, juce::MidiBuffer &midi) -> bool {
    juce::SpinLock::ScopedTryLockType sl(extLock);
    if (!sl.isLocked())
      return false;
    auto *plug = extSlots[extSlotIdx].plugin.get();
    if (plug == nullptr || extSlots[extSlotIdx].bypassed.load(std::memory_order_relaxed))
      return false;

    const int nCh = juce::jmax(2, juce::jmax(plug->getTotalNumInputChannels(),
                                             plug->getTotalNumOutputChannels()));
    // Normally pre-sized by prepare()/swapExtPlugin(); this fallback should be
    // unreachable for device blocks within the prepared capacity.
    if (extStereoBuf.getNumChannels() < nCh || extStereoBuf.getNumSamples() < need)
      extStereoBuf.setSize(nCh, need, false, false, true);
    extStereoBuf.clear();
    juce::FloatVectorOperations::copy(extStereoBuf.getWritePointer(0), scratch.data(), need);
    juce::FloatVectorOperations::copy(extStereoBuf.getWritePointer(1), scratchR.data(), need);

    juce::AudioBuffer<float> view(extStereoBuf.getArrayOfWritePointers(), nCh, need);
    const bool ok = FanfareLog::safeExecutePluginCall(
        [&]() { plug->processBlock(view, midi); },
        extSlotIdx == ExtSat ? "B3 ExtSat" : "B3 ExtLeslie");
    if (ok) {
      juce::FloatVectorOperations::copy(scratch.data(), view.getReadPointer(0), need);
      juce::FloatVectorOperations::copy(scratchR.data(), view.getReadPointer(1), need);
      return true;
    }
    return false;
  };

  // Optional External Saturation/Compression VST3 (pre-Leslie)
  {
    juce::MidiBuffer emptyMidi;
    runExtSlot(ExtSat, emptyMidi);
  }

  // External Leslie VST3 replaces native Leslie 122 when active; otherwise native Leslie runs
  if (!runExtSlot(ExtLeslie, extMidiScratch))
    leslie.process(scratch.data(), scratchR.data(), need);

  // Post-Cabinet Studio FX: Hidden 10-Band EQ -> 3-Band EQ -> Optical Comp -> Ping-Pong Delay -> Reverb
  masterFx.process(scratch.data(), scratchR.data(), need);

  // Behind-the-scenes Built-In Studio Wood Room IR (0.55s @ 18% mix)
  if (woodRoomIr.hasIrLoaded() && woodIrScratch.getNumSamples() >= need) {
    juce::FloatVectorOperations::copy(woodIrScratch.getWritePointer(0), scratch.data(), need);
    juce::FloatVectorOperations::copy(woodIrScratch.getWritePointer(1), scratchR.data(), need);
    juce::AudioBuffer<float> irView(woodIrScratch.getArrayOfWritePointers(), 2, need);
    woodRoomIr.processBlock(irView, true, 0.18f);
    juce::FloatVectorOperations::copy(scratch.data(), irView.getReadPointer(0), need);
    juce::FloatVectorOperations::copy(scratchR.data(), irView.getReadPointer(1), need);
  }

  // Output volume (smoothed ~10 ms) + soft safety ceiling + peak metering
  const float k = 1.0f - std::exp(-1.0f / (0.010f * (float)sr));
  const float volTarget = volume.load(std::memory_order_relaxed);
  float blockPeakL = 0.0f, blockPeakR = 0.0f;

  for (int s = 0; s < need; ++s) {
    volSmoothed += k * (volTarget - volSmoothed);
    float l = scratch[(size_t)s] * volSmoothed;
    float r = scratchR[(size_t)s] * volSmoothed;
    if (!std::isfinite(l)) l = 0.0f;
    if (!std::isfinite(r)) r = 0.0f;
    l = softCeiling(l);
    r = softCeiling(r);
    blockPeakL = juce::jmax(blockPeakL, std::abs(l));
    blockPeakR = juce::jmax(blockPeakR, std::abs(r));
    if (buffer.getNumChannels() > 0) buffer.addSample(0, s, l);
    if (buffer.getNumChannels() > 1) buffer.addSample(1, s, r);
  }

  peakL.store(juce::jmax(blockPeakL, peakL.load(std::memory_order_relaxed) * 0.86f),
              std::memory_order_relaxed);
  peakR.store(juce::jmax(blockPeakR, peakR.load(std::memory_order_relaxed) * 0.86f),
              std::memory_order_relaxed);

  if (wheels.activeVoices() == 0 && !sustHeld.any() &&
      blockPeakL < 1.0e-5f && blockPeakR < 1.0e-5f) {
    idleSilenceSamples += numSamples;
  } else {
    idleSilenceSamples = 0;
  }
}
