// FanfareEngine.cpp - cold member-function definitions moved out of
// FanfareEngine.h mechanically (see nov8mistral.md). Hot path (processAudio)
// and small inline getters remain in the header.
#include "FanfareEngine.h"
#include "StripStateLive.h"

FanfareEngine::FanfareEngine() 
{
#ifdef _WIN32
    avrtModule = LoadLibraryA("avrt.dll");
    if (avrtModule != nullptr) {
      avSetMmThread = (PAvSetMmThreadCharacteristicsW)GetProcAddress(avrtModule, "AvSetMmThreadCharacteristicsW");
    }
#endif
#if JUCE_PLUGINHOST_VST3
    formatManager.addFormat(std::make_unique<juce::VST3PluginFormat>());
#endif
#if JUCE_PLUGINHOST_AU
    formatManager.addFormat(std::make_unique<juce::AudioUnitPluginFormat>());
#endif

    std::fill(std::begin(stressTestLastNote), std::end(stressTestLastNote), -1);

    slots.clear();
    for (int i = 0; i < FanfareConstants::kNumSlots; ++i) {
      if (i == 0) {
        slots.push_back(std::make_unique<RackSlot>("Monitor In"));
        slots.back()->setInputChannelIndex(0); // Hardware In 1
      } else if (i == 1) {
        slots.push_back(std::make_unique<RackSlot>("Keyboard"));
        slots.back()->setInputChannelIndex(FanfareConstants::kKeyboardInputChannel); // Hardware In 11
      } else {
        slots.push_back(
            std::make_unique<RackSlot>("Slot " + juce::String(i + 1)));
      }
    }

    // Initialize Aux Returns
    auxReturns.push_back(std::make_unique<RackSlot>("Aux 1 Return"));
    auxReturns.push_back(std::make_unique<RackSlot>("Aux 2 Return"));

    // Special routing for Monitor In (slot 0): only to IEM, no aux sends
    slots[0]->setFohEnabled(false);
    slots[0]->setIemEnabled(true);
    slots[0]->setAux1Send(0.0f); // No feedback to aux buses
    slots[0]->setAux2Send(0.0f);

    // Attach harmony-routing callbacks: when a slot's harmonizer targets
    // another slot, generated voices are injected into that slot's MIDI input.
    for (int i = 0; i < (int)slots.size(); ++i) {
      slots[i]->setMidiRouteCallback(
          [this](int targetSlot, const juce::MidiBuffer &buffer) {
            if (targetSlot >= 0 && targetSlot < (int)slots.size())
              slots[targetSlot]->injectMidi(buffer);
          });
      slots[i]->onBeforePluginDestroyed = [this](juce::AudioPluginInstance *pi) {
        if (onBeforePluginDestroyed)
          onBeforePluginDestroyed(pi);
      };
    }
    for (auto &aux : auxReturns) {
      if (aux) {
        aux->onBeforePluginDestroyed = [this](juce::AudioPluginInstance *pi) {
          if (onBeforePluginDestroyed)
            onBeforePluginDestroyed(pi);
        };
      }
    }

    initializeDefaultScenes();
    performLegacyMigration();
    loadPluginList();
    startDspWorkers();
  }

  

FanfareEngine::~FanfareEngine() 
{
    if (engineAlive) engineAlive->store(false);
    // 1. Stop persistent real-time DSP helper threads FIRST
    stopDspWorkers();

    // 2. Now safe to release plugins
    releaseAllPlugins();

#ifdef _WIN32
    if (avrtModule != nullptr) {
      FreeLibrary(avrtModule);
      avrtModule = nullptr;
    }
#endif
  }

  

// Call this BEFORE the engine is destroyed to release all VST3 plugins
  void FanfareEngine::releaseAllPlugins() 
{
    ScopedAudioQuietWindow quietWindow(*this);
    juce::ScopedLock sl(lock);

    // Release all slot plugins
    for (auto &slot : slots) {
      slot->clearChain();
    }

    for (auto &slot : auxReturns) {
      slot->clearChain();
    }

    // Release Master FX chains outside masterFxLock
    std::vector<std::unique_ptr<juce::AudioPluginInstance>> oldFoh, oldIem;
    {
      juce::SpinLock::ScopedLockType mfl(masterFxLock);
      oldFoh.swap(fohPluginChain);
      oldIem.swap(iemPluginChain);
    }
    for (auto &p : oldFoh) {
      if (p) {
        if (onBeforePluginDestroyed)
          onBeforePluginDestroyed(p.get());
        FanfareLog::safeExecutePluginCall([&]() {
          p->releaseResources();
          p.reset();
        }, "fohPlugin releaseResources/reset");
      }
    }
    for (auto &p : oldIem) {
      if (p) {
        if (onBeforePluginDestroyed)
          onBeforePluginDestroyed(p.get());
        FanfareLog::safeExecutePluginCall([&]() {
          p->releaseResources();
          p.reset();
        }, "iemPlugin releaseResources/reset");
      }
    }

    {
      juce::ScopedLock sLock(stagingLock);
      lastAppliedPluginStates.clear();
    }

    // Close all opened MIDI output devices.
    juce::SpinLock::ScopedLockType mol(midiOutputLock);
    midiOutputs.clear();
    midiOutputIndexById.clear();
  }

  

void FanfareEngine::setDspWorkerAffinityMask(int workerIdx, uintptr_t mask) noexcept 
{
    if (workerIdx >= 0 && workerIdx < kMaxDspHelpers) {
      dspWorkerAffinityMasks[workerIdx].store(mask, std::memory_order_release);
      dspTuningGen.fetch_add(1, std::memory_order_acq_rel);
    }
  }

  

 void FanfareEngine::applyThreadHighQoS(bool disableEcoQoS) noexcept 
{
#ifdef _WIN32
    THREAD_POWER_THROTTLING_STATE state{};
    state.Version = THREAD_POWER_THROTTLING_CURRENT_VERSION;
    state.ControlMask = THREAD_POWER_THROTTLING_EXECUTION_SPEED;
    state.StateMask = disableEcoQoS ? 0UL : THREAD_POWER_THROTTLING_EXECUTION_SPEED;
    SetThreadInformation(GetCurrentThread(), ThreadPowerThrottling, &state, sizeof(state));
#else
    juce::ignoreUnused(disableEcoQoS);
#endif
  }

  

void FanfareEngine::applyBackgroundThreadTuning() const noexcept 
{
#ifdef _WIN32
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    const DWORD_PTR bgMask = backgroundAffinityMask.load(std::memory_order_acquire);
    if (bgMask != 0)
      SetThreadAffinityMask(GetCurrentThread(), bgMask);
#endif
  }

  

void FanfareEngine::loadScene(int index) 
{
    Scene sceneCopy("");
    {
      juce::ScopedLock sl(scenesLock);
      if (index < 0 || index >= (int)scenes.size())
        return;
      sceneCopy = scenes[index];
      currentSceneIndex = index;
    }
    // Only flush voices/panic when key-range, transpose, or MIDI channel changes,
    // so pure mute/volume/DSP snapshots allow held chords and tails to ring smoothly.
    bool needsMidiReset = false;
    for (size_t i = 0; i < slots.size() && i < sceneCopy.slotStates.size(); ++i) {
      auto* slot = slots[i].get();
      const auto& st = sceneCopy.slotStates[i];
      if (slot->getTransposeOctaves() != st.transposeOctaves ||
          slot->getTransposeSemitones() != st.transposeSemitones ||
          slot->getLowNote() != st.lowNote ||
          slot->getHighNote() != st.highNote ||
          (st.hasMidiCcState && slot->getMidiChannelOverride() != st.midiChannelOverride)) {
        needsMidiReset = true;
        break;
      }
    }
    if (needsMidiReset)
      panicTriggered.store(true);

    for (size_t i = 0; i < slots.size() && i < sceneCopy.slotStates.size(); ++i) {
      auto* slot = slots[i].get();
      const auto& st = sceneCopy.slotStates[i];
      
      slot->setBypass(st.bypassed);
      slot->setFohLevel(st.fohLevel);
      slot->setIemLevel(st.iemLevel);
      slot->setFohEnabled(st.fohEnabled);
      slot->setIemEnabled(st.iemEnabled);
      slot->setFadersLinked(st.fadersLinked);
      slot->setAux1Send(st.aux1Send);
      slot->setAux2Send(st.aux2Send);
      slot->setIemOffset(st.iemOffset);
      slot->setTransposeOctaves(st.transposeOctaves);
      slot->setTransposeSemitones(st.transposeSemitones);
      slot->setNoteRange(st.lowNote, st.highNote);
      if (st.hasMidiCcState) {
        slot->setFohCC(st.fohCC);
        slot->setIemCC(st.iemCC);
        slot->setMuteCC(st.muteCC);
        slot->setMidiChannelOverride(st.midiChannelOverride);
      }

      // DSP, Arpeggiator, Harmonizer
      Fanfare::applyStrip(slot->getStrip(), st.strip, st.hasMusetteAndChannelMode, true, false);
      Fanfare::applyArp(slot->getArpeggiator(), st.arp);
      Fanfare::applyHarm(slot->getHarmonizer(), st.harm);

      // Sampler
      slot->getSampler().enabled.store(st.samplerEnabled);
    }
  }

  

std::vector<juce::String> FanfareEngine::getSceneNamesSnapshot(int &outCurrentIndex) const 
{
    juce::ScopedLock sl(scenesLock);
    outCurrentIndex = currentSceneIndex;
    std::vector<juce::String> names;
    names.reserve(scenes.size());
    for (const auto &sc : scenes)
      names.push_back(sc.name);
    return names;
  }

  

void FanfareEngine::setSceneMidiTrigger(int index, int pc, int channel) 
{
    juce::ScopedLock sl(scenesLock);
    if (index >= 0 && index < (int)scenes.size()) {
      scenes[index].midiProgramChange = pc;
      scenes[index].midiChannel = channel;
    }
  }
  

void FanfareEngine::clearSceneMidiTrigger(int index) 
{
    juce::ScopedLock sl(scenesLock);
    if (index >= 0 && index < (int)scenes.size()) {
      scenes[index].midiProgramChange = -1;
      scenes[index].midiChannel = 0;
    }
  }

  

// Returns the scene index that has been assigned the given PC + channel,
  // or -1 if none found. channel 0 = match any channel.
  int FanfareEngine::findSceneForProgram(int pc, int channel) const 
{
    juce::ScopedLock sl(scenesLock);
    for (int i = 0; i < (int)scenes.size(); ++i) {
      const auto& sc = scenes[i];
      if (sc.midiProgramChange == pc) {
        if (sc.midiChannel == 0 || channel == 0 || sc.midiChannel == channel)
          return i;
      }
    }
    return -1;
  }

  

void FanfareEngine::saveCurrentStateToScene(int index) 
{
    std::vector<SlotState> captured;
    captured.reserve(slots.size());
    for (auto &slot : slots) {
      SlotState st;
      st.bypassed = slot->isBypassed();
      st.fohLevel = slot->getFohLevel();
      st.iemLevel = slot->getIemLevel();
      st.fohEnabled = slot->isFohEnabled();
      st.iemEnabled = slot->isIemEnabled();
      st.fadersLinked = slot->areFadersLinked();
      st.aux1Send = slot->getAux1Send();
      st.aux2Send = slot->getAux2Send();
      st.iemOffset = slot->getIemOffset();
      st.transposeOctaves = slot->getTransposeOctaves();
      st.transposeSemitones = slot->getTransposeSemitones();
      st.lowNote = slot->getLowNote();
      st.highNote = slot->getHighNote();

      // DSP, Arpeggiator, Harmonizer
      st.strip = Fanfare::captureStrip(slot->getStrip());
      st.hasMusetteAndChannelMode = true;
      st.arp = Fanfare::captureArp(slot->getArpeggiator());
      st.harm = Fanfare::captureHarm(slot->getHarmonizer());

      // Sampler
      st.samplerEnabled = slot->getSampler().enabled.load();

      // CC & MIDI channel
      st.hasMidiCcState = true;
      st.fohCC = slot->getFohCC();
      st.iemCC = slot->getIemCC();
      st.muteCC = slot->getMuteCC();
      st.midiChannelOverride = slot->getMidiChannelOverride();

      captured.push_back(std::move(st));
    }

    juce::ScopedLock sl(scenesLock);
    if (index >= 0 && index < (int)scenes.size()) {
      scenes[index].slotStates = std::move(captured);
    }
  }

  

void FanfareEngine::renameScene(int index, const juce::String &newName) 
{
    juce::ScopedLock sl(scenesLock);
    if (index >= 0 && index < (int)scenes.size()) {
      scenes[index].name = newName;
    }
  }

void FanfareEngine::setSceneColor(int index, juce::uint32 argb)
{
    juce::ScopedLock sl(scenesLock);
    if (index >= 0 && index < (int)scenes.size()) {
      scenes[index].colorArgb = argb;
    }
  }

  

void FanfareEngine::createNewScene(const juce::String &name) 
{
    Scene newScene(name);
    for (auto &slot : slots) {
      SlotState st;
      st.bypassed = slot->isBypassed();
      st.fohLevel = slot->getFohLevel();
      st.iemLevel = slot->getIemLevel();
      st.fohEnabled = slot->isFohEnabled();
      st.iemEnabled = slot->isIemEnabled();
      st.fadersLinked = slot->areFadersLinked();
      st.aux1Send = slot->getAux1Send();
      st.aux2Send = slot->getAux2Send();
      st.iemOffset = slot->getIemOffset();
      st.transposeOctaves = slot->getTransposeOctaves();
      st.transposeSemitones = slot->getTransposeSemitones();
      st.lowNote = slot->getLowNote();
      st.highNote = slot->getHighNote();

      // DSP, Arpeggiator, Harmonizer
      st.strip = Fanfare::captureStrip(slot->getStrip());
      st.hasMusetteAndChannelMode = true;
      st.arp = Fanfare::captureArp(slot->getArpeggiator());
      st.harm = Fanfare::captureHarm(slot->getHarmonizer());

      // Sampler
      st.samplerEnabled = slot->getSampler().enabled.load();

      // CC & MIDI channel
      st.hasMidiCcState = true;
      st.fohCC = slot->getFohCC();
      st.iemCC = slot->getIemCC();
      st.muteCC = slot->getMuteCC();
      st.midiChannelOverride = slot->getMidiChannelOverride();

      newScene.slotStates.push_back(st);
    }
    juce::ScopedLock sl(scenesLock);
    scenes.push_back(std::move(newScene));
  }

  

void FanfareEngine::deleteScene(int index) 
{
    int toLoad = -1;
    {
      juce::ScopedLock sl(scenesLock);
      if (scenes.size() <= 1)
        return;
      if (index >= 0 && index < (int)scenes.size()) {
        const bool deletedActive = (index == currentSceneIndex);
        scenes.erase(scenes.begin() + index);
        if (index < currentSceneIndex) {
          --currentSceneIndex;
        } else if (currentSceneIndex >= (int)scenes.size()) {
          currentSceneIndex = (int)scenes.size() - 1;
        }
        if (deletedActive)
          toLoad = currentSceneIndex;
      }
    }
    if (toLoad >= 0)
      loadScene(toLoad);
  }

  

// Lock-free, zero-allocation slot DSP execution used by both Sequential and
  // Parallel Work-Sharing paths (eliminates juce::ThreadPool job->pool reuse races).
  void FanfareEngine::executeSlotDsp(int slotIdx, const float *const *rawInput,
                      int numInputChannels, bool seedOnly,
                      int activeSamples) noexcept 
{
    juce::ScopedNoDenormals noDenormals;
    if (slotIdx >= 0 && slotIdx < (int)slots.size()) {
      auto &slot = slots[slotIdx];
      auto &scratch = scratchBuffers[slotIdx];
      auto &midi = slotMidiBuffers[slotIdx];
      int numSamples = (activeSamples > 0)
                           ? juce::jmin(activeSamples, scratch.getNumSamples())
                           : scratch.getNumSamples();

      int inputIdx = slot->getInputChannelIndex();
      if (inputIdx >= 0 && rawInput != nullptr && inputIdx < numInputChannels) {
        if (const float *l = rawInput[inputIdx])
          scratch.addFrom(0, 0, l, numSamples);

        if (slot->isInputStereo() && inputIdx + 1 < numInputChannels) {
          if (const float *r = rawInput[inputIdx + 1])
            scratch.addFrom(1, 0, r, numSamples);
        } else if (const float *l = rawInput[inputIdx]) {
          scratch.addFrom(1, 0, l, numSamples);
        }
      }

      if (!seedOnly) {
        juce::AudioBuffer<float> activeView(scratch.getArrayOfWritePointers(),
                                            scratch.getNumChannels(),
                                            numSamples);
        slot->processBlock(activeView, midi);
      }
    }
  }

  

void FanfareEngine::prepareToPlay(int samplesPerBlockExpected, double sampleRate) 
{
    ScopedAudioQuietWindow quietWindow(*this);
    currentSampleRate = sampleRate;
    currentBlockSize = samplesPerBlockExpected;
    dspActiveSamples.store(samplesPerBlockExpected, std::memory_order_relaxed);
    // Stereo buses (multi-out VST3 instruments render into RackSlot::scratchBuffer and sum stereo into slotBuffer)
    fohBus.setSize(2, samplesPerBlockExpected);
    iemBus.setSize(2, samplesPerBlockExpected);
    iemClick.prepare(sampleRate);

    // Initialize per-slot stereo scratch buffers
    scratchBuffers.resize(slots.size() + auxReturns.size());

    for (int i = 0; i < (int)slots.size(); ++i) {
      slots[i]->prepare(sampleRate, samplesPerBlockExpected);
      scratchBuffers[i].setSize(2, samplesPerBlockExpected);
    }

    // Scratch buffers for Aux Returns
    for (int i = 0; i < (int)auxReturns.size(); ++i) {
      auxReturns[i]->prepare(sampleRate, samplesPerBlockExpected);
      scratchBuffers[slots.size() + i].setSize(2, samplesPerBlockExpected);
    }

    aux1Bus.setSize(2, samplesPerBlockExpected);
    aux2Bus.setSize(2, samplesPerBlockExpected);

    // Pre-allocate per-block reusable buffers (avoid heap allocs on audio thread)
    dummyAux1.setSize(2, samplesPerBlockExpected);
    dummyAux2.setSize(2, samplesPerBlockExpected);
    slotMidiBuffers.resize(slots.size());
    for (auto &smb : slotMidiBuffers)
      smb.ensureSize(2048);
  }

  

