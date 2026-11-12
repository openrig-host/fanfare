#pragma once

#include "Logger.h"
#include "PluginBuildGate.h"
#include "RackSlot.h"
#include "Scene.h"
#include "RigModel.h"
#include "Version.h"
#include "RigSerializer.h"
#include "SaveHistory.h"
#include <JuceHeader.h>
#include "FanfareConstants.h"
#include <algorithm>
#include <fstream>
#include <functional>
#include <memory>
#include <thread>
#include <vector>

#ifdef _WIN32
#define NOMINMAX // Prevent Windows.h from defining min/max macros
#include <objbase.h> // CoInitializeEx/CoUninitialize — off-thread plugin builds
#include <Windows.h>
#include <intrin.h> // For _mm_pause()
#endif

#ifndef OPENRIG_PINDLL_DEFINED
#define OPENRIG_PINDLL_DEFINED
inline void pinDllInMemory(const juce::String &path) {
#if JUCE_WINDOWS
  if (path.isNotEmpty()) {
    ::LoadLibraryW(path.toWideCharPointer());
  }
#endif
}
#endif

class FanfareEngine {
public:
  // ========== KNOWN PLUGINS REGISTRY ==========
  // These are the VST3 plugins available for loading into slots.
  // Each entry is {Display Name, Full Path}
  struct PluginInfo {
    juce::String name;
    juce::String path;
  };
FanfareEngine();
~FanfareEngine();
// Call this BEFORE the engine is destroyed to release all VST3 plugins
  void releaseAllPlugins();
std::shared_ptr<std::atomic<bool>> getAliveToken() const { return engineAlive; }
  
bool isRigTransitioning() const noexcept { return isApplyingRig.load(std::memory_order_relaxed); }
  
juce::int64 getLastRigApplyEndTicks() const noexcept { return lastRigApplyEndTicks.load(std::memory_order_acquire); }
  
bool isParallelProcessingEnabled() const noexcept { return parallelProcessingEnabled.load(std::memory_order_relaxed); }
  
void setParallelProcessingEnabled(bool enabled) noexcept { parallelProcessingEnabled.store(enabled, std::memory_order_release); }

  
DWORD_PTR getShelteredAffinityMask() const noexcept { return shelteredAffinityMask.load(std::memory_order_relaxed); }
  
void setShelteredAffinityMask(DWORD_PTR mask) noexcept {
    shelteredAffinityMask.store(mask, std::memory_order_release);
    dspTuningGen.fetch_add(1, std::memory_order_acq_rel);
  }

  
void setDspWorkerAffinityMask(int workerIdx, DWORD_PTR mask) noexcept;
DWORD_PTR getBackgroundAffinityMask() const noexcept { return backgroundAffinityMask.load(std::memory_order_relaxed); }
  
void setBackgroundAffinityMask(DWORD_PTR mask) noexcept { backgroundAffinityMask.store(mask, std::memory_order_release); }

  
void setActiveDspHelperLimit(int count) noexcept {
    activeDspHelperLimit.store(juce::jlimit(0, kMaxDspHelpers, count), std::memory_order_release);
  }
  
int getActiveDspHelperLimit() const noexcept { return activeDspHelperLimit.load(std::memory_order_relaxed); }
  
bool isPreventPowerThrottlingEnabled() const noexcept { return preventPowerThrottlingEnabled.load(std::memory_order_relaxed); }

  
void setPreventPowerThrottling(bool prevent) noexcept {
    preventPowerThrottlingEnabled.store(prevent, std::memory_order_release);
    dspTuningGen.fetch_add(1, std::memory_order_acq_rel);
  }

  
static void applyThreadHighQoS(bool disableEcoQoS) noexcept;
void applyBackgroundThreadTuning() const noexcept;
int getNumSlots() const { return (int)slots.size(); }
  
RackSlot *getSlot(int index) { return slots[index].get(); }

  
int getNumAuxReturns() const { return (int)auxReturns.size(); }
  
RackSlot *getAuxReturn(int index) { return auxReturns[index].get(); }

  
void loadScene(int index);
int getNumScenes() const {
    juce::ScopedLock sl(scenesLock);
    return (int)scenes.size();
  }
  
juce::String getSceneName(int index) const {
    juce::ScopedLock sl(scenesLock);
    if (index < 0 || index >= (int)scenes.size())
      return {};
    return scenes[index].name;
  }
  
int getCurrentSceneIndex() const {
    juce::ScopedLock sl(scenesLock);
    return currentSceneIndex;
  }
  
std::vector<juce::String> getSceneNamesSnapshot(int &outCurrentIndex) const;
// Per-scene MIDI trigger accessors
  int getSceneMidiPC(int index) const {
    juce::ScopedLock sl(scenesLock);
    if (index >= 0 && index < (int)scenes.size())
      return scenes[index].midiProgramChange;
    return -1;
  }
  
int getSceneMidiChannel(int index) const {
    juce::ScopedLock sl(scenesLock);
    if (index >= 0 && index < (int)scenes.size())
      return scenes[index].midiChannel;
    return 0;
  }
  
void setSceneMidiTrigger(int index, int pc, int channel);
void clearSceneMidiTrigger(int index);
// Returns the scene index that has been assigned the given PC + channel,
  // or -1 if none found. channel 0 = match any channel.
  int findSceneForProgram(int pc, int channel) const;
void saveCurrentStateToScene(int index);
void renameScene(int index, const juce::String &newName);
void createNewScene(const juce::String &name);
void deleteScene(int index);
// Lock-free, zero-allocation slot DSP execution used by both Sequential and
  // Parallel Work-Sharing paths (eliminates juce::ThreadPool job->pool reuse races).
  void executeSlotDsp(int slotIdx, const float *const *rawInput,
                      int numInputChannels, bool seedOnly,
                      int activeSamples = -1) noexcept;
void prepareToPlay(int samplesPerBlockExpected, double sampleRate);
struct ScanResults {
    std::vector<PluginInfo> newPlugins;
    std::vector<PluginInfo> missingPlugins;
  };
std::function<void(ScanResults)> onScanFinished;
void scanForPlugins();
void applyScanResults(const std::vector<PluginInfo> &toAdd,
                        const std::vector<PluginInfo> &toRemove);
void savePluginList();
bool hasEverScannedPlugins() const { return hasSavedPluginList; }

  
void loadPluginList();
void processAudio(const float *const *inputData, int numInputs,
                    float *const *outputData, int numOutputs, int numSamples,
                    juce::MidiBuffer &midi) {
    juce::ScopedNoDenormals noDenormals;
    FanfareLog::t_isRealtimeAudioThread = true;
#ifdef _WIN32
    FanfareLog::g_audioThreadId.store((uint32_t)::GetCurrentThreadId(), std::memory_order_relaxed);
#endif

    // CRITICAL: Clear hardware output buffers unconditionally first to prevent feedback
    for (int i = 0; i < numOutputs; ++i)
      if (outputData[i])
        juce::FloatVectorOperations::clear(outputData[i], numSamples);

    // Dekker quiet-window pair (part 1): bail out BEFORE setting isInProcessAudio
    // if a rig/slot transition is active so ScopedAudioQuietWindow never starves.
    if (isApplyingRig.load(std::memory_order_seq_cst))
      return;

    isInProcessAudio.store(true, std::memory_order_seq_cst);
    struct ProcessAudioScope {
      std::atomic<bool> &flag;
      ~ProcessAudioScope() { flag.store(false, std::memory_order_seq_cst); }
    } activeAudioScope{isInProcessAudio};

    // Dekker quiet-window pair (part 2): re-check after publishing isInProcessAudio.
    if (isApplyingRig.load(std::memory_order_seq_cst))
      return;

#if FANFARE_ENABLE_STRESS_TEST
    // Inject stress test MIDI if active (dev/test builds only)
    if (stressTestActive.load()) {
        double sr = currentSampleRate;
        if (sr > 0.0) {
            double samplesPerTrigger = 2048.0;
            stressTestPhase += numSamples;
            if (stressTestPhase >= samplesPerTrigger) {
                stressTestPhase = std::fmod(stressTestPhase, samplesPerTrigger);
                for (int c = 0; c < FanfareConstants::kNumSlots; ++c) {
                    if (stressTestLastNote[c] >= 0) {
                        midi.addEvent(juce::MidiMessage::noteOff(1, stressTestLastNote[c]), 0);
                        stressTestLastNote[c] = -1;
                    }
                }
                static std::mt19937 stressRng(1337);
                std::uniform_int_distribution<int> noteDist(48, 84);
                std::uniform_int_distribution<int> velDist(60, 127);
                for (int c = 0; c < 6; ++c) {
                    int note = noteDist(stressRng);
                    int vel = velDist(stressRng);
                    midi.addEvent(juce::MidiMessage::noteOn(1, note, (juce::uint8)vel), 0);
                    stressTestLastNote[c] = note;
                }
            }
        }
    }
#endif

    const bool isSwitching = false;

    fohBus.clear(0, numSamples);
    iemBus.clear(0, numSamples);
    aux1Bus.clear(0, numSamples);
    aux2Bus.clear(0, numSamples);

    if (slotMidiBuffers.size() != slots.size()) {
      // Pre-allocated in prepareToPlay(). A mismatch means prepareToPlay()
      // wasn't called for this rack — never allocate on the audio thread.
      audioUnderrunFlag.store(true);
      return; // hardware outputs + buses already cleared above
    }

    for (auto &smb : slotMidiBuffers)
      smb.clear();

    // Panic Button Trigger
    if (panicTriggered.exchange(false)) {
      for (auto &slot : slots) {
        if (slot) {
          if (slot->getArpeggiator().enabled.load(std::memory_order_relaxed))
            slot->getArpeggiator().reset();
          if (slot->getHarmonizer().enabled.load(std::memory_order_relaxed))
            slot->getHarmonizer().reset();
          if (slot->getSampler().enabled.load(std::memory_order_relaxed))
            slot->getSampler().reset();
          for (int c = 0; c < 3; ++c) {
            if (slot->isChainSlotSynth(c) ||
                (c == 0 && slot->getSynth(0).enabled.load(std::memory_order_relaxed))) {
              slot->getSynth(c).allNotesOff();
            }
            if (slot->isChainSlotFm(c) ||
                (c == 0 && slot->getFmSynth(0).enabled.load(std::memory_order_relaxed))) {
              slot->getFmSynth(c).panicNow();
            }
          }
        }
      }
      for (size_t i = 0; i < slots.size() && i < slotMidiBuffers.size(); ++i) {
        if (slots[i] && slots[i]->hasActivePlugins()) {
          auto &smb = slotMidiBuffers[i];
          for (int ch = 1; ch <= 16; ++ch) {
            smb.addEvent(juce::MidiMessage::allNotesOff(ch), 0);
            smb.addEvent(juce::MidiMessage::allSoundOff(ch), 0);
            smb.addEvent(juce::MidiMessage::controllerEvent(ch, 64, 0), 0); // Sustain Off
            smb.addEvent(juce::MidiMessage::allControllersOff(ch), 0);
          }
        }
      }
    }

    int globalChan = defaultMidiChannel.load();
    // Per-slot MIDI channel routing. Replaces the legacy hard "channel 1 only"
    // filter. effective channel = per-slot override (>=0) else global default.
    // 0 means Omni. A message routes to a slot when either side is Omni or the
    // channels match. Default (global=1) preserves the previous behaviour.
    auto slotAcceptsChannel = [this, globalChan](size_t i, int msgChannel) {
      int overrideCh = slots[i]->getMidiChannelOverride();
      if (overrideCh == -2)
        return false;
      int effective = (overrideCh >= 0) ? overrideCh : globalChan;
      if (effective == 0)
        return true; // slot is Omni
      if (msgChannel == 0)
        return true; // message is Omni
      return msgChannel == effective;
    };

    if (!isSwitching) {
      for (const auto metadata : midi) {
        auto msg = metadata.getMessage();
        int pos = metadata.samplePosition;

        // NEVER pass program change (patch change) messages
        if (msg.isProgramChange())
          continue;

        int msgChannel = msg.getChannel();

        // Special routing: MIDI messages -> only to slots
        if (msg.isNoteOn() || msg.isNoteOff()) {
          int noteNum = msg.getNoteNumber();
          // Apply per-slot channel + note range filtering
          for (size_t i = 0; i < slots.size(); ++i) {
            if (slotAcceptsChannel(i, msgChannel) &&
                slots[i]->isNoteInRange(noteNum)) {
              int transposeSemis = juce::jlimit(-48, 48, slots[i]->getTransposeSemis());
              if (transposeSemis != 0) {
                int transposed = juce::jlimit(0, 127, noteNum + transposeSemis);
                auto transposedMsg = msg.isNoteOn()
                    ? juce::MidiMessage::noteOn(msg.getChannel(), transposed,
                                                msg.getVelocity())
                    : juce::MidiMessage::noteOff(msg.getChannel(), transposed,
                                                 msg.getVelocity());
                slotMidiBuffers[i].addEvent(transposedMsg, pos);
              } else {
                slotMidiBuffers[i].addEvent(msg, pos);
              }
            }
          }
        } else if (msg.isAftertouch() || msg.isPitchWheel()) {
          // Aftertouch and pitch wheel go to channel-matched slots
          for (size_t i = 0; i < slots.size(); ++i) {
            if (slotAcceptsChannel(i, msgChannel))
              slotMidiBuffers[i].addEvent(msg, pos);
          }
        } else if (msg.isController()) {
          int ccNum = msg.getControllerNumber();

          for (size_t i = 0; i < slots.size(); ++i) {
            if (slotAcceptsChannel(i, msgChannel) &&
                slots[i]->isCCAllowed(ccNum)) {
              slotMidiBuffers[i].addEvent(msg, pos);
            }
          }
        }
      }
    }

    // Ensure scratch buffers are sized correctly. These are pre-allocated in
    // prepareToPlay(); on a shape/block-size mismatch we bail to silence rather
    // than allocate (priority inversion / dropout on the audio thread).
    size_t totalBuffers = slots.size() + auxReturns.size();
    if (scratchBuffers.size() < totalBuffers) {
      audioUnderrunFlag.store(true);
      return;
    }

    for (size_t i = 0; i < totalBuffers; ++i) {
      if (scratchBuffers[i].getNumChannels() < 2 ||
          scratchBuffers[i].getNumSamples() < numSamples) {
        audioUnderrunFlag.store(true);
        return; // block-size mismatch; never reallocate on the audio thread
      }
      const int chToClear = juce::jmin(2, scratchBuffers[i].getNumChannels());
      for (int ch = 0; ch < chToClear; ++ch)
        scratchBuffers[i].clear(ch, 0, numSamples);
    }

    // Precompute subgroup target flags in O(N) on stack
    const int numActiveSlots = juce::jmin((int)slots.size(), FanfareConstants::kNumSlots);
    bool isSubgroupTgt[FanfareConstants::kNumSlots] = {};
    for (int i = 0; i < numActiveSlots; ++i) {
      if (slots[i]->isSubgroupTarget()) {
        int tgt = slots[i]->getOutputTarget();
        if (tgt >= 0 && tgt < numActiveSlots && tgt > i)
          isSubgroupTgt[tgt] = true;
      }
    }

    int numDspCandidates = 0;
    for (int i = 0; i < numActiveSlots; ++i) {
      if (!isSubgroupTgt[i] && slots[i]->hasActivePlugins() && !slots[i]->isBypassed())
        ++numDspCandidates;
    }

#ifdef _WIN32
    const int maxHelpers = activeDspHelperLimit.load(std::memory_order_relaxed);
#else
    const int maxHelpers = 0;
#endif

    // Guard BOTH sequential and parallel paths if an earlier block wedged a DSP helper:
    // never touch scratchBuffers or invoke processBlock concurrently on an in-flight VST.
    if (engineWedged.load(std::memory_order_acquire)) {
      if (dspInFlightWorkers.load(std::memory_order_acquire) != 0) {
        audioUnderrunFlag.store(true, std::memory_order_relaxed);
        aux1Bus.clear(0, numSamples);
        aux2Bus.clear(0, numSamples);
        return;
      }
      engineWedged.store(false, std::memory_order_release);
    }

    // Process sequentially inline directly on the audio thread if parallel is disabled,
    // no helper cores are assigned, or <= 1 non-target slot has active plugins.
    if (!parallelProcessingEnabled.load(std::memory_order_relaxed) ||
        maxHelpers <= 0 || numDspCandidates <= 1) {
      aux1Bus.clear(0, numSamples);
      aux2Bus.clear(0, numSamples);
      for (int i = 0; i < numActiveSlots; ++i) {
        executeSlotDsp(i, inputData, numInputs, false, numSamples);

        int target = slots[i]->getOutputTarget();
        if (slots[i]->isSubgroupTarget() && target < numActiveSlots && target > i) {
          slots[i]->sumToSubgroup(scratchBuffers[i], scratchBuffers[target], aux1Bus, aux2Bus, numSamples);
          slots[i]->sumToIemBus(scratchBuffers[i], iemBus, numSamples);
        } else if (isSubgroupTgt[i]) {
          slots[i]->sumToFohBusOnly(scratchBuffers[i], fohBus, aux1Bus, aux2Bus, numSamples);
        } else {
          slots[i]->sumToBuses(scratchBuffers[i], fohBus, iemBus, aux1Bus, aux2Bus, numSamples);
        }
      }
    } else {
      // Lock-Free Work-Sharing Parallel Engine:
      // Persistent Priority-15 DSP helpers + the Priority-24 ASIO Audio Thread
      // pull active plugin slots together via atomic fetch_add(1). Zero locks,
      // zero allocations, zero juce::ThreadPool job->pool reuse races, and if a
      // helper wake is ever delayed by Windows, the Priority-24 ASIO thread
      // renders the remaining slots itself without stalling!
      int numDspJobs = 0;
      for (int i = 0; i < numActiveSlots; ++i) {
        const bool isTgt = isSubgroupTgt[i];
        const bool hasDsp = slots[i]->hasActivePlugins() && !slots[i]->isBypassed();
        if (hasDsp && !isTgt) {
          dspSlotQueue[numDspJobs++] = i;
        } else {
          // Seed subgroup targets (hardware input only) and process idle/input-only slots inline
          executeSlotDsp(i, inputData, numInputs, isTgt, numSamples);
        }
      }

      // Publish work queue (`dspJobCount` transitions 0 -> numDspJobs as the seq_cst gate)
      dspRawInput = inputData;
      dspNumInputs = numInputs;
      dspActiveSamples.store(numSamples, std::memory_order_relaxed);
      dspNextJobIdx.store(0, std::memory_order_release);
      dspJobCount.store(numDspJobs, std::memory_order_seq_cst);

#ifdef _WIN32
      const int helpersToWake = juce::jmin(maxHelpers, juce::jmin(kMaxDspHelpers, numDspJobs - 1));
      for (int h = 0; h < helpersToWake; ++h) {
        if (dspWorkerWakeEvents[h] != nullptr)
          SetEvent(dspWorkerWakeEvents[h]);
      }
#endif

      // Audio Thread acts as Worker #0 at MMCSS Pro Audio Priority 24
      for (;;) {
        const int qIdx = dspNextJobIdx.fetch_add(1, std::memory_order_acq_rel);
        if (qIdx >= numDspJobs)
          break;
        executeSlotDsp(dspSlotQueue[qIdx], inputData, numInputs, false, numSamples);
      }

      // Close queue gate before checking in-flight helper count (Dekker seq_cst pair)
      dspJobCount.store(0, std::memory_order_seq_cst);

      bool timedOut = false;
      if (dspInFlightWorkers.load(std::memory_order_seq_cst) != 0) {
        const double sr = (currentSampleRate > 0.0) ? currentSampleRate : 48000.0;
        const double expectedBlockSec = (double)juce::jmax(32, numSamples) / sr;
        const double maxWaitSec = juce::jlimit(0.012, 0.030, expectedBlockSec * 4.5);
        const juce::int64 deadlineTicks =
            juce::Time::getHighResolutionTicks() +
            juce::Time::secondsToHighResolutionTicks(maxWaitSec);
        long long spinCount = 0;
        while (dspInFlightWorkers.load(std::memory_order_acquire) != 0) {
          ++spinCount;
          if ((spinCount & 0x3F) == 0 &&
              juce::Time::getHighResolutionTicks() >= deadlineTicks) {
            timedOut = true;
            break;
          }
#if defined(_MSC_VER) || defined(__INTEL_COMPILER)
          _mm_pause();
#elif defined(__arm__) || defined(__aarch64__)
          __yield();
#endif
          if ((spinCount & 0x3FF) == 0) {
#ifdef _WIN32
            SwitchToThread();
#else
            std::this_thread::yield();
#endif
          }
        }
      }

      if (timedOut) {
        engineWedged.store(true, std::memory_order_release);
        audioUnderrunFlag.store(true, std::memory_order_relaxed);
        int timeouts = consecutiveParallelTimeouts.fetch_add(1, std::memory_order_relaxed) + 1;
        if (timeouts >= 3) {
          parallelProcessingEnabled.store(false, std::memory_order_release);
          recordFlightEvent(-1, "WARN: 3 consecutive parallel DSP timeouts -> fallback to sequential mode");
        } else {
          recordFlightEvent(-1, "WARN: Parallel DSP worker timeout");
        }
        fohBus.clear(0, numSamples);
        iemBus.clear(0, numSamples);
        aux1Bus.clear(0, numSamples);
        aux2Bus.clear(0, numSamples);
        return;
      } else {
        consecutiveParallelTimeouts.store(0, std::memory_order_relaxed);
        // Step 1: Accumulate subgroup sends and IEM mixes
        aux1Bus.clear(0, numSamples);
        aux2Bus.clear(0, numSamples);
        for (int i = 0; i < numActiveSlots; ++i) {
          int target = slots[i]->getOutputTarget();
          if (slots[i]->isSubgroupTarget() && target < numActiveSlots && target > i) {
            slots[i]->sumToSubgroup(scratchBuffers[i], scratchBuffers[target], aux1Bus, aux2Bus, numSamples);
            slots[i]->sumToIemBus(scratchBuffers[i], iemBus, numSamples);
          }
        }

        // Step 2: Process DSP chain for subgroup target slots on accumulated subgroup audio
        for (int i = 0; i < numActiveSlots; ++i) {
          if (isSubgroupTgt[i]) {
            juce::AudioBuffer<float> subView(
                scratchBuffers[i].getArrayOfWritePointers(),
                scratchBuffers[i].getNumChannels(), numSamples);
            slots[i]->processBlock(subView, slotMidiBuffers[i]);
          }
        }

        // Step 3: Sum to buses (subgroup targets sum to FOH only to avoid IEM double-feeding)
        for (int i = 0; i < numActiveSlots; ++i) {
          int target = slots[i]->getOutputTarget();
          if (slots[i]->isSubgroupTarget() && target < numActiveSlots && target > i)
            continue;
          if (isSubgroupTgt[i]) {
            slots[i]->sumToFohBusOnly(scratchBuffers[i], fohBus, aux1Bus, aux2Bus, numSamples);
          } else {
            slots[i]->sumToBuses(scratchBuffers[i], fohBus, iemBus, aux1Bus, aux2Bus, numSamples);
          }
        }
      }
    }

    // --- Process Aux Returns ---
    // Aux returns should NOT feed back into the aux buses (feedback loop)
    // Use pre-allocated dummy buffers (cleared, not re-constructed)
    dummyAux1.clear(0, numSamples);
    dummyAux2.clear(0, numSamples);

    for (int i = 0; i < (int)auxReturns.size(); ++i) {
      auto &scratch = scratchBuffers[slots.size() + i];
      const int chToClear = juce::jmin(2, scratch.getNumChannels());
      for (int ch = 0; ch < chToClear; ++ch)
        scratch.clear(ch, 0, numSamples);
      auto &bus = (i == 0) ? aux1Bus : aux2Bus;

      // Copy aux bus to return slot input
      for (int ch = 0; ch < 2; ++ch)
        scratch.copyFrom(ch, 0, bus, ch, 0, numSamples);

      juce::AudioBuffer<float> auxView(scratch.getArrayOfWritePointers(),
                                       scratch.getNumChannels(), numSamples);
      auxReturns[i]->processBlock(auxView, emptyMidiBuf);

      // Sum to main output buses only - NOT back to aux buses
      auxReturns[i]->sumToBuses(scratch, fohBus, iemBus, dummyAux1, dummyAux2, numSamples);
    }

    // --- Forward per-slot MIDI OUT to hardware devices ---
    // Each chain slot flagged as MIDI OUT captured its filtered/routed MIDI in
    // processBlock. Send it now (after the fork-join completes). The try-lock
    // means a device being opened on the message thread can never block (or
    // corrupt) the audio thread; on the rare miss we simply skip this block.
    {
      juce::SpinLock::ScopedTryLockType mol(midiOutputLock);
      if (mol.isLocked() && !midiOutputs.empty()) {
        for (int i = 0; i < (int)slots.size(); ++i) {
          auto *s = slots[i].get();
          for (int c = 0; c < 3; ++c) {
            if (!s->isChainSlotMidiOut(c))
              continue;
            const auto &buf = s->getMidiOutChainBuffer(c);
            if (buf.isEmpty())
              continue;
            int devIdx = s->getChainSlotMidiOutDeviceIndex(c);
            if (devIdx >= 0 && devIdx < (int)midiOutputs.size() && midiOutputs[devIdx])
              midiOutputs[devIdx]->sendBlockOfMessagesNow(buf);
          }
        }
      }
    }

    // --- Master FX Processing ---
    // Apply Master Fader Levels BEFORE FX (or typically after sums, but before
    // output)
    fohBus.applyGain(0, numSamples, fohMasterLevel);
    iemBus.applyGain(0, numSamples, iemMasterLevel);

    {
      juce::SpinLock::ScopedTryLockType mfl(masterFxLock);
      if (mfl.isLocked()) {
        juce::AudioBuffer<float> fohView(fohBus.getArrayOfWritePointers(),
                                         fohBus.getNumChannels(), numSamples);
        for (size_t idx = 0; idx < fohPluginChain.size(); ++idx) {
          auto &plugin = fohPluginChain[idx];
          if (plugin && (idx >= 3 || !fohFxBypassed[idx].load(std::memory_order_relaxed))) {
            bool ok = FanfareLog::safeExecutePluginCall([&]() {
              plugin->processBlock(fohView, emptyMidiBuf);
            }, "fohBus processBlock", -1);
            if (!ok) {
              fohView.clear();
              if (idx < 3) {
                int errs = fohFxFaultCount[idx].fetch_add(1, std::memory_order_relaxed) + 1;
                if (errs >= FanfareConstants::kMaxPluginExceptionsBeforeBypass) {
                  fohFxBypassed[idx].store(true, std::memory_order_relaxed);
                  recordFlightEvent(-1, "ERROR: FOH Master FX faulted max consecutive times -> AUTO-BYPASSED");
                }
              }
            } else if (idx < 3) {
              fohFxFaultCount[idx].store(0, std::memory_order_relaxed);
            }
          }
        }
        juce::AudioBuffer<float> iemView(iemBus.getArrayOfWritePointers(),
                                         iemBus.getNumChannels(), numSamples);
        for (size_t idx = 0; idx < iemPluginChain.size(); ++idx) {
          auto &plugin = iemPluginChain[idx];
          if (plugin && (idx >= 3 || !iemFxBypassed[idx].load(std::memory_order_relaxed))) {
            bool ok = FanfareLog::safeExecutePluginCall([&]() {
              plugin->processBlock(iemView, emptyMidiBuf);
            }, "iemBus processBlock", -1);
            if (!ok) {
              iemView.clear();
              if (idx < 3) {
                int errs = iemFxFaultCount[idx].fetch_add(1, std::memory_order_relaxed) + 1;
                if (errs >= FanfareConstants::kMaxPluginExceptionsBeforeBypass) {
                  iemFxBypassed[idx].store(true, std::memory_order_relaxed);
                  recordFlightEvent(-1, "ERROR: IEM Master FX faulted max consecutive times -> AUTO-BYPASSED");
                }
              }
            } else if (idx < 3) {
              iemFxFaultCount[idx].store(0, std::memory_order_relaxed);
            }
          }
        }
      }
    }

    // --- Post-load mute + fade-in ---
    // Blue 3 and similar plugins dump internal buffer content (Lesie tails,
    // tonewheel state) for several seconds after state restoration. We hard-
    // mute for 1.5s to let those flush, then fade in over 0.5s.
    {
      int fadeRem = postLoadFadeRemaining.load();
      if (fadeRem > 0) {
        int fadeTotal = juce::jmax(1, postLoadFadeTotal.load());
        int fadeStart = fadeTotal / 4; // last 25% = fade-in phase

        if (fadeRem > fadeStart) {
          // Hard mute phase
          for (int ch = 0; ch < 2; ++ch) {
            if (ch < fohBus.getNumChannels())
              fohBus.clear(ch, 0, numSamples);
            if (ch < iemBus.getNumChannels())
              iemBus.clear(ch, 0, numSamples);
          }
        } else {
          // Linear fade-in phase (0 -> 1 over fadeStart samples)
          float startGain = 1.0f - (float)fadeRem / (float)fadeStart;
          int samplesNow = juce::jmin(numSamples, fadeRem);
          float endGain =
              1.0f - (float)(fadeRem - samplesNow) / (float)fadeStart;
          for (int ch = 0; ch < 2; ++ch) {
            if (ch < fohBus.getNumChannels())
              fohBus.applyGainRamp(ch, 0, samplesNow, startGain, endGain);
            if (ch < iemBus.getNumChannels())
              iemBus.applyGainRamp(ch, 0, samplesNow, startGain, endGain);
            if (samplesNow < numSamples) {
              if (ch < fohBus.getNumChannels())
                fohBus.applyGain(ch, samplesNow, numSamples - samplesNow,
                                 endGain);
              if (ch < iemBus.getNumChannels())
                iemBus.applyGain(ch, samplesNow, numSamples - samplesNow,
                                 endGain);
            }
          }
        }
        postLoadFadeRemaining.store(juce::jmax(0, fadeRem - numSamples));
      }
    }

    // --- Pre-DAC Hearing & Hardware Protection (per-sample NaN / Inf hold + 0 dBFS soft-knee brickwall limiter) ---
    auto sanitizeAndPeak = [](float *data, int n, int &nanHold, bool softKneeIem = false) noexcept -> float {
      if (data == nullptr || n <= 0)
        return 0.0f;
      float peak = 0.0f;
      for (int s = 0; s < n; ++s) {
        float v = data[s];
        if (!std::isfinite(v)) {
          nanHold = 64;
          v = 0.0f;
        } else if (nanHold > 0) {
          --nanHold;
          v = 0.0f;
        } else if (softKneeIem) {
          // Soft-knee saturation above 0.85 (-1.4 dBFS) converging cleanly to +/-1.0f (0 dBFS) for IEM hearing safety
          const float avRaw = std::abs(v);
          if (avRaw > 0.85f) {
            const float excess = avRaw - 0.85f;
            const float compressed = 0.85f + 0.15f * (excess / (0.15f + excess));
            v = (v >= 0.0f) ? compressed : -compressed;
          }
          v = juce::jlimit(-1.0f, 1.0f, v);
        } else {
          // FOH / direct hardware out brickwall ceiling at +/-1.0f (0 dBFS)
          v = juce::jlimit(-1.0f, 1.0f, v);
        }
        data[s] = v;
        const float av = std::abs(v);
        if (av > peak)
          peak = av;
      }
      return peak;
    };

    float fL = sanitizeAndPeak(fohBus.getWritePointer(0), numSamples, fohNanHold[0], false);
    float fR = (fohBus.getNumChannels() > 1)
                   ? sanitizeAndPeak(fohBus.getWritePointer(1), numSamples, fohNanHold[1], false)
                   : fL;
    float iL = sanitizeAndPeak(iemBus.getWritePointer(0), numSamples, iemNanHold[0], true);
    float iR = (iemBus.getNumChannels() > 1)
                   ? sanitizeAndPeak(iemBus.getWritePointer(1), numSamples, iemNanHold[1], true)
                   : iL;

    // --- Copy Buses to Hardware Outputs ---
    // Snapshot the atomic offsets ONCE per block: the setters can write from the
    // message thread, so reading them several times (bounds-check then index)
    // would be a check-then-use race that could index outputData out of bounds.
    const int fohOff = fohOutputOffset.load(std::memory_order_acquire);
    const int iemOff = iemOutputOffset.load(std::memory_order_acquire);

    if (!isSwitching) {
      // Route FOH to configured outputs (default: 1+2)
      if (fohOff >= 0 && fohOff < numOutputs && outputData[fohOff] != nullptr) {
        juce::FloatVectorOperations::copy(outputData[fohOff],
                                          fohBus.getReadPointer(0), numSamples);
        if (fohOff + 1 < numOutputs && outputData[fohOff + 1] != nullptr)
          juce::FloatVectorOperations::copy(outputData[fohOff + 1],
                                            fohBus.getReadPointer(1), numSamples);
      }

      // Route IEM to configured outputs (default: 3+4)
      if (iemOff >= 0 && iemOff < numOutputs && outputData[iemOff] != nullptr) {
        juce::FloatVectorOperations::copy(outputData[iemOff],
                                          iemBus.getReadPointer(0), numSamples);
        if (iemOff + 1 < numOutputs && outputData[iemOff + 1] != nullptr)
          juce::FloatVectorOperations::copy(outputData[iemOff + 1],
                                            iemBus.getReadPointer(1), numSamples);
      }

      // Route Direct Hardware Outputs from individual slots
      for (size_t i = 0; i < slots.size(); ++i) {
        if (!slots[i]->isBypassed() && slots[i]->isDirectHardwareOut()) {
          int dirCh = slots[i]->getDirectHardwareOutChannel();
          if (dirCh >= 0 && dirCh < numOutputs && outputData[dirCh] != nullptr) {
            float fohBase = slots[i]->getFohLevel();
            if (slots[i]->isDirectHardwareOutStereo()) {
              if (scratchBuffers[i].getNumChannels() > 0)
                juce::FloatVectorOperations::addWithMultiply(
                    outputData[dirCh], scratchBuffers[i].getReadPointer(0),
                    fohBase, numSamples);
              if (dirCh + 1 < numOutputs && outputData[dirCh + 1] != nullptr &&
                  scratchBuffers[i].getNumChannels() > 1) {
                juce::FloatVectorOperations::addWithMultiply(
                    outputData[dirCh + 1], scratchBuffers[i].getReadPointer(1),
                    fohBase, numSamples);
                (void)sanitizeAndPeak(outputData[dirCh + 1], numSamples, directOutNanHold, false);
              }
            } else {
              // Mono direct out: sum L + R * 0.5f if 2 channels, or L if 1 channel
              if (scratchBuffers[i].getNumChannels() >= 2) {
                const float *l = scratchBuffers[i].getReadPointer(0);
                const float *r = scratchBuffers[i].getReadPointer(1);
                float *dst = outputData[dirCh];
                float gain = fohBase * 0.5f;
                for (int s = 0; s < numSamples; ++s) {
                  dst[s] += (l[s] + r[s]) * gain;
                }
              } else if (scratchBuffers[i].getNumChannels() == 1) {
                juce::FloatVectorOperations::addWithMultiply(
                    outputData[dirCh], scratchBuffers[i].getReadPointer(0),
                    fohBase, numSamples);
              }
            }
            (void)sanitizeAndPeak(outputData[dirCh], numSamples, directOutNanHold, false);
          }
        }
      }
    }

    fohPeakL.store(fL);
    fohPeakR.store(fR);
    iemPeakL.store(iL);
    iemPeakR.store(iR);
  }

  
// --- Master FX Management ---
  void
  addPluginToMasterChain(bool isFoh,
                         std::unique_ptr<juce::AudioPluginInstance> plugin);
// --- Persistence (JSON / var) ---
  juce::String exportRigToJson();
void importRigFromJson(const juce::String &json) {
    auto rig = juce::JSON::parse(json);
    applyRig(rig);
  }

  
// Apply a parsed rig object to the live rack under the callback lock. This
  // is the shared implementation used by both the synchronous import path and
  // the async RigTransitioner (which has already pre-built plugins into the
  // staging cache, so this call stays fast).
  // Dekker-synchronized quiet window: pauses processAudio cleanly (which outputs
  // hardware silence and returns before touching engine buses/plugins) and waits
  // unbounded until any in-flight real-time audio block + DSP helpers complete.
  struct ScopedAudioQuietWindow {
    FanfareEngine &eng;
    bool owned = false;
    juce::int64 startTicks = 0;
    explicit ScopedAudioQuietWindow(FanfareEngine &e) : eng(e) {
      if (!eng.isApplyingRig.exchange(true, std::memory_order_seq_cst)) {
        owned = true;
        startTicks = juce::Time::getHighResolutionTicks();
        while (eng.isInProcessAudio.load(std::memory_order_seq_cst) ||
               eng.dspInFlightWorkers.load(std::memory_order_seq_cst) != 0) {
          juce::Thread::sleep(1);
        }
      }
    }
    ~ScopedAudioQuietWindow() {
      if (owned) {
        const juce::int64 endTicks = juce::Time::getHighResolutionTicks();
        eng.lastRigApplyEndTicks.store(endTicks, std::memory_order_release);
        eng.isApplyingRig.store(false, std::memory_order_seq_cst);
        const double elapsedMs =
            juce::Time::highResolutionTicksToSeconds(endTicks - startTicks) * 1000.0;
        eng.lastQuietWindowMs.store((float)elapsedMs, std::memory_order_relaxed);
        recordFlightEvent(-1, "TRACE: ScopedAudioQuietWindow exited");
      }
    }
  };
  float getLastQuietWindowMs() const noexcept {
    return lastQuietWindowMs.load(std::memory_order_relaxed);
  }
  std::function<void(juce::AudioPluginInstance *)> onBeforePluginDestroyed;