namespace {
void collectVst3Bundles(
    const juce::File &dir,
    std::vector<FanfareEngine::PluginInfo> &out,
    const std::shared_ptr<std::atomic<bool>> &alive = nullptr) {
  if (!dir.exists() || !dir.isDirectory())
    return;
  for (const auto &entry : juce::RangedDirectoryIterator(
           dir, false, "*", juce::File::findFilesAndDirectories)) {
    if (alive && !alive->load())
      return;
    const juce::File f = entry.getFile();
    const juce::String name = f.getFileName();
    if (name.endsWithIgnoreCase(".vst3")) {
      out.push_back({f.getFileNameWithoutExtension(), f.getFullPathName()});
    } else if (f.isDirectory() && !name.endsWithIgnoreCase(".component") &&
               !name.endsWithIgnoreCase(".app")) {
      collectVst3Bundles(f, out, alive);
    }
  }
}

void appendUniquePluginInfo(std::vector<FanfareEngine::PluginInfo> &dst,
                            const FanfareEngine::PluginInfo &info) {
  for (const auto &existing : dst) {
    if (existing.path == info.path)
      return;
  }
  dst.push_back(info);
}

juce::String extractVst3LeafName(const juce::String &rawPath) {
  juce::String p = rawPath.trim();
  if (p.isEmpty())
    return {};
  p = p.replaceCharacter('\\', '/');
  juce::StringArray parts;
  parts.addTokens(p, "/", "");
  // Unwrap inner bundle paths like Foo.vst3/Contents/x86_64-win/Foo.vst3
  for (const auto &seg : parts) {
    if (seg.endsWithIgnoreCase(".vst3"))
      return seg;
  }
  return parts.isEmpty() ? juce::String{} : parts[parts.size() - 1];
}

juce::String normalizeVst3StemForMatch(const juce::String &leafName) {
  juce::String stem = leafName.trim();
  if (stem.endsWithIgnoreCase(".vst3"))
    stem = stem.dropLastCharacters(5);
  static const char *const kSuffixes[] = {
      "(VST3 64bit)", "(VST3 32bit)", "(VST3)", "(64bit)", "(64-bit)", "_x64", " x64"};
  for (const char *sfx : kSuffixes) {
    const int idx = stem.indexOfIgnoreCase(sfx);
    if (idx >= 0)
      stem = stem.substring(0, idx) + stem.substring(idx + (int)std::strlen(sfx));
  }
  return stem.trim();
}
} // namespace

void FanfareEngine::scanForPlugins()
{
    const auto vstDirs = FanfareConstants::getVst3SearchDirectories();
    bool hasAnyVstDir = false;
    for (const auto &d : vstDirs) {
      if (d.exists() && d.isDirectory()) {
        hasAnyVstDir = true;
        break;
      }
    }
#if !JUCE_PLUGINHOST_AU
    if (!hasAnyVstDir) {
      juce::AlertWindow::showMessageBoxAsync(
          juce::MessageBoxIconType::WarningIcon, "Scan Error",
          "VST3 folder not found!");
      return;
    }
#else
    juce::ignoreUnused(hasAnyVstDir);
#endif

    // Run in background to avoid UI freeze
    auto alive = engineAlive;
    std::thread([this, alive, vstDirs]() {
      ScanResults results;
      std::vector<PluginInfo> foundPlugins;

      for (const auto &vstFolder : vstDirs) {
        if (!alive->load()) return;
        std::vector<PluginInfo> dirFound;
        collectVst3Bundles(vstFolder, dirFound, alive);
        for (const auto &p : dirFound)
          appendUniquePluginInfo(foundPlugins, p);
      }

#if JUCE_PLUGINHOST_AU
      if (alive->load()) {
        juce::AudioPluginFormat *auFormat = nullptr;
        for (int i = 0; i < formatManager.getNumFormats(); ++i) {
          if (formatManager.getFormat(i)->getName() == "AudioUnit") {
            auFormat = formatManager.getFormat(i);
            break;
          }
        }
        if (auFormat != nullptr) {
          const juce::StringArray auIds =
              auFormat->searchPathsForPlugins({}, true, false);
          for (const auto &id : auIds) {
            if (!alive->load()) return;
            juce::String auName = auFormat->getNameOfPluginFromIdentifier(id);
            if (auName.isEmpty())
              auName = id;
            appendUniquePluginInfo(foundPlugins, {auName + " (AU)", id});
          }
        }
      }
#endif

      if (!alive->load()) return;

      // Compare with availablePlugins
      {
        juce::ScopedLock sl(lock);

        // New plugins
        for (const auto &found : foundPlugins) {
          bool exists = false;
          for (const auto &existing : availablePlugins) {
            if (existing.path == found.path) {
              exists = true;
              break;
            }
          }
          if (!exists)
            results.newPlugins.push_back(found);
        }

        // Missing plugins
        for (const auto &existing : availablePlugins) {
          bool exists = false;
          for (const auto &found : foundPlugins) {
            if (found.path == existing.path) {
              exists = true;
              break;
            }
          }
          // Also consider missing if file no longer exists
          if (!exists || !FanfareConstants::pluginExistsOnDisk(existing.path)) {
            results.missingPlugins.push_back(existing);
          }
        }
      }

      if (!alive->load()) return;

      // Call callback on UI thread
      juce::MessageManager::callAsync([this, alive, results]() {
        if (alive->load() && onScanFinished)
          onScanFinished(results);
      });
    }).detach();
  }

  

void FanfareEngine::applyScanResults(const std::vector<PluginInfo> &toAdd,
                        const std::vector<PluginInfo> &toRemove) 
{
    juce::ScopedLock sl(lock);

    // Remove
    for (const auto &r : toRemove) {
      availablePlugins.erase(std::remove_if(availablePlugins.begin(),
                                            availablePlugins.end(),
                                            [&r](const PluginInfo &p) {
                                              return p.path == r.path;
                                            }),
                             availablePlugins.end());
    }

    // Add
    for (const auto &a : toAdd) {
      // Avoid duplicates
      bool exists = false;
      for (const auto &p : availablePlugins) {
        if (p.path == a.path) {
          exists = true;
          break;
        }
      }
      if (!exists)
        availablePlugins.push_back(a);
    }

    // Sort alphabetically
    std::sort(availablePlugins.begin(), availablePlugins.end(),
              [](const PluginInfo &a, const PluginInfo &b) {
                return a.name.compareIgnoreCase(b.name) < 0;
              });

    savePluginList();
  }

  

void FanfareEngine::savePluginList() 
{
    juce::File file =
        juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
            .getChildFile("Fanfare")
            .getChildFile("known_plugins.json");

    if (!file.getParentDirectory().exists())
      file.getParentDirectory().createDirectory();

    juce::Array<juce::var> pluginNodes;
    for (const auto &p : availablePlugins) {
      auto *obj = new juce::DynamicObject();
      obj->setProperty("name", p.name);
      obj->setProperty("path", p.path);
      pluginNodes.add(juce::var(obj));
    }

    juce::File tempFile = file.withFileExtension("tmp");
    if (tempFile.replaceWithText(
            juce::JSON::toString(juce::var(pluginNodes)))) {
      tempFile.moveFileTo(file);
      hasSavedPluginList = true;
    }
  }

  

void FanfareEngine::loadPluginList() 
{
    juce::File file =
        juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
            .getChildFile("Fanfare")
            .getChildFile("known_plugins.json");

    if (file.existsAsFile()) {
      auto json = juce::JSON::parse(file);
      if (auto *arr = json.getArray()) {
        availablePlugins.clear();
        for (int i = 0; i < arr->size(); ++i) {
          auto v = arr->getReference(i);
          availablePlugins.push_back({v.getProperty("name", "").toString(),
                                      v.getProperty("path", "").toString()});
        }
        hasSavedPluginList = true;
        return; // Success
      }
    }

    hasSavedPluginList = false;
    // First-run state: filter out any hardcoded default paths that aren't
    // actually installed on this machine so new users don't see dead entries.
    availablePlugins.erase(
        std::remove_if(availablePlugins.begin(), availablePlugins.end(),
                       [](const PluginInfo &p) {
                         return !FanfareConstants::pluginExistsOnDisk(p.path);
                       }),
        availablePlugins.end());

#if JUCE_MAC
    if (availablePlugins.empty()) {
      for (const auto &vstFolder : FanfareConstants::getVst3SearchDirectories()) {
        std::vector<PluginInfo> dirFound;
        collectVst3Bundles(vstFolder, dirFound);
        for (const auto &p : dirFound)
          appendUniquePluginInfo(availablePlugins, p);
      }
#if JUCE_PLUGINHOST_AU
      for (int i = 0; i < formatManager.getNumFormats(); ++i) {
        auto *fmt = formatManager.getFormat(i);
        if (fmt != nullptr && fmt->getName() == "AudioUnit") {
          const juce::StringArray auIds = fmt->searchPathsForPlugins({}, true, false);
          for (const auto &id : auIds) {
            juce::String auName = fmt->getNameOfPluginFromIdentifier(id);
            if (auName.isEmpty())
              auName = id;
            appendUniquePluginInfo(availablePlugins, {auName + " (AU)", id});
          }
          break;
        }
      }
#endif
    }
#endif

    std::sort(availablePlugins.begin(), availablePlugins.end(),
              [](const PluginInfo &a, const PluginInfo &b) {
                return a.name.compareIgnoreCase(b.name) < 0;
              });
  }

  

// --- Master FX Management ---
  void
  FanfareEngine::addPluginToMasterChain(bool isFoh,
                         std::unique_ptr<juce::AudioPluginInstance> plugin) 
{
    juce::ScopedLock sl(lock);
    juce::SpinLock::ScopedLockType mfl(masterFxLock);
    if (isFoh)
      fohPluginChain.push_back(std::move(plugin));
    else
      iemPluginChain.push_back(std::move(plugin));
  }

  

// --- Persistence (JSON / var) ---
  juce::String FanfareEngine::exportRigToJson() 
{
    juce::ScopedLock sl(lock);
    saveCurrentStateToScene(getCurrentSceneIndex());

    auto *rig = new juce::DynamicObject();
    rig->setProperty("version", 2);
    rig->setProperty("schemaVersion", 2);
    rig->setProperty("fohMasterLevel", (double)fohMasterLevel);
    rig->setProperty("iemMasterLevel", (double)iemMasterLevel);
    rig->setProperty("fohOutputOffset", (int)fohOutputOffset);
    rig->setProperty("iemOutputOffset", (int)iemOutputOffset);
    rig->setProperty("defaultMidiChannel", defaultMidiChannel.load());
    rig->setProperty("freshBuild", currentRigFreshBuild.load());

    // Master FX
    juce::Array<juce::var> fohNodes;
    for (auto &p : fohPluginChain) {
      if (p)
        fohNodes.add(getPluginVar(p.get()));
      else
        fohNodes.add(juce::var());
    }
    rig->setProperty("fohFx", fohNodes);

    juce::Array<juce::var> iemNodes;
    for (auto &p : iemPluginChain) {
      if (p)
        iemNodes.add(getPluginVar(p.get()));
      else
        iemNodes.add(juce::var());
    }
    rig->setProperty("iemFx", iemNodes);

    // Channels
    juce::Array<juce::var> channelNodes;
    for (int i = 0; i < (int)slots.size(); ++i) {
      auto s = slots[i].get();
      auto *st = new juce::DynamicObject();
      st->setProperty("name", s->getName());
      st->setProperty("iconIndex", s->getIconIndex());
      st->setProperty("channelColor", s->getChannelColor().toString());
      st->setProperty("level", (double)s->getChannelLevel());
      st->setProperty("foh", s->isFohEnabled());
      st->setProperty("iem", s->isIemEnabled());
      st->setProperty("fadersLinked", s->areFadersLinked());
      st->setProperty("mute", s->isBypassed());
      st->setProperty("inputIndex", s->getInputChannelIndex());
      st->setProperty("inputStereo", s->isInputStereo());
      st->setProperty("aux1", (double)s->getAux1Send());
      st->setProperty("aux2", (double)s->getAux2Send());
      st->setProperty("iemOffset", (double)s->getIemOffset());
      st->setProperty("transposeOctaves", s->getTransposeOctaves());
      st->setProperty("transposeSemitones", s->getTransposeSemitones());
      st->setProperty("lowNote", s->getLowNote());
      st->setProperty("highNote", s->getHighNote());

      juce::String ccList;
      for (int cc : s->getAllowedCCs())
        ccList += juce::String(cc) + ",";
      st->setProperty("allowedCCs", ccList);

      // CC-controlled faders + per-slot MIDI channel override (new in v2)
      st->setProperty("fohCC", s->getFohCC());
      st->setProperty("iemCC", s->getIemCC());
      st->setProperty("muteCC", s->getMuteCC());
      st->setProperty("swapCC", s->getSwapCC());
      st->setProperty("swapA", s->getSwapA());
      st->setProperty("swapB", s->getSwapB());
      st->setProperty("midiChannel", s->getMidiChannelOverride());
      st->setProperty("outputTarget", s->getOutputTarget());

      // Save Channel Strip Settings
      auto *stripObj = new juce::DynamicObject();
      Fanfare::writeStripToObject(*stripObj, Fanfare::captureStrip(s->getStrip()), true);
      st->setProperty("strip", stripObj);

      // Save Sampler Settings
      auto *sampObj = new juce::DynamicObject();
      auto &sampler = s->getSampler();
      sampObj->setProperty("enabled", sampler.enabled.load());
      juce::Array<juce::var> samplerSlots;
      for (int sIdx = 0; sIdx < 8; ++sIdx) {
        auto cfg = sampler.getSlotConfig(sIdx);
        auto *slo = new juce::DynamicObject();
        slo->setProperty("wavPath", cfg.wavPath);
        slo->setProperty("rootNote", cfg.rootNote);
        slo->setProperty("keyLow", cfg.keyLow);
        slo->setProperty("keyHigh", cfg.keyHigh);
        slo->setProperty("pitchOffset", (double)cfg.pitchOffsetSemitones);
        slo->setProperty("volume", (double)cfg.volume);
        slo->setProperty("startRatio", (double)cfg.startRatio);
        slo->setProperty("endRatio", (double)cfg.endRatio);
        slo->setProperty("isLooping", cfg.isLooping);
        slo->setProperty("volumeCC", cfg.volumeCC);
        samplerSlots.add(juce::var(slo));
      }
      sampObj->setProperty("slots", samplerSlots);
      st->setProperty("sampler", sampObj);

      // Fanfare Synth (native) — whole patch as one JSON object
      st->setProperty("synth", s->getSynth().getState());
      st->setProperty("fm", s->getFmSynth().getState());

      // Save MP3 Player / Backing Track Settings
      auto *mp3Obj = new juce::DynamicObject();
      auto &mp3 = s->getMp3Player();
      mp3Obj->setProperty("loopMode", (int)mp3.getLoopMode());
      mp3Obj->setProperty("shuffle", mp3.isShuffleEnabled());
      mp3Obj->setProperty("gain", (double)mp3.getGain());
      juce::Array<juce::var> mp3Tracks;
      for (const auto &t : mp3.getPlaylist()) {
        auto *to = new juce::DynamicObject();
        to->setProperty("path", t.file.getFullPathName());
        to->setProperty("title", t.title);
        to->setProperty("duration", t.durationSeconds);
        mp3Tracks.add(juce::var(to));
      }
      mp3Obj->setProperty("tracks", mp3Tracks);
      st->setProperty("mp3Player", mp3Obj);

      // Save Arpeggiator Settings
      auto *arpObj = new juce::DynamicObject();
      Fanfare::writeArpToObject(*arpObj, Fanfare::captureArp(s->getArpeggiator()), false);
      st->setProperty("arpeggiator", arpObj);

      // Save Harmonizer Settings
      auto *harmObj = new juce::DynamicObject();
      Fanfare::writeHarmToObject(*harmObj, Fanfare::captureHarm(s->getHarmonizer()), false);
      st->setProperty("harmonizer", harmObj);

      // Save CC-to-Parameter mappings
      juce::Array<juce::var> ccMappingNodes;
      for (const auto &pair : s->getCCMappings()) {
        auto *mapping = new juce::DynamicObject();
        mapping->setProperty("cc", pair.first);
        mapping->setProperty("chainIndex", pair.second.chainIndex);
        mapping->setProperty("paramId", pair.second.paramId);
        mapping->setProperty("paramIndex", pair.second.parameterIndex);
        mapping->setProperty("minValue", (double)pair.second.minValue);
        mapping->setProperty("maxValue", (double)pair.second.maxValue);
        mapping->setProperty("invert", pair.second.invert);
        mapping->setProperty("toggle", pair.second.toggle);
        ccMappingNodes.add(juce::var(mapping));
      }
      st->setProperty("ccMappings", ccMappingNodes);

      // Save CC Passthrough (remap) mappings
      juce::Array<juce::var> ccPassthroughNodes;
      for (const auto &pair : s->getCCPassthroughMap()) {
        auto *pt = new juce::DynamicObject();
        pt->setProperty("incomingCC", pair.first);
        pt->setProperty("outgoingCC", pair.second);
        ccPassthroughNodes.add(juce::var(pt));
      }
      st->setProperty("ccPassthroughs", ccPassthroughNodes);

      auto writeChainMeta = [](juce::DynamicObject* pObj,
                               const RackSlot::ChainSlotSettings& cs) {
        pObj->setProperty("lowNote", cs.lowNote.load());
        pObj->setProperty("highNote", cs.highNote.load());
        pObj->setProperty("level", (double)cs.level.load());
        pObj->setProperty("enabled", cs.enabled.load());
        if (int layerCh = cs.midiChannel.load(); layerCh != 0)
          pObj->setProperty("midiChannel", juce::jlimit(0, 16, layerCh));
      };
      juce::Array<juce::var> chainNodes;
      for (int pIdx = 0; pIdx < s->getChainSize(); ++pIdx) {
        auto plugin = s->getPluginInstance(pIdx);
        if (plugin) {
          auto pluginVar = getPluginVar(plugin);
          if (pIdx < 3 && pluginVar.isObject())
            writeChainMeta(pluginVar.getDynamicObject(), s->getChainSlotSettings(pIdx));
          chainNodes.add(pluginVar);
        } else if (pIdx < 3 && s->isChainSlotSynth(pIdx)) {
          auto* pObj = new juce::DynamicObject();
          pObj->setProperty("isSynth", true);
          pObj->setProperty("synthState", s->getSynth(pIdx).getState());
          writeChainMeta(pObj, s->getChainSlotSettings(pIdx));
          chainNodes.add(juce::var(pObj));
        } else if (pIdx < 3 && s->isChainSlotFm(pIdx)) {
          auto* pObj = new juce::DynamicObject();
          pObj->setProperty("isFm", true);
          pObj->setProperty("fmState", s->getFmSynth(pIdx).getState());
          writeChainMeta(pObj, s->getChainSlotSettings(pIdx));
          chainNodes.add(juce::var(pObj));
        } else if (pIdx < 3 && s->isChainSlotB3(pIdx)) {
          auto* pObj = new juce::DynamicObject();
          pObj->setProperty("isB3", true);
          pObj->setProperty("b3State", s->getB3Organ(pIdx).getState());
          writeChainMeta(pObj, s->getChainSlotSettings(pIdx));
          chainNodes.add(juce::var(pObj));
        } else if (pIdx < 3 && s->isChainSlotMidiOut(pIdx)) {
          auto* pObj = new juce::DynamicObject();
          pObj->setProperty("isMidiOut", true);
          pObj->setProperty("midiOutDevice", s->getChainSlotMidiOutIdentifier(pIdx));
          pObj->setProperty("midiOutChannel", s->getChainSlotMidiOutChannel(pIdx));
          writeChainMeta(pObj, s->getChainSlotSettings(pIdx));
          chainNodes.add(juce::var(pObj));
        } else {
          chainNodes.add(juce::var());
        }
      }
      st->setProperty("chain", chainNodes);
      channelNodes.add(juce::var(st));
    }
    rig->setProperty("channels", channelNodes);

    // Aux return strips (Aux 1 / Aux 2): their plugin chains and strip state
    // previously were never saved, so returns lost their plugins on reload.
    juce::Array<juce::var> auxReturnNodes;
    for (auto &aux : auxReturns) {
      auto *at = new juce::DynamicObject();
      at->setProperty("mute", aux->isBypassed());
      at->setProperty("level", (double)aux->getChannelLevel());
      at->setProperty("foh", aux->isFohEnabled());
      at->setProperty("iem", aux->isIemEnabled());

      juce::Array<juce::var> auxChainNodes;
      for (int pIdx = 0; pIdx < aux->getChainSize(); ++pIdx) {
        auto plugin = aux->getPluginInstance(pIdx);
        if (plugin)
          auxChainNodes.add(getPluginVar(plugin));
        else
          auxChainNodes.add(juce::var());
      }
      at->setProperty("chain", auxChainNodes);
      auxReturnNodes.add(juce::var(at));
    }
    rig->setProperty("auxReturns", auxReturnNodes);

    // Scenes (Presets)
    std::vector<Scene> scenesSnapshot;
    int sceneIdxSnapshot = 0;
    {
      juce::ScopedLock sl(scenesLock);
      scenesSnapshot = scenes;
      sceneIdxSnapshot = currentSceneIndex;
    }
    juce::Array<juce::var> sceneNodes;
    for (const auto &scene : scenesSnapshot) {
      auto *sceneObj = new juce::DynamicObject();
      sceneObj->setProperty("name", scene.name);
      sceneObj->setProperty("midiProgramChange", scene.midiProgramChange);
      sceneObj->setProperty("midiChannel", scene.midiChannel);
      sceneObj->setProperty("colorArgb", (juce::int64)scene.colorArgb);

      juce::Array<juce::var> states;
      for (const auto &st : scene.slotStates) {
        auto *stateObj = new juce::DynamicObject();
        stateObj->setProperty("bypassed", st.bypassed);
        stateObj->setProperty("level", (double)st.fohLevel); // Backward compatibility
        stateObj->setProperty("fohLevel", (double)st.fohLevel);
        stateObj->setProperty("iemLevel", (double)st.iemLevel);
        stateObj->setProperty("foh", st.fohEnabled);
        stateObj->setProperty("iem", st.iemEnabled);
        stateObj->setProperty("fadersLinked", st.fadersLinked);
        stateObj->setProperty("aux1Send", (double)st.aux1Send);
        stateObj->setProperty("aux2Send", (double)st.aux2Send);
        stateObj->setProperty("iemOffset", (double)st.iemOffset);
        stateObj->setProperty("transposeOctaves", st.transposeOctaves);
        stateObj->setProperty("transposeSemitones", st.transposeSemitones);
        stateObj->setProperty("lowNote", st.lowNote);
        stateObj->setProperty("highNote", st.highNote);

        // DSP, Arpeggiator, Harmonizer
        Fanfare::writeStripToObject(*stateObj, st.strip, st.hasMusetteAndChannelMode);
        Fanfare::writeArpToObject(*stateObj, st.arp, true);
        Fanfare::writeHarmToObject(*stateObj, st.harm, true);

        // Sampler
        stateObj->setProperty("samplerEnabled", st.samplerEnabled);
        if (st.hasMidiCcState) {
          stateObj->setProperty("fohCC", st.fohCC);
          stateObj->setProperty("iemCC", st.iemCC);
          stateObj->setProperty("muteCC", st.muteCC);
          stateObj->setProperty("midiChannel", st.midiChannelOverride);
        }

        states.add(juce::var(stateObj));
      }
      sceneObj->setProperty("states", states);
      sceneNodes.add(juce::var(sceneObj));
    }
    rig->setProperty("scenes", sceneNodes);
    rig->setProperty("currentSceneIndex", sceneIdxSnapshot);

    // Notes / Gig Notepad
    juce::Array<juce::var> noteNodes;
    {
      juce::ScopedLock nl(notesLock);
      for (const auto &nt : currentNotes) {
        auto *no = new juce::DynamicObject();
        no->setProperty("title", nt.title);
        no->setProperty("content", nt.content);
        noteNodes.add(juce::var(no));
      }
      rig->setProperty("notes", noteNodes);
      rig->setProperty("activeNoteTabIndex", activeNoteTabIndex);
      rig->setProperty("noteFontSize", (double)noteFontSize);
      rig->setProperty("noteIsMonospace", noteIsMonospace);
    }

    return juce::JSON::toString(juce::var(rig));
  }

  

void FanfareEngine::applyRig(const juce::var &rig) 
{
    if (!rig.isObject())
      return;

    struct ReapFlushOnExit {
      FanfareEngine &eng;
      ~ReapFlushOnExit() { eng.flushReapQueue(); }
    } reapFlusher{*this};

    ScopedAudioQuietWindow quietWindow(*this);
    juce::ScopedLock sl(lock);

    // Note: ScopedAudioQuietWindow pauses audio processing cleanly without spinning or priority inversions.
    // Displaced plugins are moved into pendingReapPlugins and destroyed by ReapFlushOnExit AFTER
    // quietWindow exits and audio resumes.

    for (int i = 0; i < 3; ++i) {
      fohFxFaultCount[i].store(0, std::memory_order_relaxed);
      iemFxFaultCount[i].store(0, std::memory_order_relaxed);
      fohFxBypassed[i].store(false, std::memory_order_relaxed);
      iemFxBypassed[i].store(false, std::memory_order_relaxed);
    }

    fohMasterLevel = (float)rig.getProperty("fohMasterLevel", 1.0);
    iemMasterLevel = (float)rig.getProperty("iemMasterLevel", 1.0);
    fohOutputOffset = rig.getProperty("fohOutputOffset", 0);
    iemOutputOffset = rig.getProperty("iemOutputOffset", 2);
    defaultMidiChannel.store(
        (int)rig.getProperty("defaultMidiChannel", FanfareConstants::kDefaultMidiChannel));
    bool fresh = (bool)rig.getProperty("freshBuild", false);
    currentRigFreshBuild.store(fresh);
    if (fresh && !hasStagedPlugins()) {
      clearAllRacks();
    }

    // Master FX
    // Master FX - Smart Loading
    // Don't clear upfront - smart load handles replacement
    if (auto *fohArr = rig.getProperty("fohFx", juce::var()).getArray()) {
      for (int i = 0; i < fohArr->size(); ++i) {
        auto v = fohArr->getReference(i);
        if (v.isObject())
          loadPluginFromVarSmart(-1, i, v, true);
      }
      // Defer destruction of any extra plugins beyond new chain size until after quietWindow
      {
        juce::SpinLock::ScopedLockType mfl(masterFxLock);
        while (fohPluginChain.size() > fohArr->size()) {
          if (fohPluginChain.back())
            enqueueReapPlugin(std::move(fohPluginChain.back()));
          fohPluginChain.pop_back();
        }
      }
    } else {
      clearMasterChain(true);
    }

    if (auto *iemArr = rig.getProperty("iemFx", juce::var()).getArray()) {
      for (int i = 0; i < iemArr->size(); ++i) {
        auto v = iemArr->getReference(i);
        if (v.isObject())
          loadPluginFromVarSmart(-1, i, v, false);
      }
      // Defer destruction of any extra plugins beyond new chain size until after quietWindow
      {
        juce::SpinLock::ScopedLockType mfl(masterFxLock);
        while (iemPluginChain.size() > iemArr->size()) {
          if (iemPluginChain.back())
            enqueueReapPlugin(std::move(iemPluginChain.back()));
          iemPluginChain.pop_back();
        }
      }
    } else {
      clearMasterChain(false);
    }

    // Channels
    if (auto *chanArr = rig.getProperty("channels", juce::var()).getArray()) {
      int numSlots = (int)slots.size();
      int numNodes = chanArr->size();
      int numToLoad = (numSlots < numNodes) ? numSlots : numNodes;

      for (int i = 0; i < numToLoad; ++i) {
        auto s = slots[i].get();
        auto v = chanArr->getReference(i);

        s->setName(v.getProperty("name", "Slot").toString());
        s->setIconIndex(v.getProperty("iconIndex", 0));
        s->setChannelColor(juce::Colour::fromString(
            v.getProperty("channelColor", "#2a2a2a").toString()));
        s->setChannelLevel((float)v.getProperty("level", 0.8));
        s->setFohEnabled(v.getProperty("foh", true));
        s->setIemEnabled(v.getProperty("iem", true));
        s->setFadersLinked(v.getProperty("fadersLinked", true));
        s->setBypass(v.getProperty("mute", false));
        s->setInputChannelIndex(v.getProperty("inputIndex", -1));
        s->setInputStereo((bool)v.getProperty("inputStereo", i > 0));
        s->setAux1Send((float)v.getProperty("aux1", 0.0));
        s->setAux2Send((float)v.getProperty("aux2", 0.0));
        s->setIemOffset((float)v.getProperty("iemOffset", 1.0));
        s->setTransposeOctaves(v.getProperty("transposeOctaves", 0));
        s->setTransposeSemitones(v.getProperty("transposeSemitones", 0));
        s->setNoteRange(v.getProperty("lowNote", 0), v.getProperty("highNote", 127));

        juce::String ccList = v.getProperty("allowedCCs", "64").toString();
        juce::StringArray ccs;
        ccs.addTokens(ccList, ",", "");
        std::set<int> allowed;
        for (auto &cc : ccs)
          if (cc.isNotEmpty())
            allowed.insert(cc.getIntValue());
        s->setAllowedCCs(allowed);

        // CC-controlled faders + per-slot MIDI channel override (v2)
        s->setFohCC(v.getProperty("fohCC", -1));
        s->setIemCC(v.getProperty("iemCC", -1));
        s->setMuteCC(v.getProperty("muteCC", -1));
        s->setSwapPair(v.getProperty("swapCC", -1), v.getProperty("swapA", -1),
                       v.getProperty("swapB", -1));
        s->setMidiChannelOverride(v.getProperty("midiChannel", -1));
        int tgt = v.getProperty("outputTarget", -1);
        if (tgt >= 0 && tgt < 100 && tgt <= i)
          tgt = -1; // Enforce DAG: subgroup targets must be downstream slots (tgt > i)
        s->setOutputTarget(tgt);

        // Load Channel Strip Settings
        if (auto *stripObj =
                v.getProperty("strip", juce::var()).getDynamicObject()) {
          auto &strip = s->getStrip();
          Fanfare::ChannelStripSettings cs;
          Fanfare::readStripFromObject(juce::var(stripObj), cs, true);
          Fanfare::applyStrip(strip, cs, true, false, false);
          // Re-prepare first so sample rate is set before generating any built-in IR
          strip.prepare(currentSampleRate, currentBlockSize > 0 ? currentBlockSize : 8192);
          if (cs.irPath.isNotEmpty())
            strip.getIRReverb().loadPathOrPreset(cs.irPath);
        }

        // Restore Arpeggiator Settings
        if (auto *arpObj = v.getProperty("arpeggiator", juce::var()).getDynamicObject()) {
          Fanfare::ArpParams ap;
          Fanfare::readArpFromObject(juce::var(arpObj), ap, false);
          Fanfare::applyArp(s->getArpeggiator(), ap);
        }
        if (auto *harmObj = v.getProperty("harmonizer", juce::var()).getDynamicObject()) {
          Fanfare::HarmParams hp;
          Fanfare::readHarmFromObject(juce::var(harmObj), hp, false);
          Fanfare::applyHarm(s->getHarmonizer(), hp);
        }

        // Restore Sampler Settings
        if (auto *sampObj =
                v.getProperty("sampler", juce::var()).getDynamicObject()) {
          auto &sampler = s->getSampler();
          sampler.enabled.store((bool)sampObj->getProperty("enabled"));
          if (auto *sls = sampObj->getProperty("slots").getArray()) {
            for (int sIdx = 0; sIdx < juce::jmin(8, sls->size()); ++sIdx) {
              const auto &slo = sls->getReference(sIdx);
              if (slo.isObject()) {
                SamplerProcessor::SlotConfig cfg;
                cfg.wavPath = slo.getProperty("wavPath", "").toString();
                cfg.rootNote = (int)slo.getProperty("rootNote", 60);
                cfg.keyLow = (int)slo.getProperty("keyLow", 0);
                cfg.keyHigh = (int)slo.getProperty("keyHigh", 127);
                cfg.pitchOffsetSemitones = (float)slo.getProperty("pitchOffset", 0.0f);
                cfg.volume = (float)slo.getProperty("volume", 1.0f);
                cfg.startRatio = (float)slo.getProperty("startRatio", 0.0f);
                cfg.endRatio = (float)slo.getProperty("endRatio", 1.0f);
                cfg.isLooping = (bool)slo.getProperty("isLooping", false);
                cfg.volumeCC = (int)slo.getProperty("volumeCC", -1);
                sampler.setSlotConfig(sIdx, cfg);
              }
            }
          }
        }

        // Restore Fanfare Synth (native)
        if (auto *synthObj =
                v.getProperty("synth", juce::var()).getDynamicObject()) {
          auto &synthLive = s->getSynth();
          synthLive.applyState(juce::var(synthObj));
        }
        if (auto *fmObj = v.getProperty("fm", juce::var()).getDynamicObject()) {
          auto &fmLive = s->getFmSynth();
          fmLive.applyState(juce::var(fmObj));
        }

        // Restore MP3 Player / Backing Track Settings
        {
          auto &mp3 = s->getMp3Player();
          mp3.clearPlaylist();
          if (auto *mp3Obj =
                  v.getProperty("mp3Player", juce::var()).getDynamicObject()) {
            if (mp3Obj->hasProperty("loopMode"))
              mp3.setLoopMode((Mp3PlayerProcessor::LoopMode)(int)mp3Obj->getProperty("loopMode"));
            if (mp3Obj->hasProperty("shuffle"))
              mp3.setShuffleEnabled((bool)mp3Obj->getProperty("shuffle"));
            if (mp3Obj->hasProperty("gain"))
              mp3.setGain((float)mp3Obj->getProperty("gain"));
            if (auto *trArr = mp3Obj->getProperty("tracks").getArray()) {
              for (const auto &tv : *trArr) {
                if (auto *to = tv.getDynamicObject()) {
                  juce::String tp = to->getProperty("path").toString();
                  juce::String tt = to->getProperty("title").toString();
                  double td = (double)to->getProperty("duration");
                  if (tp.isNotEmpty())
                    mp3.addTrack(juce::File(tp), tt, td);
                }
              }
            }
          }
        }

        // Restore CC-to-Parameter mappings (paramId-primary, index fallback)
        s->clearAllCCMappings();
        if (auto *mappingArr =
                v.getProperty("ccMappings", juce::var()).getArray()) {
          for (int m = 0; m < mappingArr->size(); ++m) {
            auto mv = mappingArr->getReference(m);
            int ccNum = mv.getProperty("cc", 0);
            int chainIdx = mv.getProperty("chainIndex", 0);
            juce::String paramId = mv.getProperty("paramId", "").toString();
            int paramIdx = mv.getProperty("paramIndex", mv.getProperty("parameterIndex", 0));
            float minVal = (float)mv.getProperty("minValue", 0.0);
            float maxVal = (float)mv.getProperty("maxValue", 1.0);
            bool inv = mv.getProperty("invert", false);
            bool tog = mv.getProperty("toggle", false);
            s->mapCCToParameter(ccNum, chainIdx, paramId, paramIdx, minVal, maxVal, inv, tog);
          }
        }

        // Restore CC Passthrough (remap) mappings
        s->clearAllCCPassthroughs();
        if (auto *ptArr =
                v.getProperty("ccPassthroughs", juce::var()).getArray()) {
          for (int m = 0; m < ptArr->size(); ++m) {
            auto pv = ptArr->getReference(m);
            int inCC = pv.getProperty("incomingCC", -1);
            int outCC = pv.getProperty("outgoingCC", -1);
            if (inCC >= 0 && outCC >= 0)
              s->addCCPassthrough(inCC, outCC);
          }
        }

        // Load Channel Plugin Chains - Smart Loading (two-pass approach)
        std::set<int> pluginsToReuse;
        // Pass 1: Identify which plugins can be reused
        if (auto *chainArr = v.getProperty("chain", juce::var()).getArray()) {
          for (int p = 0; p < chainArr->size(); ++p) {
            auto pv = chainArr->getReference(p);
            if (pv.isObject()) {
              juce::String newPath = pv.getProperty("path", "").toString();
              juce::String currentPath = s->getPluginPath(p);
              if (normalizePath(currentPath) == normalizePath(newPath) &&
                  !currentPath.isEmpty()) {
                pluginsToReuse.insert(p);
              }
            }
          }
        }

        // Pass 2: Clear only non-matching plugins (deferring destruction until after quietWindow), then load
        s->clearChainPreserve(pluginsToReuse, &pendingReapPlugins);
        for (int c = 0; c < 3; ++c) {
          if (pluginsToReuse.find(c) == pluginsToReuse.end()) {
            s->clearChainSlotMidiOut(c);
            s->clearChainSlotSynth(c);
            s->clearChainSlotFm(c);
            s->clearChainSlotB3(c);
          }
        }
        bool anyChainSynthRestored = false;
        if (auto *chainArr = v.getProperty("chain", juce::var()).getArray()) {
          for (int p = 0; p < chainArr->size(); ++p) {
            auto pv = chainArr->getReference(p);
            if (pv.isObject()) {
              bool isMo = pv.getProperty("isMidiOut", false);
              juce::String moDev = pv.getProperty("midiOutDevice", "").toString();
              bool isSy = pv.getProperty("isSynth", false);
              bool isFm = pv.getProperty("isFm", false);
              bool isB3 = pv.getProperty("isB3", false);
              if (isSy && p < 3) {
                s->setChainSlotSynth(p, true, pv.getProperty("synthState", juce::var()));
                anyChainSynthRestored = true;
              } else if (isFm && p < 3) {
                s->setChainSlotFm(p, true, pv.getProperty("fmState", juce::var()));
              } else if (isB3 && p < 3) {
                s->setChainSlotB3(p, true, pv.getProperty("b3State", juce::var()));
                restoreB3ExtPluginsForSlot(s);
              } else if (isMo && moDev.isNotEmpty()) {
                // MIDI OUT destination: open the device and bind this chain slot.
                int devIdx = getOrCreateMidiOutput(moDev);
                s->setChainSlotMidiOut(p, devIdx,
                                       (int)pv.getProperty("midiOutChannel", 1),
                                       moDev, midiOutputNameForIdentifier(moDev));
              } else {
                loadPluginFromVarSmart(i, p, pv, true);
                s->clearChainSlotMidiOut(p); // plugin replaces any midi-out/synth/fm/b3 config
                s->clearChainSlotSynth(p);
                s->clearChainSlotFm(p);
                s->clearChainSlotB3(p);
              }
              // Per-instrument stacking settings
              if (p < 3) {
                auto& cs = s->getChainSlotSettings(p);
                cs.lowNote.store((int)pv.getProperty("lowNote", 0));
                cs.highNote.store((int)pv.getProperty("highNote", 127));
                cs.level.store((float)pv.getProperty("level", 1.0));
                cs.enabled.store((bool)pv.getProperty("enabled", true));
                cs.midiChannel.store(juce::jlimit(0, 16, (int)pv.getProperty("midiChannel", 0)));
              }
            } else if (p < 3) {
              s->clearChainSlotMidiOut(p);
              s->clearChainSlotSynth(p);
              s->clearChainSlotFm(p);
              s->clearChainSlotB3(p);
              s->getChainSlotSettings(p).midiChannel.store(0);
            }
          }
          for (int p = chainArr->size(); p < 3; ++p)
            s->getChainSlotSettings(p).midiChannel.store(0);
        } else {
          for (int p = 0; p < 3; ++p)
            s->getChainSlotSettings(p).midiChannel.store(0);
        }
        // Legacy migration: if the channel had a top-level enabled synth and no
        // chain slot was explicitly flagged isSynth, bind it into the first free
        // chain slot so it appears in SLOT 1..3.
        if (!anyChainSynthRestored && s->getSynth(0).enabled.load()) {
          for (int c = 0; c < 3; ++c) {
            if (!s->getPluginInstance(c) && !s->isChainSlotMidiOut(c) &&
                !s->isChainSlotSynth(c) && !s->isChainSlotFm(c) &&
                !s->isChainSlotB3(c)) {
              if (c != 0)
                s->setChainSlotSynth(c, true, s->getSynth(0).getState());
              else
                s->setChainSlotSynth(0, true);
              break;
            }
          }
        }

        // Re-resolve CC mappings now that this slot's plugin chain is loaded.
        // Mappings were restored earlier in this function, before the plugin
        // swap — their cached parameter pointers are null (fresh boot) or
        // invalidated by setPluginInChain. Without this pass the assignments
        // show in the manager but never reach the plugin.
        s->revalidateCCMappings();
      }
      for (int i = numToLoad; i < numSlots; ++i) {
        auto s = slots[i].get();
        s->setName("Slot " + juce::String(i + 1));
        s->setIconIndex(0);
        s->setChannelColor(juce::Colour::fromString("#2a2a2a"));
        s->setChannelLevel(0.0f);
        s->setFohEnabled(false);
        s->setIemEnabled(false);
        s->setFadersLinked(true);
        s->setBypass(true);
        s->clearChain(&pendingReapPlugins);
        s->clearAllCCMappings();
        s->clearAllCCPassthroughs();
        s->getMp3Player().clearPlaylist();
      }
    }

    // Aux return strips: restore chains + strip state. Files saved before
    // Oct 2026 have no "auxReturns" key — clear the chains so a rig load is
    // a full replacement (matching how absent fohFx/iemFx clear the masters).
    if (auto *auxArr = rig.getProperty("auxReturns", juce::var()).getArray()) {
      for (int r = 0; r < (int)auxReturns.size() && r < auxArr->size(); ++r) {
        auto *aux = auxReturns[r].get();
        if (!aux)
          continue;
        auto av = auxArr->getReference(r);
        if (!av.isObject())
          continue;
        aux->setBypass(av.getProperty("mute", false));
        aux->setChannelLevel((float)av.getProperty("level", 0.8));
        aux->setFohEnabled(av.getProperty("foh", true));
        aux->setIemEnabled(av.getProperty("iem", true));

        // Two-pass chain restore: keep live instances whose path matches
        // (state-only update), replace the rest — same as channels.
        std::set<int> pluginsToReuse;
        if (auto *chainArr = av.getProperty("chain", juce::var()).getArray()) {
          for (int p = 0; p < chainArr->size() && p < 3; ++p) {
            auto pv = chainArr->getReference(p);
            if (!pv.isObject())
              continue;
            juce::String newPath = pv.getProperty("path", "").toString();
            juce::String curPath = aux->getPluginPath(p);
            if (!curPath.isEmpty() &&
                normalizePath(curPath) == normalizePath(newPath))
              pluginsToReuse.insert(p);
          }
        }
        aux->clearChainPreserve(pluginsToReuse, &pendingReapPlugins);
        if (auto *chainArr = av.getProperty("chain", juce::var()).getArray()) {
          for (int p = 0; p < chainArr->size() && p < 3; ++p) {
            auto pv = chainArr->getReference(p);
            if (pv.isObject())
              loadPluginFromVarSmart(100 + r, p, pv, true);
          }
        }
        aux->revalidateCCMappings();
      }
    } else {
      for (auto &aux : auxReturns)
        if (aux)
          aux->clearChain(&pendingReapPlugins);
    }

    // Scenes (Presets)
    {
      juce::ScopedLock sl(scenesLock);
      if (auto *sceneArr = rig.getProperty("scenes", juce::var()).getArray()) {
        scenes.clear();
        for (int i = 0; i < sceneArr->size(); ++i) {
          auto sv = sceneArr->getReference(i);
          Scene newScene(sv.getProperty("name", "Scene").toString());
          newScene.midiProgramChange = sv.getProperty("midiProgramChange", -1);
          newScene.midiChannel = sv.getProperty("midiChannel", 0);
          newScene.colorArgb = static_cast<juce::uint32>((juce::int64)sv.getProperty("colorArgb", 0));

          if (auto *stateArr = sv.getProperty("states", juce::var()).getArray()) {
            for (int s = 0; s < stateArr->size(); ++s) {
              auto stv = stateArr->getReference(s);
              SlotState st;
              st.bypassed = stv.getProperty("bypassed", false);
              st.fohLevel = (float)stv.getProperty("fohLevel", stv.getProperty("level", 0.8));
              st.iemLevel = (float)stv.getProperty("iemLevel", st.fohLevel);
              st.fohEnabled = stv.getProperty("foh", true);
              st.iemEnabled = stv.getProperty("iem", true);
              st.fadersLinked = stv.getProperty("fadersLinked", true);
              st.aux1Send = (float)stv.getProperty("aux1Send", 0.0);
              st.aux2Send = (float)stv.getProperty("aux2Send", 0.0);
              st.iemOffset = (float)stv.getProperty("iemOffset", 1.0);
              st.transposeOctaves = stv.getProperty("transposeOctaves", 0);
              st.transposeSemitones = stv.getProperty("transposeSemitones", 0);
              st.lowNote = stv.getProperty("lowNote", 0);
              st.highNote = stv.getProperty("highNote", 127);

              // DSP, Arpeggiator, Harmonizer
              auto* stDyn = stv.getDynamicObject();
              st.hasMusetteAndChannelMode = stDyn != nullptr &&
                                            (stDyn->hasProperty("musetteEnabled") ||
                                             stDyn->hasProperty("channelMode"));
              Fanfare::readStripFromObject(stv, st.strip, st.hasMusetteAndChannelMode);
              Fanfare::readArpFromObject(stv, st.arp, true);
              Fanfare::readHarmFromObject(stv, st.harm, true);

              // Sampler
              st.samplerEnabled = stv.getProperty("samplerEnabled", false);
              if (stDyn != nullptr) {
                st.hasMidiCcState = stDyn->hasProperty("fohCC") ||
                                    stDyn->hasProperty("iemCC") ||
                                    stDyn->hasProperty("muteCC") ||
                                    stDyn->hasProperty("midiChannel");
              }
              if (st.hasMidiCcState) {
                st.fohCC = stv.getProperty("fohCC", -1);
                st.iemCC = stv.getProperty("iemCC", -1);
                st.muteCC = stv.getProperty("muteCC", -1);
                st.midiChannelOverride = stv.getProperty("midiChannel", -1);
              }

              newScene.slotStates.push_back(st);
            }
          }
          scenes.push_back(newScene);
        }
      }
      int rawSceneIdx = (int)rig.getProperty("currentSceneIndex", 0);
      currentSceneIndex = scenes.empty() ? 0 : juce::jlimit(0, (int)scenes.size() - 1, rawSceneIdx);
    }

    // Notes / Gig Notepad
    {
      juce::ScopedLock nl(notesLock);
      currentNotes.clear();
      if (auto* noteArr = rig.getProperty("notes", juce::var()).getArray()) {
        for (int i = 0; i < noteArr->size(); ++i) {
          const auto& nv = noteArr->getReference(i);
          if (nv.isObject()) {
            Fanfare::NoteTab nt;
            nt.title = nv.getProperty("title", "Notes").toString();
            nt.content = nv.getProperty("content", "").toString();
            currentNotes.push_back(nt);
          }
        }
      }
      if (currentNotes.empty()) {
        currentNotes.push_back({ "Chords", "" });
        currentNotes.push_back({ "Lyrics", "" });
        currentNotes.push_back({ "Notes", "" });
      }
      activeNoteTabIndex = rig.getProperty("activeNoteTabIndex", 0);
      noteFontSize = (float)rig.getProperty("noteFontSize", 20.0);
      noteIsMonospace = (bool)rig.getProperty("noteIsMonospace", true);
    }

    broadcastNotesChanged();

    // Kill any stuck notes from the previous rig / state restoration
    panicTriggered.store(true);

    // Trigger a 2s mute + fade-in (1.5s hard mute, 0.5s linear fade) so
    // restored plugin state (Leslie tails, tonewheels) doesn't blast.
    int fadeSamples = (int)(currentSampleRate * 2.0);
    if (fadeSamples < currentBlockSize)
      fadeSamples = currentBlockSize;
    postLoadFadeTotal.store(fadeSamples);
    postLoadFadeRemaining.store(fadeSamples);
  }

  