  void enqueueReapPlugin(std::unique_ptr<juce::AudioPluginInstance> p) {
    if (p) {
      if (onBeforePluginDestroyed)
        onBeforePluginDestroyed(p.get());
      pendingReapPlugins.push_back(std::move(p));
    }
  }
  void flushReapQueue() {
    std::vector<std::unique_ptr<juce::AudioPluginInstance>> toReap;
    toReap.swap(pendingReapPlugins);
    for (auto &p : toReap) {
      if (p) {
        if (onBeforePluginDestroyed)
          onBeforePluginDestroyed(p.get());
        FanfareLog::safeExecutePluginCall([&]() {
          p->releaseResources();
          p.reset();
        }, "flushReapQueue releaseResources/reset");
      }
    }
  }
void applyRig(const juce::var &rig);
void performLegacyMigration();
Fanfare::Song getSongRepresentation(const juce::String& songName = "Current Rig");
void importRigFromSong(const Fanfare::Song& song) {
    juce::String json = Fanfare::RigSerializer::serializeSong(song);
    importRigFromJson(json);
  }

  
Fanfare::SongSlot exportSlot(int slotIdx);
void importSlot(int slotIdx, const Fanfare::SongSlot& songSlot);
// Apply a SongSlot preset directly (e.g. from drag-and-drop). Thread-safe:
  // takes the engine lock.
  void loadSlotPreset(int slotIdx, const Fanfare::SongSlot &songSlot) {
    importSlot(slotIdx, songSlot);
  }

  
void saveStripToFile(int slotIdx, const juce::File& file);
bool loadStripFromFile(int slotIdx, const juce::File& file);
void saveRigToFile() {
    juce::File f = FanfareConstants::getAppDirectory().getChildFile("FanfareFullRig.json");
    Fanfare::RigSerializer::save(f, exportRigToJson());
  }

  
void loadRigFromFile();
juce::var getPluginVar(juce::AudioPluginInstance *plugin);
juce::String normalizePath(const juce::String &path);
juce::var songSlotToPluginVar(const Fanfare::PluginState& ps);
juce::String getMasterPluginPath(bool isFoh, int chainIndex) const;
void loadPluginFromVarSmart(int slotIdx, int chainIdx, const juce::var &vt,
                              bool isFohOrChannel);
bool loadPluginFromVar(int slotIdx, int chainIdx, const juce::var &vt,
                         bool isFohOrChannel);
juce::AudioPluginFormat *findVst3Format();
void configureStereoLayout(juce::AudioProcessor *processor);
public:
  std::vector<std::unique_ptr<RackSlot>>& getSlots() { return slots; }
  
const std::vector<std::unique_ptr<RackSlot>>& getSlots() const { return slots; }
  
std::vector<std::unique_ptr<RackSlot>>& getAuxReturns() { return auxReturns; }
  
const std::vector<std::unique_ptr<RackSlot>>& getAuxReturns() const { return auxReturns; }
  
std::vector<std::unique_ptr<juce::AudioPluginInstance>>& getFohPluginChain() { return fohPluginChain; }
  
std::vector<std::unique_ptr<juce::AudioPluginInstance>>& getIemPluginChain() { return iemPluginChain; }
  
juce::AudioPluginFormatManager& getFormatManager() { return formatManager; }
  
double getCurrentSampleRate() const { return currentSampleRate; }
  
int getCurrentBlockSize() const { return currentBlockSize; }
  
std::vector<::Scene>& getScenes() { return scenes; }
  
void setCurrentSceneIndex(int index) { currentSceneIndex = index; }

  
juce::String getMasterPluginName(bool isFoh, int chainIndex) const;
juce::AudioPluginInstance *getMasterPluginInstance(bool isFoh,
                                                     int chainIndex) const;
void clearMasterChains();
void clearAllRacks();
bool isCurrentRigFreshBuild() const { return currentRigFreshBuild.load(); }
  
void setCurrentRigFreshBuild(bool fresh) { currentRigFreshBuild.store(fresh); }

  
float getFohPeakL() const { return fohPeakL; }
  
float getFohPeakR() const { return fohPeakR; }
  
float getIemPeakL() const { return iemPeakL; }
  
float getIemPeakR() const { return iemPeakR; }

  
// Returns true if the last audio block bailed to silence (e.g. a wedged
  // worker or an unprepared rack) and resets the flag. Poll from a UI timer.
  bool consumeAudioUnderrun() { return audioUnderrunFlag.exchange(false); }

  
void setFohMasterLevel(float level) { fohMasterLevel = level; }
  
void setIemMasterLevel(float level) { iemMasterLevel = level; }
  
float getFohMasterLevel() const { return fohMasterLevel.load(); }
  
float getIemMasterLevel() const { return iemMasterLevel.load(); }

  
// Global default MIDI channel for routing (1..16, or 0 = Omni).
  void setDefaultMidiChannel(int channel) {
    defaultMidiChannel.store(juce::jlimit(0, 16, channel));
  }
  
int getDefaultMidiChannel() const { return defaultMidiChannel.load(); }

  
// --- MIDI OUT device management ---
  // Returns (name, identifier) pairs for every available MIDI output device.
  juce::Array<std::pair<juce::String, juce::String>> getAvailableMidiOutputs() const;
// Open (or reuse) a MIDI output device by identifier. Returns a stable index
  // into midiOutputs, or -1 on failure. Message-thread only.
  int getOrCreateMidiOutput(const juce::String &identifier);
juce::String midiOutputNameForIdentifier(const juce::String &id) const;
// Configure a chain slot to send MIDI OUT to a device (by identifier).
  // Message-thread only. Mutually exclusive with hosting a plugin.
  void setSlotChainMidiOut(int slotIdx, int chainIdx,
                           const juce::String &deviceIdentifier, int channel);
void clearSlotChainMidiOut(int slotIdx, int chainIdx);
void setFohOutputOffset(int offset) { fohOutputOffset = offset; }
  
void setIemOutputOffset(int offset) { iemOutputOffset = offset; }
  
int getFohOutputOffset() const { return fohOutputOffset; }
  
int getIemOutputOffset() const { return iemOutputOffset; }

  
void triggerPanic() { panicTriggered.store(true); }

  
void setSelectedSlot(int index) {
    selectedSlotIndex = juce::jlimit(0, (int)slots.size() - 1, index);
  }

  
void resetScenesToDefault() {
    initializeDefaultScenes();
  }

private:
  void initializeDefaultScenes() {
    juce::ScopedLock sl(scenesLock);
    scenes.clear();
    Scene s1("WARM PIANO");
    // Initialize 10 slots
    for (int i = 0; i < (int)slots.size(); ++i) {
      SlotState st;
      // Hardware inputs are active by default, synths are bypassed until
      // selected
      bool isHardwareInput = (slots[i]->getInputChannelIndex() >= 0);
      st.bypassed = !isHardwareInput && (i != 1);
      st.fohLevel = 0.8f;
      st.iemLevel = 0.8f;
      st.fohEnabled = (slots[i]->getName() != "Monitor In");
      st.iemEnabled = true;
      s1.slotStates.push_back(st);
    }
    scenes.push_back(s1);

    Scene s2("BIG B3");
    for (int i = 0; i < (int)slots.size(); ++i) {
      SlotState st;
      st.bypassed = (i != 1 && i != 2);
      st.fohLevel = (i == 2) ? 0.9f : 0.4f;
      st.iemLevel = (i == 2) ? 0.9f : 0.4f;
      st.fohEnabled = true;
      st.iemEnabled = true;
      s2.slotStates.push_back(st);
    }
    scenes.push_back(s2);

    Scene s3("SOLO PAD");
    for (int i = 0; i < (int)slots.size(); ++i) {
      SlotState st;
      st.bypassed = (i != 3);
      st.fohLevel = 0.8f;
      st.iemLevel = 0.8f;
      st.fohEnabled = true;
      st.iemEnabled = true;
      s3.slotStates.push_back(st);
    }
    scenes.push_back(s3);
    currentSceneIndex = 0;
  }

  
std::vector<std::unique_ptr<RackSlot>> slots;
std::vector<juce::MidiBuffer> slotMidiBuffers;
mutable juce::CriticalSection scenesLock;
std::vector<Scene> scenes;
int currentSceneIndex = 0;
juce::AudioPluginFormatManager formatManager;
juce::KnownPluginList knownPluginList;
std::atomic<bool> panicTriggered{false};
bool hasSavedPluginList = false;
double currentSampleRate = 44100.0;
int currentBlockSize = 512;
int selectedSlotIndex = 1;
juce::AudioBuffer<float> fohBus;
juce::AudioBuffer<float> iemBus;
juce::AudioBuffer<float> aux1Bus;
juce::AudioBuffer<float> aux2Bus;
// Pre-allocated per-block reusable buffers (avoid heap allocs on audio thread)
  juce::AudioBuffer<float> dummyAux1;
juce::AudioBuffer<float> dummyAux2;
juce::MidiBuffer emptyMidiBuf;
std::vector<std::unique_ptr<RackSlot>> auxReturns;
std::atomic<bool> isLoading{false};
std::atomic<bool> audioUnderrunFlag{false};
// set when a block bailed to silence
  std::atomic<bool> engineWedged{false};
// set when worker timeout occurs
  std::shared_ptr<std::atomic<bool>> engineAlive = std::make_shared<std::atomic<bool>>(true);
// Lock-Free Parallel DSP Work-Sharing Pool
  static constexpr int kMaxDspHelpers = 4;
std::vector<juce::AudioBuffer<float>> scratchBuffers;
std::atomic<bool> parallelProcessingEnabled{true};
std::atomic<DWORD_PTR> shelteredAffinityMask{0};
std::atomic<DWORD_PTR> dspWorkerAffinityMasks[kMaxDspHelpers] = {};
std::atomic<DWORD_PTR> backgroundAffinityMask{0};
std::atomic<int> activeDspHelperLimit{1};
std::atomic<bool> preventPowerThrottlingEnabled{true};
std::atomic<uint32_t> dspTuningGen{1};
int dspSlotQueue[FanfareConstants::kNumSlots] = {};
const float *const *dspRawInput = nullptr;
int dspNumInputs = 0;
std::atomic<int> dspActiveSamples{512};
std::atomic<int> consecutiveParallelTimeouts{0};
std::atomic<int> dspNextJobIdx{FanfareConstants::kNumSlots};
std::atomic<int> dspJobCount{0};
std::atomic<int> dspInFlightWorkers{0};
std::atomic<bool> dspShutdown{false};
std::atomic<bool> isInProcessAudio{false};
#ifdef _WIN32
HANDLE dspWorkerWakeEvents[kMaxDspHelpers] = {};
std::thread dspWorkers[kMaxDspHelpers];
#endif
void startDspWorkers();
void stopDspWorkers();
void runDspWorkerLoop(int workerIdx);
// Post-load fade-in to prevent transient bursts when a rig is applied
  std::atomic<int> postLoadFadeRemaining{0};
std::atomic<int> postLoadFadeTotal{0};
// Rig transition atomic guard + completion timestamp + telemetry
  std::atomic<bool> isApplyingRig{false};
std::atomic<juce::int64> lastRigApplyEndTicks{0};
std::atomic<float> lastQuietWindowMs{0.0f};
std::vector<std::unique_ptr<juce::AudioPluginInstance>> pendingReapPlugins;
// Master FX chains
  juce::SpinLock masterFxLock;
std::vector<std::unique_ptr<juce::AudioPluginInstance>> fohPluginChain;
std::vector<std::unique_ptr<juce::AudioPluginInstance>> iemPluginChain;
std::atomic<int> fohFxFaultCount[3]{};
std::atomic<int> iemFxFaultCount[3]{};
std::atomic<bool> fohFxBypassed[3]{};
std::atomic<bool> iemFxBypassed[3]{};
int fohNanHold[2] = {0, 0};
int iemNanHold[2] = {0, 0};
int directOutNanHold = 0;
std::atomic<float> fohPeakL{0.0f}
, fohPeakR{0.0f};
std::atomic<float> iemPeakL{0.0f}
, iemPeakR{0.0f};
std::atomic<float> fohMasterLevel{1.0f};
std::atomic<float> iemMasterLevel{1.0f};
std::atomic<int> defaultMidiChannel{FanfareConstants::kDefaultMidiChannel};
// 1 = legacy behaviour