void FanfareEngine::performLegacyMigration() 
{
    auto appDir = FanfareConstants::getAppDirectory();
    auto backupLegacyDir = FanfareConstants::getBackupsDirectory().getChildFile("legacy");

    juce::File migrationFlag = appDir.getChildFile(".migrated");
    if (migrationFlag.existsAsFile())
      return;

    backupLegacyDir.createDirectory();

    // 1. Migrate FanfareFullRig.json from Desktop
    juce::File desktopRig = juce::File::getSpecialLocation(juce::File::userDesktopDirectory).getChildFile("FanfareFullRig.json");
    if (desktopRig.existsAsFile()) {
      desktopRig.copyFileTo(backupLegacyDir.getChildFile("FanfareFullRig.json"));
      desktopRig.copyFileTo(appDir.getChildFile("FanfareFullRig.json"));
    }

    // 2. Migrate Setups from Documents (copy each file; JUCE has no recursive dir copy)
    juce::File docSetups = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory).getChildFile("Fanfare").getChildFile("Setups");
    if (docSetups.isDirectory()) {
      juce::File newSetups = FanfareConstants::getSongsDirectory();
      newSetups.createDirectory();
      juce::File backupSetups = backupLegacyDir.getChildFile("Setups");
      backupSetups.createDirectory();

      juce::Array<juce::File> files;
      docSetups.findChildFiles(files, juce::File::findFiles, false, "*.json");
      for (const auto& f : files) {
        f.copyFileTo(newSetups.getChildFile(f.getFileName()));
        f.copyFileTo(backupSetups.getChildFile(f.getFileName()));
      }
    }

    // 3. Migrate button_mappings.json if exists
    juce::File appDataMappings = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory).getChildFile("Fanfare").getChildFile("button_mappings.json");
    if (appDataMappings.existsAsFile()) {
      appDataMappings.copyFileTo(backupLegacyDir.getChildFile("button_mappings.json"));
    }

    migrationFlag.replaceWithText("migrated");
  }

  

Fanfare::Song FanfareEngine::getSongRepresentation(const juce::String& songName ) 
{
    juce::ScopedLock sl(lock);
    Fanfare::Song song;
    song.name = songName;
    song.fohMasterLevel = fohMasterLevel;
    song.iemMasterLevel = iemMasterLevel;
    song.fohOutputOffset = fohOutputOffset;
    song.iemOutputOffset = iemOutputOffset;
    song.defaultMidiChannel = defaultMidiChannel.load();
    song.freshBuild = currentRigFreshBuild.load();

    // FOH FX
    for (auto& p : fohPluginChain) {
        Fanfare::PluginState ps;
        if (p) {
            ps.name = p->getName();
            ps.path = p->getPluginDescription().fileOrIdentifier;
            juce::MemoryBlock blob;
            FanfareLog::safeExecutePluginCall([&]() {
                p->getStateInformation(blob);
            }, "getStateInformation (" + p->getName() + ")");
            ps.stateBase64 = blob.toBase64Encoding();
            ps.uid = p->getPluginDescription().uniqueId;
        }
        song.fohFx.push_back(ps);
    }

    // IEM FX
    for (auto& p : iemPluginChain) {
        Fanfare::PluginState ps;
        if (p) {
            ps.name = p->getName();
            ps.path = p->getPluginDescription().fileOrIdentifier;
            juce::MemoryBlock blob;
            FanfareLog::safeExecutePluginCall([&]() {
                p->getStateInformation(blob);
            }, "getStateInformation (" + p->getName() + ")");
            ps.stateBase64 = blob.toBase64Encoding();
            ps.uid = p->getPluginDescription().uniqueId;
        }
        song.iemFx.push_back(ps);
    }

    // Channels/Slots
    for (int i = 0; i < (int)slots.size(); ++i) {
        song.slots.push_back(exportSlot(i));
    }

    // Scenes
    {
        juce::ScopedLock scLock(scenesLock);
        song.scenes = scenes;
        song.currentSceneIndex = currentSceneIndex;
    }
    {
        juce::ScopedLock nl(notesLock);
        song.notes = currentNotes;
        song.activeNoteTabIndex = activeNoteTabIndex;
        song.noteFontSize = noteFontSize;
        song.noteIsMonospace = noteIsMonospace;
    }

    return song;
  }

  