  std::atomic<int> fohOutputOffset{0};
// Hardware channels 1+2
  std::atomic<int> iemOutputOffset{2};
// Hardware channels 3+4
  std::atomic<bool> currentRigFreshBuild{false};
juce::CriticalSection lock;
// Opened MIDI output devices for chain-slot MIDI OUT routing. Devices are
  // created on the message thread and appended (never destroyed while running);
  // the audio thread reads midiOutputs[idx] under a try-lock so an opening
  // device can never corrupt/block playback.
  juce::SpinLock midiOutputLock;
std::vector<std::unique_ptr<juce::MidiOutput>> midiOutputs;
std::map<juce::String, int> midiOutputIndexById;
#ifdef _WIN32
HMODULE avrtModule = nullptr;
typedef HANDLE(WINAPI * PAvSetMmThreadCharacteristicsW)(LPCWSTR, LPDWORD);
PAvSetMmThreadCharacteristicsW avSetMmThread = nullptr;
#endif
double stressTestPhase = 0.0;
int stressTestLastNote[FanfareConstants::kNumSlots];
public:
  std::atomic<bool> stressTestActive{false};
// Staging cache for async rig transitions: plugin instances built off-thread
  // by RigBuilder, keyed by rack location (see stagingKeyFor()).
  juce::CriticalSection stagingLock;
std::map<juce::String, std::unique_ptr<juce::AudioPluginInstance>> stagedPlugins;
std::map<juce::String, std::unique_ptr<juce::AudioPluginInstance>> preloadedPlugins;
std::set<juce::String> stagedFailedKeys;
std::map<juce::String, juce::String> lastAppliedPluginStates;
public:
  std::vector<PluginInfo> availablePlugins = {
      // --- Original Plugins ---
      {"bx_meter (VU)", "C:\\Program Files\\Common Files\\VST3\\bx_meter.vst3"},
      {"EZkeys 2", "C:\\Program Files\\Common Files\\VST3\\EZkeys 2.vst3"},
      {"TR5 Metering",
       "C:\\Program Files\\Common Files\\VST3\\TR5 Metering.vst3"},
      {"Kontakt 8", "C:\\Program Files\\Common Files\\VST3\\Kontakt 8.vst3"},
      {"Supercharger GT",
       "C:\\Program Files\\Common Files\\VST3\\Supercharger GT.vst3"},
      {"Super 8", "C:\\Program Files\\Common Files\\VST3\\Super 8.vst3"},
      {"JUNO-106", "C:\\Program Files\\Common "
                   "Files\\VST3\\Roland\\JUNO-106\\JUNO-106(VST3 64bit).vst3"},
      {"ZENOLOGY", "C:\\Program Files\\Common "
                   "Files\\VST3\\Roland\\ZENOLOGY\\ZENOLOGY.vst3"},
      {"XV-5080", "C:\\Program Files\\Common "
                  "Files\\VST3\\Roland\\XV-5080\\XV-5080(VST3 64bit).vst3"},
      {"UVI Workstation", "C:\\Program Files\\Common "
                          "Files\\VST3\\UVIWorkstation.vst3\\Contents\\x86_64-"
                          "win\\UVIWorkstation.vst3"},

      // --- New Additions ---
      {"bx_console Focusrite SC",
       "C:\\Program Files\\Common Files\\VST3\\bx_console Focusrite SC.vst3"},
      {"Jun-6 V", "C:\\Program Files\\Common Files\\VST3\\Jun-6 V.vst3"},
      {"Replika XT", "C:\\Program Files\\Common Files\\VST3\\Replika XT.vst3"},
      {"Blue3 Organ", "C:\\Program Files\\Common Files\\VST3\\Cherry "
                      "Audio\\Blue3 Organ.vst3"},
      {"MixBox", "C:\\Program Files\\Common Files\\VST3\\MixBox.vst3"},
      {"Hammond B-3X",
       "C:\\Program Files\\Common Files\\VST3\\Hammond B-3X.vst3"},

      // --- Dave's Latest Additions ---
      {"Syntronik 2",
       "C:\\Program Files\\Common Files\\VST3\\Syntronik 2.vst3"},
      {"Omnisphere", "C:\\Program Files\\Common Files\\VST3\\Omnisphere.vst3"},
      {"PolyMax (UA)",
       "C:\\Program Files\\Common Files\\VST3\\uaudio_polymax.vst3"},
      {"Pre 1973", "C:\\Program Files\\Common Files\\VST3\\Pre 1973.vst3"},
      {"Pre TridA", "C:\\Program Files\\Common Files\\VST3\\Pre TridA.vst3"},
      {"Chorus JUN-6",
       "C:\\Program Files\\Common Files\\VST3\\Chorus JUN-6.vst3"},
      {"Bus EXCITER-104",
       "C:\\Program Files\\Common Files\\VST3\\Bus EXCITER-104.vst3"},
      {"Bus FORCE", "C:\\Program Files\\Common Files\\VST3\\Bus FORCE.vst3"},
      {"Bus PEAK", "C:\\Program Files\\Common Files\\VST3\\Bus PEAK.vst3"},
      {"TR5 British Channel",
       "C:\\Program Files\\Common Files\\VST3\\TR5 British Channel.vst3"},
      {"TR5 White Channel",
       "C:\\Program Files\\Common Files\\VST3\\TR5 White Channel.vst3"},
  };
int getNumAvailablePlugins() const { return (int)availablePlugins.size(); }
  
juce::String getAvailablePluginName(int index) const {
    return availablePlugins[index].name;
  }
  
juce::String getAvailablePluginPath(int index) const {
    return availablePlugins[index].path;
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
  std::unique_ptr<juce::AudioPluginInstance> buildInstanceOffThread(
      const juce::PluginDescription &desc, juce::String &error,
      int timeoutMs = 300000);
// Load a plugin from the registry into a specific slot
  // NOTE: instantiation+configuration happen on an MTA background thread with
  // a bounded wait — see buildInstanceOffThread. The message thread waits
  // (UI frozen for the build duration) but can no longer deadlock inside a
  // plugin whose init needs a pumping thread.
  void
  loadPluginIntoSlot(int slotIndex, int chainIndex, int pluginIndex,
                     std::function<void(bool, const juce::String &)> callback);
// Same logic for Master chains
  void loadPluginIntoMasterBus(
      bool isFoh, int chainIndex, int pluginIndex,
      std::function<void(bool, const juce::String &)> callback);
// ========== ASYNC RIG TRANSITION SUPPORT ==========
  // The RigBuilder builds plugin instances off the audio/message threads into
  // a staging cache; applyRig() then consumes the cache under the callback
  // lock, keeping the locked window short (heavy instantiation already done).

  struct PluginPathSnapshot {
    std::vector<juce::StringArray> slotChains; // [slotIdx] -> normalized paths (padded)
    std::vector<juce::StringArray> auxChains;  // [auxIdx] -> normalized paths (padded)
    juce::StringArray fohChain;                // normalized master paths (padded)
    juce::StringArray iemChain;
  };
// Snapshot normalized plugin paths of the live rack (briefly holds lock).
  PluginPathSnapshot snapshotPluginPaths();
// Build (create + configure + prepare + restore state) a plugin instance from
  // its serialized var, WITHOUT touching the rack. Thread-safe vs. the audio
  // thread. Used off-thread by RigBuilder and on-thread as a fallback.
  bool buildPluginFromVar(const juce::var &vt,
                          std::unique_ptr<juce::AudioPluginInstance> &out,
                          juce::String &error);
// Silent processBlock validation. Returns false if processing throws.
  bool validatePluginInstance(juce::AudioPluginInstance &inst);
// Staging cache: pre-built plugin instances keyed by rack location.
  static juce::String stagingKeyFor(int slotIdx, int chainIdx, bool isFoh);
void pushStagedPlugin(const juce::String &key,
                        std::unique_ptr<juce::AudioPluginInstance> inst) {
    juce::ScopedLock sl(stagingLock);
    stagedPlugins[key] = std::move(inst);
  }
  
bool popStagedPlugin(const juce::String &key,
                       std::unique_ptr<juce::AudioPluginInstance> &out);
void clearStagingCache() {
    juce::ScopedLock sl(stagingLock);
    stagedPlugins.clear();
    stagedFailedKeys.clear();
  }
  
void markStagedBuildFailed(const juce::String &key) {
    juce::ScopedLock sl(stagingLock);
    stagedFailedKeys.insert(key);
  }
  
bool isStagedBuildFailed(const juce::String &key) {
    juce::ScopedLock sl(stagingLock);
    return stagedFailedKeys.count(key) > 0;
  }
  
bool stagingHasKey(const juce::String &key) {
    juce::ScopedLock sl(stagingLock);
    return stagedPlugins.find(key) != stagedPlugins.end();
  }
  
bool hasStagedPlugins() {
    juce::ScopedLock sl(stagingLock);
    return !stagedPlugins.empty();
  }

  
bool isPluginStateIdentical(const juce::String &key, const juce::String &stateBase64) const;
void pushPreloadedPlugin(const juce::String &key,
                           std::unique_ptr<juce::AudioPluginInstance> inst) {
    juce::ScopedLock sl(stagingLock);
    preloadedPlugins[key] = std::move(inst);
  }
  
void clearPreloadedCache() {
    juce::ScopedLock sl(stagingLock);
    preloadedPlugins.clear();
  }
  
bool hasPreloadedPlugin(const juce::String &key) {
    juce::ScopedLock sl(stagingLock);
    return preloadedPlugins.find(key) != preloadedPlugins.end();
  }
  
void promotePreloadedToStaged();
bool hasPreloadedPlugins() {
    juce::ScopedLock sl(stagingLock);
    return !preloadedPlugins.empty();
  }

  
juce::CriticalSection &getCallbackLock() { return lock; }

  
Mp3PlayerProcessor* getMp3Player() {
    auto* s = getSlot(10);
    return s ? &s->getMp3Player() : nullptr;
  }

  
// Gig Notepad State (thread-safe across Message Thread & WebSocket Companion)
  std::vector<Fanfare::NoteTab> getNotes() const {
    juce::ScopedLock nl(notesLock);
    return currentNotes;
  }
  
void setNotes(const std::vector<Fanfare::NoteTab>& n) {
    juce::ScopedLock nl(notesLock);
    currentNotes = n;
  }
  
int getActiveNoteTabIndex() const {
    juce::ScopedLock nl(notesLock);
    return activeNoteTabIndex;
  }
  
void setActiveNoteTabIndex(int idx) {
    juce::ScopedLock nl(notesLock);
    activeNoteTabIndex = idx;
  }
  
float getNoteFontSize() const {
    juce::ScopedLock nl(notesLock);
    return noteFontSize;
  }
  
void setNoteFontSize(float sz) {
    juce::ScopedLock nl(notesLock);
    noteFontSize = sz;
  }
  
bool getNoteIsMonospace() const {
    juce::ScopedLock nl(notesLock);
    return noteIsMonospace;
  }
  
void setNoteIsMonospace(bool mono) {
    juce::ScopedLock nl(notesLock);
    noteIsMonospace = mono;
  }

  
void broadcastNotesChanged();
void addNotesListener(void* key, std::function<void()> callback) {
    noteListeners[key] = callback;
  }

  
void removeNotesListener(void* key) {
    noteListeners.erase(key);
  }

  
std::function<void()> onNotesChanged;
private:
  mutable juce::CriticalSection notesLock;
std::vector<Fanfare::NoteTab> currentNotes{ { "Chords", "" }, { "Lyrics", "" }, { "Notes", "" } };
std::map<void*, std::function<void()>> noteListeners;
int activeNoteTabIndex = 0;
float noteFontSize = 20.0f;
bool noteIsMonospace = true;
public:
  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(FanfareEngine)

};