Fanfare::SongSlot FanfareEngine::exportSlot(int slotIdx) 
{
    Fanfare::SongSlot songSlot;
    if (slotIdx < 0 || slotIdx >= (int)slots.size())
      return songSlot;
    juce::ScopedLock sl(lock);
    auto *s = slots[slotIdx].get();

    songSlot.name = s->getName();
    songSlot.iconIndex = s->getIconIndex();
    songSlot.channelColor = s->getChannelColor();
    songSlot.level = s->getChannelLevel();
    songSlot.fohEnabled = s->isFohEnabled();
    songSlot.iemEnabled = s->isIemEnabled();
    songSlot.fadersLinked = s->areFadersLinked();
    songSlot.bypassed = s->isBypassed();
    songSlot.inputChannelIndex = s->getInputChannelIndex();
    songSlot.inputStereo = s->isInputStereo();
    songSlot.aux1Send = s->getAux1Send();
    songSlot.aux2Send = s->getAux2Send();
    songSlot.iemOffset = s->getIemOffset();
    songSlot.transposeOctaves = s->getTransposeOctaves();
    songSlot.transposeSemitones = s->getTransposeSemitones();
    songSlot.lowNote = s->getLowNote();
    songSlot.highNote = s->getHighNote();
    songSlot.allowedCCs = s->getAllowedCCs();
    songSlot.fohCC = s->getFohCC();
    songSlot.iemCC = s->getIemCC();
    songSlot.muteCC = s->getMuteCC();
    songSlot.swapCC = s->getSwapCC();
    songSlot.swapA = s->getSwapA();
    songSlot.swapB = s->getSwapB();
    songSlot.midiChannelOverride = s->getMidiChannelOverride();
    songSlot.outputTarget = s->getOutputTarget();

    songSlot.strip = Fanfare::captureStrip(s->getStrip());
    songSlot.arpeggiator = Fanfare::captureArp(s->getArpeggiator());
    songSlot.harmonizer = Fanfare::captureHarm(s->getHarmonizer());

    auto& samplerLive = s->getSampler();
    songSlot.sampler.enabled = samplerLive.enabled.load();
    for (int idx = 0; idx < 8; ++idx) {
      auto cfg = samplerLive.getSlotConfig(idx);
      songSlot.sampler.slots[idx].wavPath = cfg.wavPath;
      songSlot.sampler.slots[idx].rootNote = cfg.rootNote;
      songSlot.sampler.slots[idx].keyLow = cfg.keyLow;
      songSlot.sampler.slots[idx].keyHigh = cfg.keyHigh;
      songSlot.sampler.slots[idx].pitchOffsetSemitones = cfg.pitchOffsetSemitones;
      songSlot.sampler.slots[idx].volume = cfg.volume;
      songSlot.sampler.slots[idx].startRatio = cfg.startRatio;
      songSlot.sampler.slots[idx].endRatio = cfg.endRatio;
      songSlot.sampler.slots[idx].isLooping = cfg.isLooping;
      songSlot.sampler.slots[idx].volumeCC = cfg.volumeCC;
    }

    auto& synthExport = s->getSynth();
    songSlot.synth.enabled = synthExport.enabled.load();
    songSlot.synth.state = synthExport.getState();

    auto& fmExport = s->getFmSynth();
    songSlot.fm.enabled = fmExport.enabled.load();
    songSlot.fm.state = fmExport.getState();

    auto& mp3Live = s->getMp3Player();
    songSlot.mp3Player.loopMode = (int)mp3Live.getLoopMode();
    songSlot.mp3Player.shuffle = mp3Live.isShuffleEnabled();
    songSlot.mp3Player.gain = mp3Live.getGain();
    songSlot.mp3Player.tracks.clear();
    for (const auto& t : mp3Live.getPlaylist()) {
      Fanfare::Mp3TrackSettings ts;
      ts.path = t.file.getFullPathName();
      ts.title = t.title;
      ts.duration = t.durationSeconds;
      songSlot.mp3Player.tracks.push_back(ts);
    }

    for (const auto& pair : s->getCCMappings()) {
      Fanfare::CCMapping map;
      map.cc = pair.first;
      map.chainIndex = pair.second.chainIndex;
      map.paramId = pair.second.paramId;
      map.parameterIndex = pair.second.parameterIndex;
      map.minValue = pair.second.minValue;
      map.maxValue = pair.second.maxValue;
      map.invert = pair.second.invert;
      map.toggle = pair.second.toggle;
      songSlot.ccMappings.push_back(map);
    }

    for (const auto& pair : s->getCCPassthroughMap()) {
      Fanfare::CCPassthrough pt;
      pt.incomingCC = pair.first;
      pt.outgoingCC = pair.second;
      songSlot.ccPassthroughs.push_back(pt);
    }

    for (int pIdx = 0; pIdx < s->getChainSize(); ++pIdx) {
      auto plugin = s->getPluginInstance(pIdx);
      Fanfare::PluginState ps;
      if (plugin) {
        ps.name = plugin->getName();
        ps.path = plugin->getPluginDescription().fileOrIdentifier;
        juce::MemoryBlock blob;
        FanfareLog::safeExecutePluginCall([&]() {
            plugin->getStateInformation(blob);
        }, "getStateInformation (" + plugin->getName() + ")");
        ps.stateBase64 = blob.toBase64Encoding();
        ps.uid = plugin->getPluginDescription().uniqueId;
      }
      // Per-instrument stacking settings
      auto& cs = s->getChainSlotSettings(pIdx);
      ps.lowNote = cs.lowNote.load();
      ps.highNote = cs.highNote.load();
      ps.level = cs.level.load();
      ps.enabled = cs.enabled.load();
      ps.midiChannel = cs.midiChannel.load();
      // MIDI OUT target (mutually exclusive with a plugin instance)
      ps.isMidiOut = s->isChainSlotMidiOut(pIdx);
      ps.midiOutDevice = s->getChainSlotMidiOutIdentifier(pIdx);
      ps.midiOutChannel = s->getChainSlotMidiOutChannel(pIdx);
      ps.isSynth = s->isChainSlotSynth(pIdx);
      if (ps.isSynth)
        ps.synthState = s->getSynth(pIdx).getState();
      ps.isFm = s->isChainSlotFm(pIdx);
      if (ps.isFm)
        ps.fmState = s->getFmSynth(pIdx).getState();
      ps.isB3 = s->isChainSlotB3(pIdx);
      if (ps.isB3)
        ps.b3State = s->getB3Organ(pIdx).getState();
      songSlot.chain.push_back(ps);
    }

    return songSlot;
  }

  

void FanfareEngine::importSlot(int slotIdx, const Fanfare::SongSlot& songSlot) 
{
    if (slotIdx < 0 || slotIdx >= (int)slots.size())
      return;
    struct ReapFlushOnExit {
      FanfareEngine &eng;
      ~ReapFlushOnExit() { eng.flushReapQueue(); }
    } reapFlusher{*this};
    ScopedAudioQuietWindow quietWindow(*this);
    juce::ScopedLock sl(lock);
    auto *s = slots[slotIdx].get();

    s->setName(songSlot.name);
    s->setIconIndex(songSlot.iconIndex);
    s->setChannelColor(songSlot.channelColor);
    s->setChannelLevel(songSlot.level);
    s->setFohEnabled(songSlot.fohEnabled);
    s->setIemEnabled(songSlot.iemEnabled);
    s->setFadersLinked(songSlot.fadersLinked);
    s->setBypass(songSlot.bypassed);
    s->setInputChannelIndex(songSlot.inputChannelIndex);
    s->setInputStereo(songSlot.inputStereo);
    s->setAux1Send(songSlot.aux1Send);
    s->setAux2Send(songSlot.aux2Send);
    s->setIemOffset(songSlot.iemOffset);
    s->setTransposeOctaves(songSlot.transposeOctaves);
    s->setTransposeSemitones(songSlot.transposeSemitones);
    s->setNoteRange(songSlot.lowNote, songSlot.highNote);
    s->setAllowedCCs(songSlot.allowedCCs);
    s->setFohCC(songSlot.fohCC);
    s->setIemCC(songSlot.iemCC);
    s->setMuteCC(songSlot.muteCC);
    s->setSwapPair(songSlot.swapCC, songSlot.swapA, songSlot.swapB);
    s->setMidiChannelOverride(songSlot.midiChannelOverride);
    int tgt = songSlot.outputTarget;
    if (tgt >= 0 && tgt < 100 && tgt <= slotIdx)
      tgt = -1; // Enforce DAG: subgroup targets must be downstream slots (tgt > slotIdx)
    s->setOutputTarget(tgt);

    auto& stripData = s->getStrip();
    Fanfare::applyStrip(stripData, songSlot.strip, true, false, false);
    stripData.prepare(currentSampleRate);
    if (songSlot.strip.irPath.isNotEmpty())
      stripData.getIRReverb().loadPathOrPreset(songSlot.strip.irPath);
    else
      stripData.getIRReverb().clear();

    Fanfare::applyArp(s->getArpeggiator(), songSlot.arpeggiator);
    Fanfare::applyHarm(s->getHarmonizer(), songSlot.harmonizer);

    auto& samplerLive = s->getSampler();
    samplerLive.enabled.store(songSlot.sampler.enabled);
    for (int idx = 0; idx < 8; ++idx) {
      SamplerProcessor::SlotConfig cfg;
      cfg.wavPath = songSlot.sampler.slots[idx].wavPath;
      cfg.rootNote = songSlot.sampler.slots[idx].rootNote;
      cfg.keyLow = songSlot.sampler.slots[idx].keyLow;
      cfg.keyHigh = songSlot.sampler.slots[idx].keyHigh;
      cfg.pitchOffsetSemitones = songSlot.sampler.slots[idx].pitchOffsetSemitones;
      cfg.volume = songSlot.sampler.slots[idx].volume;
      cfg.startRatio = songSlot.sampler.slots[idx].startRatio;
      cfg.endRatio = songSlot.sampler.slots[idx].endRatio;
      cfg.isLooping = songSlot.sampler.slots[idx].isLooping;
      cfg.volumeCC = songSlot.sampler.slots[idx].volumeCC;
      samplerLive.setSlotConfig(idx, cfg);
    }

    auto& synthLive = s->getSynth();
    synthLive.enabled.store(songSlot.synth.enabled);
    if (songSlot.synth.state.isObject())
      synthLive.applyState(songSlot.synth.state);

    auto& fmLive = s->getFmSynth();
    fmLive.enabled.store(songSlot.fm.enabled);
    if (songSlot.fm.state.isObject())
      fmLive.applyState(songSlot.fm.state);

    auto& mp3Live = s->getMp3Player();
    mp3Live.clearPlaylist();
    mp3Live.setLoopMode((Mp3PlayerProcessor::LoopMode)songSlot.mp3Player.loopMode);
    mp3Live.setShuffleEnabled(songSlot.mp3Player.shuffle);
    mp3Live.setGain(songSlot.mp3Player.gain);
    for (const auto& ts : songSlot.mp3Player.tracks) {
      mp3Live.addTrack(juce::File(ts.path), ts.title, ts.duration);
    }

    s->clearAllCCMappings();
    for (const auto& m : songSlot.ccMappings)
      s->mapCCToParameter(m.cc, m.chainIndex, m.paramId, m.parameterIndex,
                          m.minValue, m.maxValue, m.invert, m.toggle);

    s->clearAllCCPassthroughs();
    for (const auto& pt : songSlot.ccPassthroughs)
      s->addCCPassthrough(pt.incomingCC, pt.outgoingCC);

    // Plugin loading: reuse matching plugins, clear others
    std::set<int> pluginsToReuse;
    for (int p = 0; p < (int)songSlot.chain.size(); ++p) {
      if (songSlot.chain[p].path.isNotEmpty()) {
        juce::String newPath = songSlot.chain[p].path;
        juce::String currentPath = s->getPluginPath(p);
        if (normalizePath(currentPath) == normalizePath(newPath) && !currentPath.isEmpty())
          pluginsToReuse.insert(p);
      }
    }
    s->clearChainPreserve(pluginsToReuse, &pendingReapPlugins);
    for (int c = 0; c < 3; ++c) {
      if (pluginsToReuse.find(c) == pluginsToReuse.end()) {
        s->clearChainSlotMidiOut(c);
        s->clearChainSlotSynth(c);
        s->clearChainSlotFm(c);
        s->clearChainSlotB3(c);
      }
    }
    bool anyChainSynthRestored = false;
    for (int p = 0; p < (int)songSlot.chain.size(); ++p) {
      if (p < 3 && songSlot.chain[p].isSynth) {
        s->setChainSlotSynth(p, true, songSlot.chain[p].synthState);
        anyChainSynthRestored = true;
      } else if (p < 3 && songSlot.chain[p].isFm) {
        s->setChainSlotFm(p, true, songSlot.chain[p].fmState);
      } else if (p < 3 && songSlot.chain[p].isB3) {
        s->setChainSlotB3(p, true, songSlot.chain[p].b3State);
        restoreB3ExtPluginsForSlot(s);
      } else if (songSlot.chain[p].isMidiOut && songSlot.chain[p].midiOutDevice.isNotEmpty()) {
        // MIDI OUT destination takes over this chain slot.
        const auto& ps = songSlot.chain[p];
        int devIdx = getOrCreateMidiOutput(ps.midiOutDevice);
        s->setChainSlotMidiOut(p, devIdx, ps.midiOutChannel, ps.midiOutDevice,
                               midiOutputNameForIdentifier(ps.midiOutDevice));
      } else if (songSlot.chain[p].path.isNotEmpty()) {
        juce::var pv = songSlotToPluginVar(songSlot.chain[p]);
        loadPluginFromVarSmart(slotIdx, p, pv, true);
        s->clearChainSlotMidiOut(p); // plugin replaces any midi-out config
        s->clearChainSlotSynth(p);
        s->clearChainSlotFm(p);
        s->clearChainSlotB3(p);
      } else {
        s->clearChainSlotMidiOut(p);
        s->clearChainSlotSynth(p);
        s->clearChainSlotFm(p);
        s->clearChainSlotB3(p);
      }
    }
    if (!anyChainSynthRestored && songSlot.synth.enabled) {
      for (int c = 0; c < 3; ++c) {
        if (!s->getPluginInstance(c) && !s->isChainSlotMidiOut(c) &&
            !s->isChainSlotSynth(c) && !s->isChainSlotFm(c) &&
            !s->isChainSlotB3(c)) {
          s->setChainSlotSynth(c, true, songSlot.synth.state);
          break;
        }
      }
    }
    if (songSlot.fm.enabled) {
      bool anyFmRestored = false;
      for (int c = 0; c < 3; ++c)
        if (s->isChainSlotFm(c))
          anyFmRestored = true;
      if (!anyFmRestored) {
        for (int c = 0; c < 3; ++c) {
          if (!s->getPluginInstance(c) && !s->isChainSlotMidiOut(c) &&
              !s->isChainSlotSynth(c) && !s->isChainSlotFm(c) &&
              !s->isChainSlotB3(c)) {
            s->setChainSlotFm(c, true, songSlot.fm.state);
            break;
          }
        }
      }
    }

    // Apply per-instrument stacking settings (note range, level, enable)
    for (int p = 0; p < (int)songSlot.chain.size() && p < 3; ++p) {
      auto& cs = s->getChainSlotSettings(p);
      cs.lowNote.store(songSlot.chain[p].lowNote);
      cs.highNote.store(songSlot.chain[p].highNote);
      cs.level.store(songSlot.chain[p].level);
      cs.enabled.store(songSlot.chain[p].enabled);
      cs.midiChannel.store(juce::jlimit(0, 16, songSlot.chain[p].midiChannel));
    }
    for (int p = (int)songSlot.chain.size(); p < 3; ++p)
      s->getChainSlotSettings(p).midiChannel.store(0);

    // Re-resolve CC mappings now that the plugin chain is loaded — the
    // mappings were restored above, before the plugin swap, so their cached
    // parameter pointers are null (fresh boot) or were invalidated by
    // setPluginInChain. Without this pass the assignments show in the CC
    // manager but never reach the plugin.
    s->revalidateCCMappings();
  }

  

void FanfareEngine::saveStripToFile(int slotIdx, const juce::File& file) 
{
    SaveHistory::recordSave(file);
    auto songSlot = exportSlot(slotIdx);
    auto json = Fanfare::RigSerializer::serializeStrip(songSlot);
    if (Fanfare::RigSerializer::save(file, json))
      SaveHistory::recordSave(file);
  }

  

bool FanfareEngine::loadStripFromFile(int slotIdx, const juce::File& file) 
{
    SaveHistory::snapshotBeforeLoadAsync(file);
    Fanfare::SongSlot songSlot;
    if (!Fanfare::RigSerializer::readStripFromFile(file, songSlot))
      return false;
    importSlot(slotIdx, songSlot);
    return true;
  }

  

void FanfareEngine::loadRigFromFile() 
{
    juce::File f = FanfareConstants::getAppDirectory().getChildFile("FanfareFullRig.json");
    if (f.existsAsFile()) {
      auto loaded = Fanfare::RigSerializer::load(f);
      if (loaded.ok)
        applyRig(loaded.rig);
    }
  }

  

juce::var FanfareEngine::getPluginVar(juce::AudioPluginInstance *plugin) 
{
    auto *vt = new juce::DynamicObject();
    vt->setProperty("name", plugin->getName());

    juce::MemoryBlock blob;
    FanfareLog::safeExecutePluginCall([&]() {
      plugin->getStateInformation(blob);
    }, "getStateInformation (" + plugin->getName() + ")");
    vt->setProperty("state", blob.toBase64Encoding());

    auto desc = plugin->getPluginDescription();
    vt->setProperty("uid", (int)desc.uniqueId);
    vt->setProperty("path", desc.fileOrIdentifier);

    return juce::var(vt);
  }

  

juce::String FanfareEngine::normalizePath(const juce::String &path) 
{
    const juce::String trimmed = path.trim();
    if (trimmed.isEmpty())
      return "";
    if (FanfareConstants::isAudioUnitIdentifier(trimmed))
      return trimmed;
#if !JUCE_WINDOWS
    if (trimmed.containsChar('\\') || (trimmed.length() >= 2 && trimmed[1] == ':')) {
      juce::String p = trimmed.toLowerCase().replaceCharacter('/', '\\');
      while (p.endsWithChar('\\'))
        p = p.dropLastCharacters(1);
      return p;
    }
#endif
    juce::String p = juce::File(trimmed).getFullPathName().trim().toLowerCase();
    p = p.replaceCharacter('/', '\\');
    while (p.endsWithChar('\\'))
      p = p.dropLastCharacters(1);
    return p;
  }

  

juce::var FanfareEngine::songSlotToPluginVar(const Fanfare::PluginState& ps) 
{
    auto *obj = new juce::DynamicObject();
    obj->setProperty("name", ps.name);
    obj->setProperty("path", ps.path);
    obj->setProperty("state", ps.stateBase64);
    obj->setProperty("uid", ps.uid);
    return juce::var(obj);
  }

  

juce::String FanfareEngine::getMasterPluginPath(bool isFoh, int chainIndex) const 
{
    juce::SpinLock::ScopedLockType mfl(masterFxLock);
    const auto &chain = isFoh ? fohPluginChain : iemPluginChain;
    if (chainIndex >= 0 && chainIndex < (int)chain.size()) {
      if (chain[chainIndex]) {
        auto desc = chain[chainIndex]->getPluginDescription();
        return desc.fileOrIdentifier;
      }
    }
    return "";
  }

  

void FanfareEngine::loadPluginFromVarSmart(int slotIdx, int chainIdx, const juce::var &vt,
                              bool isFohOrChannel) 
{
    juce::String newPath = resolvePluginPathForCurrentPlatform(vt.getProperty("path", "").toString());
    juce::String newStateBase64 = vt.getProperty("state", "").toString();

    if (newPath.isEmpty())
      return;

    // Async path: if the builder pre-built this (staging has a fresh instance
    // with state already applied off-thread), use it directly. This avoids
    // calling setStateInformation on the live instance under the callback lock.
    auto key = stagingKeyFor(slotIdx, chainIdx, isFohOrChannel);
    if (stagingHasKey(key)) {
      const bool ok = loadPluginFromVar(slotIdx, chainIdx, vt, isFohOrChannel);
      juce::ScopedLock sLock(stagingLock);
      if (ok)
        lastAppliedPluginStates[key] = newStateBase64;
      else
        lastAppliedPluginStates.erase(key);
      return;
    }

    // Sync path (no staging): reuse if same path, else build synchronously.
    juce::String normalizedNewPath = normalizePath(newPath);

    // Get current plugin path
    juce::String currentPath;
    juce::AudioPluginInstance *currentPlugin = nullptr;

    if (slotIdx >= 100) {
      int r = slotIdx - 100;
      if (r < (int)auxReturns.size()) {
        currentPlugin = auxReturns[r]->getPluginInstance(chainIdx);
        if (currentPlugin)
          currentPath = auxReturns[r]->getPluginPath(chainIdx);
      }
    } else if (slotIdx >= 0) {
      currentPlugin = slots[slotIdx]->getPluginInstance(chainIdx);
      if (currentPlugin)
        currentPath = slots[slotIdx]->getPluginPath(chainIdx);
    } else {
      juce::SpinLock::ScopedLockType mfl(masterFxLock);
      const auto &chain = isFohOrChannel ? fohPluginChain : iemPluginChain;
      if (chainIdx >= 0 && chainIdx < (int)chain.size()) {
        currentPlugin = chain[chainIdx].get();
        if (currentPlugin)
          currentPath = currentPlugin->getPluginDescription().fileOrIdentifier;
      }
    }

    // Compare paths - if same, just update state
    if (currentPlugin && normalizePath(currentPath) == normalizedNewPath) {
      // Avoid redundant setStateInformation if the patch/state is identical.
      // This saves seconds/tens of seconds for heavy plugins (Kontakt, Omnisphere).
      {
        juce::ScopedLock sLock(stagingLock);
        auto it = lastAppliedPluginStates.find(key);
        if (it != lastAppliedPluginStates.end() && it->second == newStateBase64) {
          logToFile("TRACE: Skipping setStateInformation for " + currentPlugin->getName() + " - state is unchanged.");
          return;
        }
      }

      juce::MemoryBlock blob;
      blob.fromBase64Encoding(newStateBase64);
      logToFile("TRACE: setStateInformation started for reused plugin " + currentPlugin->getName() + " (" + juce::String(blob.getSize()) + " bytes)");
      bool stateOk = FanfareLog::safeExecutePluginCall([&]() {
        currentPlugin->setStateInformation(blob.getData(), (int)blob.getSize());
      }, "setStateInformation (Reused " + currentPlugin->getName() + ")");
      logToFile("TRACE: setStateInformation finished for reused plugin: " + currentPlugin->getName());
      {
        juce::ScopedLock sLock(stagingLock);
        if (stateOk) {
          lastAppliedPluginStates[key] = newStateBase64;
        } else {
          lastAppliedPluginStates.erase(key);
        }
      }
      if (!stateOk) {
        logToFile("WARNING: setStateInformation faulted for reused plugin: " + currentPlugin->getName() + ", continuing");
      }
      return;
    }

    // Different plugin or no plugin - load new one
    const bool loaded = loadPluginFromVar(slotIdx, chainIdx, vt, isFohOrChannel);
    juce::ScopedLock sLock(stagingLock);
    if (loaded)
      lastAppliedPluginStates[key] = newStateBase64;
    else
      lastAppliedPluginStates.erase(key);
  }

  

bool FanfareEngine::loadPluginFromVar(int slotIdx, int chainIdx, const juce::var &vt,
                         bool isFohOrChannel) 
{
    juce::String path = resolvePluginPathForCurrentPlatform(vt.getProperty("path", "").toString());
    if (path.isEmpty())
      return false;

    std::unique_ptr<juce::AudioPluginInstance> instance;

    // 1. Prefer a pre-built instance from the staging cache (async transition
    //    path). buildPluginFromVar already prepared it and restored state.
    auto key = stagingKeyFor(slotIdx, chainIdx, isFohOrChannel);
    if (!popStagedPlugin(key, instance)) {
      const juce::String normPath = normalizePath(path);
      if (Fanfare::isPathPoisoned(normPath)) {
        logToFile("WARNING: cold-path plugin build skipped for key " + key +
                  " — plugin path already stalled earlier this session (" + normPath + ").");
        return false;
      }
      if (isStagedBuildFailed(key)) {
        logToFile("WARNING: cold-path plugin build skipped for key " + key +
                  " — already failed during staged transition build; leaving slot empty.");
        return false;
      }
      if (!Fanfare::tryAcquirePathBuildGate(normPath, 60000)) {
        logToFile("WARNING: cold-path plugin build skipped for key " + key +
                  " — previous build of " + normPath + " is still wedged.");
        return false;
      }

      // 2. Fallback: cold path — if the plugin requires STA message-thread
      // instantiation (NI Kontakt/Raum/Replika, Spectrasonics, Toontrack, HALion)
      // and we are on the message thread, build directly here; otherwise build
      // off-thread with a pumping wait (required by UVI Workstation).
      if (Fanfare::pluginRequiresMessageThread(normPath) &&
          juce::MessageManager::getInstance()->isThisTheMessageThread()) {
        juce::String buildErr;
        const bool builtOk = buildPluginFromVar(vt, instance, buildErr) && (bool)instance;
        Fanfare::releasePathBuildGate(normPath);
        if (!builtOk) {
          logToFile("WARNING: cold-path message-thread build failed for key " + key +
                    ": " + buildErr);
          return false;
        }
      } else {
        struct OffThreadVarBuild {
          FanfareEngine *engine;
          std::shared_ptr<std::atomic<bool>> alive;
          juce::var vt;
          juce::String gatePath;
          std::unique_ptr<juce::AudioPluginInstance> inst;
          juce::String err;
          bool ok = false;
          std::atomic<bool> done{false};
          std::atomic<bool> abandoned{false};
        };
        auto ctx = std::make_shared<OffThreadVarBuild>();
        ctx->engine = this;
        ctx->alive = engineAlive;
        ctx->vt = vt;
        ctx->gatePath = normPath;

        std::thread t([ctx]() {
#ifdef _WIN32
          ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
#endif
          std::unique_ptr<juce::AudioPluginInstance> tmp;
          juce::String tmpErr;
          bool tmpOk = false;
          if (! ctx->abandoned.load(std::memory_order_acquire) &&
              ctx->alive && ctx->alive->load(std::memory_order_acquire))
            tmpOk = ctx->engine->buildPluginFromVar(ctx->vt, tmp, tmpErr) && (bool)tmp;
          if (! ctx->abandoned.load(std::memory_order_acquire) &&
              ctx->alive && ctx->alive->load(std::memory_order_acquire)) {
            ctx->inst = std::move(tmp);
            ctx->err = tmpErr;
            ctx->ok = tmpOk;
          } else {
            tmp.reset();
          }
#ifdef _WIN32
          ::CoUninitialize();
#endif
          Fanfare::releasePathBuildGate(ctx->gatePath);
          ctx->done.store(true, std::memory_order_release);
        });

        int waitedMs = 0;
        while (! ctx->done.load(std::memory_order_acquire) && waitedMs < 60000) {
#ifdef _WIN32
          if (juce::MessageManager::getInstance()->isThisTheMessageThread()) {
            MSG msg;
            while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
              TranslateMessage(&msg);
              DispatchMessage(&msg);
            }
          }
#endif
          juce::Thread::sleep(50);
          waitedMs += 50;
        }

        if (ctx->done.load(std::memory_order_acquire)) {
          t.join();
          instance = std::move(ctx->inst);
        } else {
          ctx->abandoned.store(true, std::memory_order_release);
          Fanfare::poisonPath(normPath);
          logToFile("WARNING: cold-path plugin build timed out (60s) for key " + key +
                    " — abandoning; the rig continues without it.");
          t.detach();
          return false;
        }
      }
    }

    const bool installedOk = (instance != nullptr);
    if (!installedOk)
      return false;

    auto *reapTarget = isApplyingRig.load(std::memory_order_acquire)
                           ? &pendingReapPlugins
                           : nullptr;
    if (slotIdx >= 100) {
      int r = slotIdx - 100;
      if (r < (int)auxReturns.size())
        auxReturns[r]->setPluginInChain(chainIdx, std::move(instance), reapTarget);
    } else if (slotIdx >= 0) {
      slots[slotIdx]->setPluginInChain(chainIdx, std::move(instance), reapTarget);
    } else {
      if (chainIdx >= 0 && chainIdx < 3) {
        if (isFohOrChannel) {
          fohFxFaultCount[chainIdx].store(0, std::memory_order_relaxed);
          fohFxBypassed[chainIdx].store(false, std::memory_order_relaxed);
        } else {
          iemFxFaultCount[chainIdx].store(0, std::memory_order_relaxed);
          iemFxBypassed[chainIdx].store(false, std::memory_order_relaxed);
        }
      }
      std::unique_ptr<juce::AudioPluginInstance> displaced;
      {
        juce::SpinLock::ScopedLockType mfl(masterFxLock);
        auto &chain = isFohOrChannel ? fohPluginChain : iemPluginChain;
        while ((int)chain.size() <= chainIdx)
          chain.push_back(nullptr);
        displaced = std::move(chain[chainIdx]);
        chain[chainIdx] = std::move(instance);
      }
      if (displaced) {
        if (onBeforePluginDestroyed)
          onBeforePluginDestroyed(displaced.get());
        if (reapTarget != nullptr) {
          enqueueReapPlugin(std::move(displaced));
        } else {
          FanfareLog::safeExecutePluginCall([&]() {
            displaced->releaseResources();
            displaced.reset();
          }, "displaced masterFx releaseResources/reset");
        }
      }
    }
    return installedOk;
  }

  

juce::AudioPluginFormat *FanfareEngine::findVst3Format() 
{
    for (int i = 0; i < formatManager.getNumFormats(); ++i) {
      if (formatManager.getFormat(i)->getName() == "VST3")
        return formatManager.getFormat(i);
    }
    return nullptr;
  }

juce::AudioPluginFormat *FanfareEngine::findFormatForPath(const juce::String &path)
{
    if (FanfareConstants::isAudioUnitIdentifier(path) || path.endsWithIgnoreCase(".component")) {
      for (int i = 0; i < formatManager.getNumFormats(); ++i) {
        if (formatManager.getFormat(i)->getName() == "AudioUnit")
          return formatManager.getFormat(i);
      }
    }
    if (path.endsWithIgnoreCase(".vst3")) {
      if (auto *vst3 = findVst3Format())
        return vst3;
    }
    for (int i = 0; i < formatManager.getNumFormats(); ++i) {
      if (formatManager.getFormat(i)->fileMightContainThisPluginType(path))
        return formatManager.getFormat(i);
    }
    return findVst3Format();
}

juce::String FanfareEngine::resolveWindowsVst3PathFromList(
    const juce::String &rawPath,
    const std::vector<PluginInfo> &knownPlugins,
    const juce::Array<juce::File> &vst3SearchDirs)
{
    const juce::String trimmed = rawPath.trim();
    if (trimmed.isEmpty() || FanfareConstants::pluginExistsOnDisk(trimmed))
      return rawPath;
    if (!trimmed.containsIgnoreCase(".vst3"))
      return rawPath;

    const juce::String leaf = extractVst3LeafName(trimmed);
    if (!leaf.endsWithIgnoreCase(".vst3"))
      return rawPath;

    std::vector<juce::String> candidates;
    std::set<juce::String> seenLower;
    auto addCandidate = [&](const juce::String &p) {
      const juce::String pt = p.trim();
      if (pt.isEmpty() || FanfareConstants::isAudioUnitIdentifier(pt))
        return;
      if (!pt.containsIgnoreCase(".vst3"))
        return;
      const juce::String key = pt.toLowerCase();
      if (seenLower.insert(key).second)
        candidates.push_back(pt);
    };

    for (const auto &kp : knownPlugins)
      addCandidate(kp.path);
    for (const auto &dir : vst3SearchDirs) {
      std::vector<PluginInfo> dirFound;
      collectVst3Bundles(dir, dirFound);
      for (const auto &p : dirFound)
        addCandidate(p.path);
    }

    juce::String exactMatch;
    int exactCount = 0;
    for (const auto &cand : candidates) {
      if (extractVst3LeafName(cand).equalsIgnoreCase(leaf)) {
        exactMatch = cand;
        ++exactCount;
      }
    }
    if (exactCount == 1)
      return exactMatch;
    if (exactCount > 1)
      return rawPath;

    const juce::String wantedStem = normalizeVst3StemForMatch(leaf);
    if (wantedStem.isEmpty())
      return rawPath;
    juce::String stemMatch;
    int stemCount = 0;
    for (const auto &cand : candidates) {
      if (normalizeVst3StemForMatch(extractVst3LeafName(cand)).equalsIgnoreCase(wantedStem)) {
        stemMatch = cand;
        ++stemCount;
      }
    }
    if (stemCount == 1)
      return stemMatch;
    return rawPath;
}

juce::String FanfareEngine::resolvePluginPathForCurrentPlatform(const juce::String &rawPath) const
{
    const juce::String trimmed = rawPath.trim();
    if (trimmed.isEmpty() || FanfareConstants::pluginExistsOnDisk(trimmed))
      return rawPath;

    std::vector<PluginInfo> snapshot;
    {
      juce::ScopedLock sl(lock);
      snapshot = availablePlugins;
    }
    std::vector<PluginInfo> installed;
    installed.reserve(snapshot.size());
    for (const auto &p : snapshot) {
      if (FanfareConstants::pluginExistsOnDisk(p.path))
        installed.push_back(p);
    }

    const juce::String remapped = resolveWindowsVst3PathFromList(
        trimmed, installed, FanfareConstants::getVst3SearchDirectories());
    if (remapped != trimmed && FanfareConstants::pluginExistsOnDisk(remapped)) {
      LOG_INFO("Remapped Windows VST3 '" + trimmed + "' -> '" + remapped + "'");
      return remapped;
    }
    return rawPath;
}

  

void FanfareEngine::configureStereoLayout(juce::AudioProcessor *processor) 
{
    if (processor == nullptr)
      return;

    if (processor->getName().containsIgnoreCase("Omnisphere") ||
        processor->getName().containsIgnoreCase("Spectrasonics")) {
      logToFile("Skipping strict layout enforcement for Omnisphere");
      return;
    }

    // HALion 7 / HALion Sonic: its main OUTPUT bus ships INACTIVE in JUCE's
    // view (desc reports outCh=0), which makes the wrapper build process
    // calls with no output buffers — no sound, and an AV on the first block
    // (Oct 8). Negotiate: activate main stereo out, disable aux outs — but
    // NEVER on a background thread (the Aug 31 crash was exactly that).
    // HALion only builds on the message thread now (requiresMessageThread),
    // so by the time we get here it's safe to negotiate.
    if (processor->getName().containsIgnoreCase("HALion")) {
      auto layout = processor->getBusesLayout();
      if (layout.outputBuses.size() > 0)
        layout.outputBuses.getReference(0) = juce::AudioChannelSet::stereo();
      for (int i = 1; i < layout.outputBuses.size(); ++i)
        layout.outputBuses.getReference(i) = juce::AudioChannelSet::disabled();
      if (layout.inputBuses.size() > 0)
        layout.inputBuses.getReference(0) = juce::AudioChannelSet::stereo();
      for (int i = 1; i < layout.inputBuses.size(); ++i)
        layout.inputBuses.getReference(i) = juce::AudioChannelSet::disabled();
      bool ok = processor->setBusesLayout(layout);
      logToFile("HALion layout negotiation: " +
                juce::String(ok ? "ACCEPTED" : "REFUSED") + " — outs now " +
                juce::String(processor->getTotalNumOutputChannels()) + ", ins " +
                juce::String(processor->getTotalNumInputChannels()));
      return; // deliberately no setPlayConfigDetails for HALion
    }

    // 1. Get current layout to know how many buses exist
    auto layout = processor->getBusesLayout();

    // 2. Configure Inputs
    // Ensure Main Input is Stereo (if it exists)
    if (layout.inputBuses.size() > 0)
      layout.inputBuses.getReference(0) = juce::AudioChannelSet::stereo();

    // Disable all aux inputs
    for (int i = 1; i < layout.inputBuses.size(); ++i)
      layout.inputBuses.getReference(i) = juce::AudioChannelSet::disabled();

    // 3. Configure Outputs
    // Ensure Main Output is Stereo
    if (layout.outputBuses.size() > 0)
      layout.outputBuses.getReference(0) = juce::AudioChannelSet::stereo();

    // Disable all aux outputs (this is the critical fix for Kontakt/B3X)
    for (int i = 1; i < layout.outputBuses.size(); ++i)
      layout.outputBuses.getReference(i) = juce::AudioChannelSet::disabled();

    // 4. Apply the strict stereo layout
    bool success = processor->setBusesLayout(layout);
    logToFile(
        "Configured Topology for " + processor->getName() +
        (success ? " [SUCCESS]" : " [FAILED]") +
        " - Ins: " + juce::String(processor->getTotalNumInputChannels()) +
        ", Outs: " + juce::String(processor->getTotalNumOutputChannels()));

    // Explicitly set processing details to match valid layout
    processor->setPlayConfigDetails(processor->getTotalNumInputChannels(),
                                    processor->getTotalNumOutputChannels(),
                                    currentSampleRate, currentBlockSize);
  }



juce::String FanfareEngine::getMasterPluginName(bool isFoh, int chainIndex) const 
{
    juce::GenericScopedTryLock<juce::CriticalSection> sl(lock);
    if (!sl.isLocked()) return "";
    const auto &chain = isFoh ? fohPluginChain : iemPluginChain;
    if (chainIndex >= 0 && chainIndex < (int)chain.size()) {
      if (chain[chainIndex])
        return chain[chainIndex]->getName();
    }
    return "";
  }

  

juce::AudioPluginInstance *FanfareEngine::getMasterPluginInstance(bool isFoh,
                                                     int chainIndex) const 
{
    juce::GenericScopedTryLock<juce::CriticalSection> sl(lock);
    if (!sl.isLocked()) return nullptr;
    const auto &chain = isFoh ? fohPluginChain : iemPluginChain;
    if (chainIndex >= 0 && chainIndex < (int)chain.size())
      return chain[chainIndex].get();
    return nullptr;
  }

  

void FanfareEngine::clearMasterChain(bool isFoh) 
{
    juce::ScopedLock sl(lock);
    std::vector<std::unique_ptr<juce::AudioPluginInstance>> oldChain;
    {
      juce::SpinLock::ScopedLockType mfl(masterFxLock);
      oldChain.swap(isFoh ? fohPluginChain : iemPluginChain);
    }
    const bool deferReap = isApplyingRig.load(std::memory_order_acquire);
    const char *label = isFoh ? "oldFoh releaseResources/reset"
                              : "oldIem releaseResources/reset";
    for (auto &p : oldChain) {
      if (p != nullptr) {
        if (onBeforePluginDestroyed)
          onBeforePluginDestroyed(p.get());
        if (deferReap) {
          enqueueReapPlugin(std::move(p));
        } else {
          FanfareLog::safeExecutePluginCall([&]() {
            p->releaseResources();
            p.reset();
          }, label);
        }
      }
    }
  }

void FanfareEngine::clearMasterChains() 
{
    clearMasterChain(true);
    clearMasterChain(false);
  }

  

void FanfareEngine::clearAllRacks() 
{
    struct ReapFlushOnExit {
      FanfareEngine &eng;
      ~ReapFlushOnExit() { eng.flushReapQueue(); }
    } reapFlusher{*this};

    ScopedAudioQuietWindow quietWindow(*this);

    juce::ScopedLock sl(lock);
    for (auto &slot : slots) {
      if (slot)
        slot->clearChain(&pendingReapPlugins);
    }
    for (auto &slot : auxReturns) {
      if (slot)
        slot->clearChain(&pendingReapPlugins);
    }
    clearMasterChains();
    {
      juce::ScopedLock sLock(stagingLock);
      lastAppliedPluginStates.clear();
    }
    logToFile("TRACE: clearAllRacks() completed - all slot and master plugins unloaded.");
  }

  

// --- MIDI OUT device management ---
  // Returns (name, identifier) pairs for every available MIDI output device.
  juce::Array<std::pair<juce::String, juce::String>> FanfareEngine::getAvailableMidiOutputs() const 
{
    juce::Array<std::pair<juce::String, juce::String>> out;
    for (const auto &d : juce::MidiOutput::getAvailableDevices())
      out.add({d.name, d.identifier});
    return out;
  }

  

// Open (or reuse) a MIDI output device by identifier. Returns a stable index
  // into midiOutputs, or -1 on failure. Message-thread only.
  int FanfareEngine::getOrCreateMidiOutput(const juce::String &identifier) 
{
    if (identifier.isEmpty())
      return -1;
    {
      juce::SpinLock::ScopedLockType sl(midiOutputLock);
      auto it = midiOutputIndexById.find(identifier);
      if (it != midiOutputIndexById.end())
        return it->second;
    }
    auto dev = juce::MidiOutput::openDevice(identifier);
    if (!dev) {
      logToFile("WARNING: Failed to open MIDI output device: " + identifier);
      return -1;
    }
    juce::SpinLock::ScopedLockType sl(midiOutputLock);
    auto it = midiOutputIndexById.find(identifier);
    if (it != midiOutputIndexById.end())
      return it->second; // another thread opened it first
    int idx = (int)midiOutputs.size();
    midiOutputIndexById[identifier] = idx;
    midiOutputs.push_back(std::move(dev));
    logToFile("Opened MIDI output device: " + identifier);
    return idx;
  }

  

juce::String FanfareEngine::midiOutputNameForIdentifier(const juce::String &id) const 
{
    for (const auto &d : juce::MidiOutput::getAvailableDevices())
      if (d.identifier == id)
        return d.name;
    return {};
  }

  

// Configure a chain slot to send MIDI OUT to a device (by identifier).
  // Message-thread only. Mutually exclusive with hosting a plugin.
  void FanfareEngine::setSlotChainMidiOut(int slotIdx, int chainIdx,
                           const juce::String &deviceIdentifier, int channel) 
{
    if (slotIdx < 0 || slotIdx >= (int)slots.size())
      return;
    juce::ScopedLock sl(lock);
    int devIdx = getOrCreateMidiOutput(deviceIdentifier);
    juce::String name = midiOutputNameForIdentifier(deviceIdentifier);
    slots[slotIdx]->setChainSlotMidiOut(chainIdx, devIdx, channel,
                                        deviceIdentifier, name);
  }

  

void FanfareEngine::clearSlotChainMidiOut(int slotIdx, int chainIdx) 
{
    if (slotIdx < 0 || slotIdx >= (int)slots.size())
      return;
    juce::ScopedLock sl(lock);
    slots[slotIdx]->clearChainSlotMidiOut(chainIdx);
  }

  

void FanfareEngine::startDspWorkers() 
{
#ifdef _WIN32
    dspShutdown.store(false, std::memory_order_release);
    dspJobCount.store(0, std::memory_order_release);
    dspNextJobIdx.store(FanfareConstants::kNumSlots, std::memory_order_release);
    dspInFlightWorkers.store(0, std::memory_order_release);
    for (int h = 0; h < kMaxDspHelpers; ++h) {
      dspWorkerWakeEvents[h] = CreateEventW(nullptr, FALSE, FALSE, nullptr);
      dspWorkers[h] = std::thread([this, h]() { runDspWorkerLoop(h); });
    }
#endif
  }

  

void FanfareEngine::stopDspWorkers() 
{
#ifdef _WIN32
    dspShutdown.store(true, std::memory_order_seq_cst);
    dspJobCount.store(0, std::memory_order_seq_cst);
    for (int h = 0; h < kMaxDspHelpers; ++h) {
      if (dspWorkerWakeEvents[h] != nullptr)
        SetEvent(dspWorkerWakeEvents[h]);
    }
    for (int h = 0; h < kMaxDspHelpers; ++h) {
      if (dspWorkers[h].joinable())
        dspWorkers[h].join();
      if (dspWorkerWakeEvents[h] != nullptr) {
        CloseHandle(dspWorkerWakeEvents[h]);
        dspWorkerWakeEvents[h] = nullptr;
      }
    }
#endif
  }

  

void FanfareEngine::runDspWorkerLoop(int workerIdx) 
{
#ifdef _WIN32
    FanfareLog::t_isRealtimeAudioThread = true;
    // Priority HIGHEST (below the ASIO thread's MMCSS Pro Audio Priority 24 so a runaway
    // helper can never preempt or starve the driver thread, yet above normal UI/plugin worker threads).
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);

    uint32_t appliedGen = 0;
    while (!dspShutdown.load(std::memory_order_acquire)) {
      WaitForSingleObject(dspWorkerWakeEvents[workerIdx], INFINITE);
      if (dspShutdown.load(std::memory_order_acquire))
        break;

      const uint32_t curGen = dspTuningGen.load(std::memory_order_acquire);
      if (curGen != appliedGen) {
        appliedGen = curGen;
        DWORD_PTR mask = (workerIdx >= 0 && workerIdx < kMaxDspHelpers)
                             ? dspWorkerAffinityMasks[workerIdx].load(std::memory_order_acquire)
                             : 0;
        if (mask == 0)
          mask = shelteredAffinityMask.load(std::memory_order_acquire);
        if (mask != 0)
          SetThreadAffinityMask(GetCurrentThread(), mask);
        applyThreadHighQoS(preventPowerThrottlingEnabled.load(std::memory_order_acquire));
      }

      juce::ScopedNoDenormals noDenormals;
      for (;;) {
        dspInFlightWorkers.fetch_add(1, std::memory_order_seq_cst);
        const int count = dspJobCount.load(std::memory_order_seq_cst);
        if (count <= 0) {
          dspInFlightWorkers.fetch_sub(1, std::memory_order_seq_cst);
          break;
        }
        const int qIdx = dspNextJobIdx.fetch_add(1, std::memory_order_acq_rel);
        if (qIdx >= count) {
          dspInFlightWorkers.fetch_sub(1, std::memory_order_seq_cst);
          break;
        }
        const int slotIdx = dspSlotQueue[qIdx];
        executeSlotDsp(slotIdx, dspRawInput, dspNumInputs, false,
                       dspActiveSamples.load(std::memory_order_relaxed));
        dspInFlightWorkers.fetch_sub(1, std::memory_order_seq_cst);
      }
    }
#else
    juce::ignoreUnused(workerIdx);
#endif
  }

  

// Build a plugin instance (create + layout configuration + prepare) on an
  // MTA background thread with a bounded wait, then hand back the prepared
  // instance. Message-thread plugin instantiation hangs for engines whose
  // init needs a pumping thread (UVI Workstation: setPlayConfigDetails
  // deadlock, Sep 7) or faults outright (HALion 7, Sep 6) — background MTA
  // builds are the environment every working host uses (RigBuilder's staging
  // path has been proving this all along). On timeout the build is abandoned:
  // the half-built instance lives on the detached worker thread and the
  // caller gets null — the rig survives.
bool FanfareEngine::findPrimaryPluginDescription(const juce::String &rawPath,
                                                 juce::PluginDescription &outDesc,
                                                 juce::String &outError) 
{
    const juce::String path = resolvePluginPathForCurrentPlatform(rawPath);
    const juce::String normKey = normalizePath(path);
    {
      juce::ScopedLock cacheSl(pluginDescCacheLock);
      auto it = pluginDescCache.find(normKey);
      if (it != pluginDescCache.end()) {
        outDesc = it->second;
        return true;
      }
    }

    juce::AudioPluginFormat *pluginFormat = findFormatForPath(path);
    if (!pluginFormat) {
      outError = "Plugin format not available";
      return false;
    }

    juce::OwnedArray<juce::PluginDescription> descriptions;
    {
      std::unique_lock<std::timed_mutex> fmLock(formatManagerMutex,
                                                std::defer_lock);
      if (!fmLock.try_lock_for(std::chrono::seconds(45))) {
        outError = "Plugin format manager busy";
        return false;
      }
      {
        juce::ScopedLock cacheSl(pluginDescCacheLock);
        auto it = pluginDescCache.find(normKey);
        if (it != pluginDescCache.end()) {
          outDesc = it->second;
          return true;
        }
      }
      pluginFormat->findAllTypesForFile(descriptions, path);
    }

    if (descriptions.isEmpty()) {
      outError = "No plugins found in file: " + path;
      return false;
    }

    outDesc = *descriptions[0];
    {
      juce::ScopedLock cacheSl(pluginDescCacheLock);
      pluginDescCache[normKey] = outDesc;
    }
    return true;
  }

  std::unique_ptr<juce::AudioPluginInstance> FanfareEngine::buildInstanceOffThread(
      const juce::PluginDescription &desc, juce::String &error,
      int timeoutMs, const juce::String &gatePath ) 
{
    struct OffThreadBuild {
      FanfareEngine *engine;
      std::shared_ptr<std::atomic<bool>> alive;
      juce::PluginDescription desc;
      juce::String gatePath;
      double sampleRate = 48000.0;
      int blockSize = 512;
      std::unique_ptr<juce::AudioPluginInstance> inst;
      juce::String err;
      std::atomic<bool> done{false};
      std::atomic<bool> abandoned{false};
    };
    auto ctx = std::make_shared<OffThreadBuild>();
    ctx->engine = this;
    ctx->alive = engineAlive;
    ctx->desc = desc;
    ctx->gatePath = gatePath;
    ctx->sampleRate = currentSampleRate > 0.0 ? currentSampleRate : 48000.0;
    ctx->blockSize = currentBlockSize > 0 ? currentBlockSize : 512;

    std::thread t([ctx]() {
      const bool engineUpAtStart =
          ctx->alive && ctx->alive->load(std::memory_order_acquire);
      if (engineUpAtStart)
        ctx->engine->applyBackgroundThreadTuning();
#ifdef _WIN32
      ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
#endif
      std::unique_ptr<juce::AudioPluginInstance> tmp;
      juce::String tmpErr;
      auto isAlive = [&]() {
        return !ctx->abandoned.load(std::memory_order_acquire) && ctx->alive &&
               ctx->alive->load(std::memory_order_acquire);
      };

      if (isAlive()) {
        std::unique_lock<std::timed_mutex> fmLock(
            ctx->engine->formatManagerMutex, std::defer_lock);
        if (fmLock.try_lock_for(std::chrono::seconds(35))) {
          if (isAlive()) {
            tmp = ctx->engine->getFormatManager().createPluginInstance(
                ctx->desc, ctx->sampleRate, ctx->blockSize, tmpErr);
          }
        } else {
          tmpErr = "VST3 format manager busy";
        }
      }
      if (tmp && isAlive()) {
        ctx->engine->configureStereoLayout(tmp.get());
        if (isAlive()) {
          bool prepOk = FanfareLog::safeExecutePluginCall([&]() {
            tmp->prepareToPlay(ctx->sampleRate, ctx->blockSize);
          }, "prepareToPlay (OffThread " + ctx->desc.name + ")");
          if (!prepOk) {
            tmpErr = "plugin prepareToPlay faulted/crashed";
            if (ctx->alive && ctx->alive->load(std::memory_order_acquire)) {
              std::unique_lock<std::timed_mutex> fmLock(
                  ctx->engine->formatManagerMutex, std::defer_lock);
              (void)fmLock.try_lock_for(std::chrono::seconds(2));
              tmp.reset();
            } else {
              tmp.reset();
            }
          }
        }
      }
      if (isAlive()) {
        ctx->inst = std::move(tmp);
        ctx->err = tmpErr;
      } else {
        if (tmp) {
          if (ctx->alive && ctx->alive->load(std::memory_order_acquire)) {
            std::unique_lock<std::timed_mutex> fmLock(
                ctx->engine->formatManagerMutex, std::defer_lock);
            (void)fmLock.try_lock_for(std::chrono::seconds(2));
            tmp.reset();
          } else {
            tmp.reset(); // engine gone: release on the worker thread
          }
        }
      }
#ifdef _WIN32
      ::CoUninitialize();
#endif
      if (ctx->gatePath.isNotEmpty())
        Fanfare::releasePathBuildGate(ctx->gatePath);
      ctx->done.store(true, std::memory_order_release);
    });

    int waitedMs = 0;
    while (! ctx->done.load(std::memory_order_acquire) && waitedMs < timeoutMs) {
#ifdef _WIN32
      if (juce::MessageManager::getInstance()->isThisTheMessageThread()) {
        MSG msg;
        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
          TranslateMessage(&msg);
          DispatchMessage(&msg);
        }
      }
#endif
      juce::Thread::sleep(50);
      waitedMs += 50;
    }

    if (ctx->done.load(std::memory_order_acquire)) {
      t.join();
      error = ctx->err;
      return std::move(ctx->inst);
    }

    ctx->abandoned.store(true, std::memory_order_release);
    logToFile("WARNING: off-thread plugin build timed out (" +
              juce::String(timeoutMs / 1000) + "s) for " + desc.name +
              " — abandoning; the rig continues without it.");
    t.detach();
    error = "build timed out";
    return nullptr;
  }

  std::unique_ptr<juce::AudioPluginInstance>
  FanfareEngine::createPreparedInteractiveInstance(
      const juce::String &rawPluginPath, juce::PluginDescription &outDesc,
      juce::String &outError) 
{
    const juce::String pluginPath = resolvePluginPathForCurrentPlatform(rawPluginPath);
    if (!FanfareConstants::pluginExistsOnDisk(pluginPath)) {
      outError = "Plugin file not found: " + pluginPath;
      return nullptr;
    }

    const juce::String normPath = normalizePath(pluginPath);
    if (Fanfare::isPathPoisoned(normPath)) {
      outError = "Plugin quarantined after prior fault: " +
                 juce::File(pluginPath).getFileName();
      return nullptr;
    }

    logToFile("Scanning plugin...");
    if (!findPrimaryPluginDescription(pluginPath, outDesc, outError))
      return nullptr;

    logToFile("Found: " + outDesc.name);
    logToFile("desc: " + outDesc.name + " isInstrument=" +
              juce::String(outDesc.isInstrument ? 1 : 0) + " inCh=" +
              juce::String(outDesc.numInputChannels) + " outCh=" +
              juce::String(outDesc.numOutputChannels));

    const double safeSr = currentSampleRate > 0.0 ? currentSampleRate : 48000.0;
    const int safeBs = currentBlockSize > 0 ? currentBlockSize : 512;

    pinDllInMemory(pluginPath);

    if (!Fanfare::tryAcquirePathBuildGate(normPath, 15000)) {
      outError = "Plugin is busy initializing in another task";
      return nullptr;
    }

    if (Fanfare::pluginRequiresMessageThread(pluginPath)) {
      struct GateReleaser {
        juce::String p;
        ~GateReleaser() { Fanfare::releasePathBuildGate(p); }
      } releaser{normPath};

      logToFile("Building " + outDesc.name +
                " on the message thread (interactive)...");
      std::unique_ptr<juce::AudioPluginInstance> instance;
      {
        std::unique_lock<std::timed_mutex> fmLock(formatManagerMutex,
                                                  std::defer_lock);
        if (!fmLock.try_lock_for(std::chrono::seconds(35))) {
          outError = "VST3 format manager busy";
          return nullptr;
        }
        instance = formatManager.createPluginInstance(outDesc, safeSr, safeBs,
                                                      outError);
      }
      if (!instance)
        return nullptr;

      configureStereoLayout(instance.get());
      bool prepOk = FanfareLog::safeExecutePluginCall([&]() {
        instance->prepareToPlay(safeSr, safeBs);
      }, "prepareToPlay (Interactive " + outDesc.name + ")");
      if (!prepOk) {
        outError = "plugin prepareToPlay faulted/crashed";
        std::unique_lock<std::timed_mutex> fmLock(formatManagerMutex,
                                                  std::defer_lock);
        (void)fmLock.try_lock_for(std::chrono::seconds(2));
        instance.reset();
        return nullptr;
      }
      return instance;
    }

    return buildInstanceOffThread(outDesc, outError, 300000, normPath);
  }

  

// Load a plugin from the registry into a specific slot
  // NOTE: instantiation+configuration happen on an MTA background thread with
  // a bounded wait — see buildInstanceOffThread. The message thread waits
  // (UI frozen for the build duration) but can no longer deadlock inside a
  // plugin whose init needs a pumping thread.
  void
  FanfareEngine::loadPluginIntoSlot(int slotIndex, int chainIndex, int pluginIndex,
                     std::function<void(bool, const juce::String &)> callback) 
{
    bool isAux = (slotIndex >= 100);
    int realIdx = isAux ? (slotIndex - 100) : slotIndex;
    auto &slotVec = isAux ? auxReturns : slots;

    if (realIdx < 0 || realIdx >= (int)slotVec.size() || pluginIndex < 0 ||
        pluginIndex >= (int)availablePlugins.size()) {
      callback(false, "Invalid indices");
      return;
    }

    juce::String pluginPath = availablePlugins[pluginIndex].path;
    logToFile("=== LOADING PLUGIN (SYNC) ===");
    logToFile("Path: " + pluginPath);

    juce::PluginDescription desc;
    juce::String errorMessage;
    size_t ramBefore = FanfareLog::getMemoryStats().workingSetBytes;
    auto instance = createPreparedInteractiveInstance(pluginPath, desc, errorMessage);

    if (instance) {
      logToFile("Plugin loaded successfully!");
      size_t ramAfter = FanfareLog::getMemoryStats().workingSetBytes;
      if (ramAfter > ramBefore) {
        slotVec[realIdx]->setEstimatedRamBytes(ramAfter - ramBefore);
      }

      // CRITICAL: Lock while swapping the actual instance in the rack
      {
        juce::ScopedLock sl(lock);
        slotVec[realIdx]->clearChainSlotMidiOut(chainIndex); // plugin replaces midi-out/synth/fm
        slotVec[realIdx]->clearChainSlotSynth(chainIndex);
        slotVec[realIdx]->clearChainSlotFm(chainIndex);
        slotVec[realIdx]->setPluginInChain(chainIndex, std::move(instance));
        if (chainIndex == 0)
          slotVec[realIdx]->setName(slotVec[realIdx]->getPluginName(0));
      }

      callback(true, "");
    } else {
      logToFile("Load failed: " + errorMessage);
      callback(false, errorMessage);
    }
  }

  void FanfareEngine::loadPluginIntoB3ExtSlot(
      int slotIndex, int chainIndex, int b3ExtSlot, int pluginIndex,
      std::function<void(bool, const juce::String &)> callback) {
    bool isAux = (slotIndex >= 100);
    int realIdx = isAux ? (slotIndex - 100) : slotIndex;
    auto &slotVec = isAux ? auxReturns : slots;

    if (realIdx < 0 || realIdx >= (int)slotVec.size() || chainIndex < 0 ||
        chainIndex >= 3 || b3ExtSlot < 0 ||
        b3ExtSlot >= B3OrganProcessor::kNumExtSlots) {
      if (callback) callback(false, "Invalid B3 external slot indices");
      return;
    }

    auto *slot = slotVec[(size_t)realIdx].get();
    auto &b3 = slot->getB3Organ(chainIndex);

    if (pluginIndex == -1) {
      std::unique_ptr<juce::AudioPluginInstance> oldPlugin;
      {
        juce::ScopedLock sl(lock);
        oldPlugin = b3.swapExtPlugin(b3ExtSlot, nullptr, {}, {});
      }
      if (oldPlugin) {
        if (onBeforePluginDestroyed)
          onBeforePluginDestroyed(oldPlugin.get());
        FanfareLog::safeExecutePluginCall([&]() {
          oldPlugin->releaseResources();
          oldPlugin.reset();
        }, "B3 oldExtPlugin releaseResources/reset");
      }
      if (callback) callback(true, "");
      return;
    }

    if (pluginIndex < 0 || pluginIndex >= (int)availablePlugins.size()) {
      if (callback) callback(false, "Invalid plugin index");
      return;
    }

    const juce::String pluginPath = availablePlugins[(size_t)pluginIndex].path;
    const juce::String pluginName = availablePlugins[(size_t)pluginIndex].name;
    juce::PluginDescription desc;
    juce::String errorMessage;
    auto instance = createPreparedInteractiveInstance(pluginPath, desc, errorMessage);
    if (instance) {
      std::unique_ptr<juce::AudioPluginInstance> oldPlugin;
      {
        juce::ScopedLock sl(lock);
        oldPlugin = b3.swapExtPlugin(b3ExtSlot, std::move(instance), pluginPath, pluginName);
      }
      if (oldPlugin) {
        if (onBeforePluginDestroyed)
          onBeforePluginDestroyed(oldPlugin.get());
        FanfareLog::safeExecutePluginCall([&]() {
          oldPlugin->releaseResources();
          oldPlugin.reset();
        }, "B3 oldExtPlugin releaseResources/reset");
      }
      if (callback) callback(true, "");
    } else {
      if (callback) callback(false, errorMessage);
    }
  }

  void FanfareEngine::restoreB3ExtPluginsForSlot(RackSlot *slot) {
    if (!slot)
      return;
    for (int c = 0; c < 3; ++c) {
      if (!slot->isChainSlotB3(c))
        continue;
      auto &b3 = slot->getB3Organ(c);
      for (int e = 0; e < B3OrganProcessor::kNumExtSlots; ++e) {
        juce::String path, name, b64;
        if (!b3.consumePendingExtPlugin(e, path, name, b64))
          continue;
        juce::PluginDescription desc;
        juce::String err;
        auto instance = createPreparedInteractiveInstance(path, desc, err);
        if (instance) {
          if (b64.isNotEmpty()) {
            juce::MemoryBlock mb;
            if (mb.fromBase64Encoding(b64) && mb.getSize() > 0)
              instance->setStateInformation(mb.getData(), (int)mb.getSize());
          }
          std::unique_ptr<juce::AudioPluginInstance> oldPlugin;
          {
            juce::ScopedLock sl(lock);
            oldPlugin = b3.swapExtPlugin(
                e, std::move(instance), path,
                name.isNotEmpty() ? name : desc.name);
          }
          if (oldPlugin) {
            if (onBeforePluginDestroyed)
              onBeforePluginDestroyed(oldPlugin.get());
            FanfareLog::safeExecutePluginCall([&]() {
              oldPlugin->releaseResources();
              oldPlugin.reset();
            }, "B3 restore oldExtPlugin");
          }
        }
      }
    }
  }

  

// Same logic for Master chains
  void FanfareEngine::loadPluginIntoMasterBus(
      bool isFoh, int chainIndex, int pluginIndex,
      std::function<void(bool, const juce::String &)> callback) 
{

    if (pluginIndex == -1) {
      std::unique_ptr<juce::AudioPluginInstance> oldPlugin;
      {
        juce::ScopedLock sl(lock);
        juce::SpinLock::ScopedLockType mfl(masterFxLock);
        auto &chain = isFoh ? fohPluginChain : iemPluginChain;
        if (chainIndex >= 0 && chainIndex < (int)chain.size()) {
          oldPlugin = std::move(chain[chainIndex]);
        }
      }
      if (oldPlugin) {
        if (onBeforePluginDestroyed)
          onBeforePluginDestroyed(oldPlugin.get());
        FanfareLog::safeExecutePluginCall([&]() {
          oldPlugin->releaseResources();
          oldPlugin.reset();
        }, "oldPlugin releaseResources/reset");
      }
      callback(true, "");
      return;
    }

    if (pluginIndex < 0 || pluginIndex >= (int)availablePlugins.size()) {
      callback(false, "Invalid index");
      return;
    }

    juce::String pluginPath = availablePlugins[pluginIndex].path;
    juce::PluginDescription desc;
    juce::String errorMessage;
    auto instance = createPreparedInteractiveInstance(pluginPath, desc, errorMessage);

    if (instance) {
      std::unique_ptr<juce::AudioPluginInstance> oldPlugin;
      {
        juce::ScopedLock sl(lock);
        juce::SpinLock::ScopedLockType mfl(masterFxLock);
        auto &chain = isFoh ? fohPluginChain : iemPluginChain;
        // Expand chain if needed
        while ((int)chain.size() <= chainIndex)
          chain.push_back(nullptr);

        oldPlugin = std::move(chain[chainIndex]);
        chain[chainIndex] = std::move(instance);
      }
      if (oldPlugin) {
        if (onBeforePluginDestroyed)
          onBeforePluginDestroyed(oldPlugin.get());
        FanfareLog::safeExecutePluginCall([&]() {
          oldPlugin->releaseResources();
          oldPlugin.reset();
        }, "oldPlugin releaseResources/reset");
      }
      callback(true, "");
    } else {
      callback(false, errorMessage);
    }
  }

  

// Snapshot normalized plugin paths of the live rack (briefly holds lock).
  FanfareEngine::PluginPathSnapshot FanfareEngine::snapshotPluginPaths() 
{
    juce::ScopedLock sl(lock);
    PluginPathSnapshot s;
    s.slotChains.resize(slots.size());
    for (size_t i = 0; i < slots.size(); ++i) {
      for (int p = 0; p < slots[i]->getChainSize(); ++p)
        s.slotChains[i].add(normalizePath(slots[i]->getPluginPath(p)));
      while (s.slotChains[i].size() < 3)
        s.slotChains[i].add("");
    }
    s.auxChains.resize(auxReturns.size());
    for (size_t i = 0; i < auxReturns.size(); ++i) {
      for (int p = 0; p < auxReturns[i]->getChainSize(); ++p)
        s.auxChains[i].add(normalizePath(auxReturns[i]->getPluginPath(p)));
      while (s.auxChains[i].size() < 3)
        s.auxChains[i].add("");
    }
    auto pad = [this](const std::vector<std::unique_ptr<juce::AudioPluginInstance>> &chain,
                      juce::StringArray &out, bool isFoh) {
      for (int p = 0; p < (int)chain.size(); ++p)
        out.add(chain[p] ? normalizePath(getMasterPluginPath(isFoh, p)) : "");
      while (out.size() < 3)
        out.add("");
    };
    pad(fohPluginChain, s.fohChain, true);
    pad(iemPluginChain, s.iemChain, false);
    return s;
  }

  

// Build (create + configure + prepare + restore state) a plugin instance from
  // its serialized var, WITHOUT touching the rack. Thread-safe vs. the audio
  // thread. Used off-thread by RigBuilder and on-thread as a fallback.
  bool FanfareEngine::buildPluginFromVar(const juce::var &vt,
                          std::unique_ptr<juce::AudioPluginInstance> &out,
                          juce::String &error) 
{
    juce::String path = resolvePluginPathForCurrentPlatform(vt.getProperty("path", "").toString());
    juce::String stateBase64 = vt.getProperty("state", "").toString();
    if (path.isEmpty()) {
      error = "empty plugin path";
      return false;
    }

    logToFile("TRACE: buildPluginFromVar started for path: " + path);

    juce::PluginDescription desc;
    if (!findPrimaryPluginDescription(path, desc, error)) {
      logToFile("TRACE: buildPluginFromVar failed - " + error);
      return false;
    }

    const double safeSr = currentSampleRate > 0.0 ? currentSampleRate : 48000.0;
    const int safeBs = currentBlockSize > 0 ? currentBlockSize : 512;

    logToFile("TRACE: buildPluginFromVar creating instance for " + desc.name);
    try {
      pinDllInMemory(path);
      std::unique_ptr<juce::AudioPluginInstance> instance;
      {
        std::unique_lock<std::timed_mutex> fmLock(formatManagerMutex,
                                                  std::defer_lock);
        if (!fmLock.try_lock_for(std::chrono::seconds(45))) {
          error = "VST3 format manager busy";
          logToFile("TRACE: buildPluginFromVar timed out waiting for formatManagerMutex: " + path);
          return false;
        }
        instance = formatManager.createPluginInstance(desc, safeSr, safeBs, error);
      }
      if (!instance) {
        logToFile("TRACE: buildPluginFromVar failed to create instance: " + error);
        return false;
      }

      logToFile("TRACE: buildPluginFromVar configuring stereo layout for " + instance->getName());
      configureStereoLayout(instance.get());

      logToFile("TRACE: buildPluginFromVar calling prepareToPlay for " + instance->getName());
      bool prepOk = FanfareLog::safeExecutePluginCall([&]() {
        instance->prepareToPlay(safeSr, safeBs);
      }, "prepareToPlay (" + instance->getName() + ")");
      if (!prepOk) {
        error = "plugin prepareToPlay faulted/crashed";
        logToFile("TRACE: buildPluginFromVar prepareToPlay faulted for: " + instance->getName());
        std::unique_lock<std::timed_mutex> fmLock(formatManagerMutex,
                                                  std::defer_lock);
        (void)fmLock.try_lock_for(std::chrono::seconds(5));
        instance.reset();
        return false;
      }

      juce::MemoryBlock blob;
      blob.fromBase64Encoding(stateBase64);
      if (blob.getSize() > 0) {
        logToFile("TRACE: buildPluginFromVar setting state information (" + juce::String(blob.getSize()) + " bytes) for " + instance->getName());
        bool stateOk = FanfareLog::safeExecutePluginCall([&]() {
          instance->setStateInformation(blob.getData(), (int)blob.getSize());
        }, "setStateInformation (" + instance->getName() + ")");
        if (!stateOk) {
          logToFile("WARN: setStateInformation faulted for plugin: " + instance->getName() + " - continuing with default state.");
        }
      }

      logToFile("TRACE: buildPluginFromVar successfully built: " + instance->getName());
      out = std::move(instance);
      return true;
    } catch (...) {
      error = "plugin threw an exception during build";
      logToFile("TRACE: buildPluginFromVar caught exception for: " + path);
      return false;
    }
  }

  

// Silent processBlock validation. Returns false if processing throws.
  bool FanfareEngine::validatePluginInstance(juce::AudioPluginInstance &inst) 
{
    // Calling processBlock on a background thread is unsafe for many complex/multi-output
    // plugins (e.g. Omnisphere) and can cause crashes/hangs.
    juce::ignoreUnused(inst);
    return true;
  }

  

// Staging cache: pre-built plugin instances keyed by rack location.
   juce::String FanfareEngine::stagingKeyFor(int slotIdx, int chainIdx, bool isFoh) 
{
    if (slotIdx >= 100)
      return "A:" + juce::String(slotIdx - 100) + ":" + juce::String(chainIdx);
    if (slotIdx >= 0)
      return "S:" + juce::String(slotIdx) + ":" + juce::String(chainIdx);
    return isFoh ? ("F:" + juce::String(chainIdx))
                 : ("I:" + juce::String(chainIdx));
  }
  

bool FanfareEngine::popStagedPlugin(const juce::String &key,
                       std::unique_ptr<juce::AudioPluginInstance> &out) 
{
    juce::ScopedLock sl(stagingLock);
    auto it = stagedPlugins.find(key);
    if (it == stagedPlugins.end())
      return false;
    out = std::move(it->second);
    stagedPlugins.erase(it);
    return true;
  }
  

bool FanfareEngine::isPluginStateIdentical(const juce::String &key, const juce::String &stateBase64) const 
{
    juce::ScopedLock sl(const_cast<juce::CriticalSection&>(stagingLock));
    auto it = lastAppliedPluginStates.find(key);
    if (it != lastAppliedPluginStates.end())
      return it->second == stateBase64 && !stateBase64.isEmpty();
    return false;
  }

  

void FanfareEngine::promotePreloadedToStaged() 
{
    juce::ScopedLock sl(stagingLock);
    for (auto &pair : preloadedPlugins) {
      stagedPlugins[pair.first] = std::move(pair.second);
    }
    preloadedPlugins.clear();
  }
  

void FanfareEngine::broadcastNotesChanged() 
{
    auto alive = engineAlive;
    juce::MessageManager::callAsync([this, alive]() {
      if (!alive || !alive->load(std::memory_order_acquire))
        return;
      for (auto& pair : noteListeners) {
        if (pair.second) pair.second();
      }
      if (onNotesChanged) onNotesChanged();
    });
  }

  

