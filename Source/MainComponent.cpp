// MainComponent.cpp - part of MainComponent (mechanical split, see nov8mistral.md)
// Windows MMCSS for real-time audio thread priority
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dbt.h>
#include <mmsystem.h>

#pragma comment(lib, "winmm.lib")
#endif
#include <limits>

#include "CCMappingComponent.h"
#include "FanfareLogoData.h"
#include "HelpLauncher.h"
#include "MainComponent.h"
#include "NoteRangeComponent.h"
#include "RackSlotComponent.h"
#include "SaveHistory.h"
#include "ThemeManager.h"

// Optional personal gig art. The file is deliberately gitignored - public
// checkouts build without it and fall back to the committed OpenRig logo.
#if __has_include("LastWaltzLogoData.h")
#include "LastWaltzLogoData.h"
#define OPENRIG_HAS_LW_LOGO 1
#else
#define OPENRIG_HAS_LW_LOGO 0
#endif

#include "MainComponentInternal.h"

// Self-deleting window for plugin editors
// Windows managed by MainComponent

//==============================================================================
//==============================================================================
#ifdef _WIN32
struct CpuTopologyMasks {
  DWORD_PTR sysMask = 0;
  DWORD_PTR pCoreMask = 0;
  DWORD_PTR eCoreMask = 0;
  std::vector<DWORD_PTR> physicalCoreMasks;
};

static int countBitsInMask(DWORD_PTR m) noexcept {
  int c = 0;
  while (m != 0) {
    m &= (m - 1);
    ++c;
  }
  return c;
}

static CpuTopologyMasks queryCpuTopology() {
  CpuTopologyMasks topo;
  DWORD_PTR procMask = 0, sysMask = 0;
  if (GetProcessAffinityMask(GetCurrentProcess(), &procMask, &sysMask))
    topo.sysMask = sysMask;

  DWORD length = 0;
  GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &length);
  if (length == 0)
    return topo;
  std::vector<char> buffer(length);
  auto *info =
      reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buffer.data());
  if (!GetLogicalProcessorInformationEx(RelationProcessorCore, info, &length))
    return topo;

  BYTE maxEfficiency = 0;
  DWORD offset = 0;
  while (offset < length) {
    auto *cur = reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(
        buffer.data() + offset);
    if (cur->Relationship == RelationProcessorCore) {
      if (cur->Processor.EfficiencyClass > maxEfficiency)
        maxEfficiency = cur->Processor.EfficiencyClass;
    }
    offset += cur->Size;
  }

  offset = 0;
  while (offset < length) {
    auto *cur = reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(
        buffer.data() + offset);
    if (cur->Relationship == RelationProcessorCore &&
        cur->Processor.GroupCount > 0) {
      const DWORD_PTR coreMask = (DWORD_PTR)cur->Processor.GroupMask[0].Mask;
      topo.physicalCoreMasks.push_back(coreMask);
      if (maxEfficiency == 0 || cur->Processor.EfficiencyClass == maxEfficiency)
        topo.pCoreMask |= coreMask;
      else
        topo.eCoreMask |= coreMask;
    }
    offset += cur->Size;
  }
  return topo;
}
#endif

MainComponent::MainComponent() {
#ifdef _WIN32
  // Per-mechanism bisect flags - see FanfareConstants.h. Each test build
  // turns exactly one back on; the startup log line records the combo.
  avrtModule = LoadLibraryA("avrt.dll");
  if constexpr (FanfareConstants::kPowerSchemeMgmtEnabled)
    restoreWindowsPowerMode();
  if (avrtModule != nullptr) {
    avSetMmThreadFn = (PAvSetMmThreadCharacteristicsA)GetProcAddress(
        avrtModule, "AvSetMmThreadCharacteristicsA");
  }

  suppressGlitchDetectionForMs(3000);

  if constexpr (FanfareConstants::kPriorityElevationEnabled) {
    // Elevate process to ABOVE_NORMAL_PRIORITY_CLASS (Base Priority 10) rather
    // than HIGH_PRIORITY_CLASS (Base 13). In HIGH_PRIORITY_CLASS, internal
    // background threads spawned inside heavy VST3s (Kontakt 8, Roland
    // ZENOLOGY/XV-5080) get boosted to Kernel Priority 15, tying and preempting
    // DSP worker threads! In ABOVE_NORMAL, Fanfare still beats normal Windows
    // apps (8), plugin background threads cap at 12, DSP helpers run
    // undisturbed at THREAD_PRIORITY_TIME_CRITICAL (15), and ASIO runs at 24.
    if (!SetPriorityClass(GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS)) {
      LOG_WARN("Failed to set ABOVE_NORMAL_PRIORITY_CLASS for process");
    } else {
      LOG_INFO("Process set to ABOVE_NORMAL_PRIORITY_CLASS (clean 10 vs 15 vs "
               "24 priority tiering)");
    }
  }

  loadEngineTuningSettings();
  applyEngineTuningSettings();

  if constexpr (FanfareConstants::kWorkingSetLockEnabled) {
    // Lock a generous working set minimum so Windows cannot page out VST sample
    // libraries when other applications demand RAM. The 2 GB max we used before
    // Oct 2026 starved massive rigs (many Kontakt multis) during state restore
    // - Windows kept trimming sample data, making loads take minutes. 8 GB
    // covers the biggest setups; the OS still trims beyond it if RAM is truly
    // short.
    SIZE_T wsMin = 512ULL * 1024 * 1024;
    SIZE_T wsMax = 16384ULL * 1024 * 1024;
    if (!SetProcessWorkingSetSize(GetCurrentProcess(), wsMin, wsMax)) {
      LOG_WARN("Failed to lock process working set (512 MB - 16 GB)");
    } else {
      LOG_INFO("Process working set locked: min 512 MB, max 16 GB");
    }
  }
  if constexpr (FanfareConstants::kPowerSchemeMgmtEnabled)
    configureWindowsPerformanceMode();

  LOG_INFO(
      juce::String("Bisect build flags: mp3=") +
      (kMp3DecksEnabled ? "1" : "0") +
      " priority=" + (FanfareConstants::kPriorityElevationEnabled ? "1" : "0") +
      " wslock=" + (FanfareConstants::kWorkingSetLockEnabled ? "1" : "0") +
      " shelter=" + (FanfareConstants::kCpuShelterEnabled ? "1" : "0") +
      " mmcss=" + (FanfareConstants::kMmcssEnabled ? "1" : "0") +
      " execstate=" + (FanfareConstants::kExecStateLockEnabled ? "1" : "0"));
#endif
  // Initialize MIDI collector with default rate
  midiCollector.reset(FanfareConstants::kDefaultSampleRate);

  // One-time, non-destructive migration of legacy scattered rig files into
  // the consolidated %APPDATA%/OpenRig library layout.
  Fanfare::RigLibrary::migrateLegacyLibraryIfNeeded();
  Fanfare::RigLibrary::seedBundledLibraryIfNeeded();
  Fanfare::RigSerializer::ensureLibraryLayout();

  // Async rig transitioner (Pillar B: trustworthy, non-freezing song switches)
  transitioner = std::make_unique<Fanfare::RigTransitioner>(engine);

  // Ensure any hosted VST3 IPlugView / AudioProcessorEditor is torn down
  // synchronously on the message thread BEFORE its owning AudioPluginInstance
  // calls releaseResources() or is destroyed.
  engine.onBeforePluginDestroyed = [this](juce::AudioPluginInstance *pi) {
    if (pi == nullptr)
      return;
    if (juce::MessageManager::getInstanceWithoutCreating() != nullptr &&
        juce::MessageManager::getInstanceWithoutCreating()
            ->isThisTheMessageThread()) {
      closePluginWindowForInstance(pi);
    }
  };

  // Listen for audio/MIDI/setlist changes
  deviceManager.addChangeListener(this);
  Fanfare::SetlistManager::getInstance().addChangeListener(this);

  // Load persisted theme before applying the look and feel so the LnF reflects
  // the user's saved choice on first paint.
  ThemeManager::getInstance().loadFromSettings();
  ThemeManager::getInstance().addChangeListener(this);
  Fanfare::FanfareSplashScreen::reportProgress(
      0.32f, "Opening audio & MIDI devices...");
  loadAudioSettings();

  // Apply look and feel
  setLookAndFeel(&boutiqueLookAndFeel);
  juce::LookAndFeel::setDefaultLookAndFeel(&boutiqueLookAndFeel);
  boutiqueLookAndFeel.applyThemeColors();

  // Preload skeuomorphic assets for performance
  AssetLoader::getInstance().preloadAssets();

  // Setup UI components (extracted for readability)
  Fanfare::FanfareSplashScreen::reportProgress(
      0.56f, "Building channel strips & buses...");
  setupSlotComponents();
  setupHeaderButtons();
  setupSetupButtons();
  setupMasterSection();

  // Library toggle retired: the tool rail opens / closes the context sidebar.
  toggleLibraryBtn.setVisible(false);
  refreshThemeDependentStyles();

  Fanfare::SetlistManager::getInstance().setEngine(&engine);

  libraryPanel = std::make_unique<LibraryPanel>();
  addAndMakeVisible(libraryPanel.get());
  libraryPanel->onSetupDoubleClicked = [this](const juce::File &file) {
    loadRigFromFile(file);
  };
  libraryPanel->onDiagnoseSetup = [this](const juce::File &file) {
    showRigDoctorModal(file);
  };
  libraryPanel->onSetDoubleClicked = [this](const juce::File &file) {
    loadSetFile(file);
  };
  if (auto *slp = libraryPanel->getSetlistPanel()) {
    slp->onAddCurrentRequested = [this] {
      juce::String currentSetupName = setupNameLabel.getText();
      juce::File file = Fanfare::RigLibrary::getSongsDirectory().getChildFile(
          currentSetupName + ".json");
      if (file.existsAsFile()) {
        Fanfare::SetlistManager::getInstance().addSetup(file);
      } else {
        juce::AlertWindow::showMessageBoxAsync(
            juce::AlertWindow::InfoIcon, "Setlist",
            "Please save the current song first before adding it to the "
            "setlist.");
      }
    };
    slp->onLoadSetupRequested = [this](const juce::File &file, int row) {
      loadRigFromFile(file, row);
    };
  }

  Fanfare::FanfareSplashScreen::reportProgress(0.78f,
                                               "Preparing Studio workspace...");
  setupStudioShell();

  // Final setup
  setSize(FanfareConstants::kDefaultWindowWidth,
          FanfareConstants::kDefaultWindowHeight);
  startTimer(FanfareConstants::kTimerIntervalMs);

  // Setup MIDI inputs (wildcard listener + event-driven hotplug auto-enable)
#ifdef _WIN32
  startUsbDeviceWatcher();
#endif
  thermalMonitor.startMonitoring();
  {
    juce::Component::SafePointer<MainComponent> safeThis(this);
    updateChecker.checkOnceAsync(
        [safeThis](const UpdateChecker::ReleaseInfo &info) {
          if (safeThis == nullptr)
            return;
          safeThis->latestReleaseUrl = info.htmlUrl;
          juce::String label = "UPDATE v" + juce::String(info.major) + "." +
                               juce::String(info.minor);
          safeThis->updateBadgeBtn.setButtonText(label);
          safeThis->updateBadgeBtn.setTooltip(
              "New Fanfare milestone release available on GitHub (" +
              info.tagName + "). Click to view release notes and download.");
          safeThis->updateBadgeBtn.setVisible(true);
          safeThis->resized();
        });
  }
  deviceManager.addMidiInputDeviceCallback({}, this);
  syncHotplugMidiInputs(true);

  // Add audio callback
  deviceManager.addAudioCallback(this);

  // Initialize scene buttons from default scenes
  refreshSceneButtons();

  // Initialize embedded Web Server for Tablet HUD Companion
  Fanfare::FanfareSplashScreen::reportProgress(
      0.90f, "Starting stage remote & telemetry...");
  webServer = std::make_unique<OpenRig::WebServer>(engine, this);
  webServer->startServer(8080);
  hotspotController = std::make_unique<OpenRig::StageHotspotController>();
  engine.onNotesChanged = [this]() {
    if (webServer)
      webServer->notifyNotesChanged();
  };

  if (auto *mp3 = engine.getMp3Player()) {
    mp3->onPlayStarted = [this]() {
      if (webServer)
        webServer->notifyMp3Changed();
    };
    mp3->onTrackChanged = [this]() {
      if (webServer)
        webServer->notifyMp3Changed();
    };
    mp3->onPlaylistChanged = [this]() {
      if (webServer)
        webServer->notifyMp3Changed();
    };
    mp3->onBanksChanged = [this]() {
      if (webServer)
        webServer->notifyMp3Changed();
    };
  }

  LOG_INFO("OpenRig initialized successfully");
  setWantsKeyboardFocus(true);
  addKeyListener(this);
}

void MainComponent::onStartupSplashFinished() {
  // Triggered after the startup splash finishes fading out so the Welcome
  // modal (or first-run scan prompt) never overlaps the splash screen.
  if (Fanfare::WelcomePrefs::getShowOnStartup()) {
    showWelcomeModal();
  } else if (!engine.hasEverScannedPlugins()) {
    showQuickStartDialog(true);
  }
}

MainComponent::~MainComponent() {
  if (menuBar)
    menuBar->setModel(nullptr);
  rackHScrollBar.removeListener(&rackScrollListener);
  if (webServer)
    webServer->stopServer();
  removeKeyListener(this);
  stopTimer();
  updateChecker.stop();
  thermalMonitor.stopMonitoring();
  flushGlitchLog(); // land any batched glitch entries before we vanish
  if (transitioner)
    transitioner->stopTransition();
  engine.onBeforePluginDestroyed = nullptr;
  for (auto *w : activePluginWindows) {
    if (auto *pw = dynamic_cast<PluginWindow *>(w))
      pw->destroyEditorNow();
  }
  activePluginWindows.clear();
  deviceManager.removeChangeListener(this);
  Fanfare::SetlistManager::getInstance().removeChangeListener(this);
  ThemeManager::getInstance().removeChangeListener(this);
  saveAudioSettings();
#ifdef _WIN32
  stopUsbDeviceWatcher();
#endif
  for (const auto &id : activeMidiInputIds)
    deviceManager.setMidiInputDeviceEnabled(id, false);
  deviceManager.removeMidiInputDeviceCallback({}, this);

  deviceManager.removeAudioCallback(this);
  engine.releaseAllPlugins();
  juce::LookAndFeel::setDefaultLookAndFeel(nullptr);
  setLookAndFeel(nullptr);

#ifdef _WIN32
  if (avrtModule != nullptr) {
    FreeLibrary(avrtModule);
    avrtModule = nullptr;
  }
#endif
}

//==============================================================================
// Setup Helper Methods (extracted from constructor for readability)
//==============================================================================

#ifdef _WIN32
void MainComponent::configureWindowsPerformanceMode() {
  // 1. Prevent system sleep and display turn-off while Fanfare is running
  if constexpr (FanfareConstants::kExecStateLockEnabled)
    SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED |
                            ES_DISPLAY_REQUIRED);
  LOG_INFO("Windows system sleep & display sleep disabled for live audio "
           "performance");

  // 2. Check battery vs AC power status
  SYSTEM_POWER_STATUS sps;
  if (GetSystemPowerStatus(&sps)) {
    if (sps.ACLineStatus == 0) { // 0 = Offline (Running on Battery)
      int pct =
          (sps.BatteryLifePercent != 255) ? (int)sps.BatteryLifePercent : -1;
      LOG_WARN("System is running on BATTERY (" +
               (pct >= 0 ? juce::String(pct) + "%" : "unknown charge") +
               "). Connecting AC power is strongly recommended for live "
               "performance.");

      juce::Timer::callAfterDelay(1500, [pct] {
        juce::String msg =
            "The laptop is currently running on BATTERY" +
            (pct >= 0 ? " (" + juce::String(pct) + "% remaining).\n\n"
                      : ".\n\n") +
            "Windows aggressively throttles CPU clock speeds on battery power, "
            "which can cause " +
            "audio buffer underruns, clicks, and dropouts during live "
            "performance.\n\n" +
            "Please plug in AC mains power.";
        juce::AlertWindow::showMessageBoxAsync(
            juce::MessageBoxIconType::WarningIcon, "AC Power Disconnected", msg,
            "OK");
      });
    } else if (sps.ACLineStatus == 1) {
      LOG_INFO("AC power verified: plugged into mains.");
    }
  }

  // 3. Automatically force Windows into High Performance power mode
  HMODULE powrprof = LoadLibraryA("powrprof.dll");
  if (powrprof != nullptr) {
    auto pfnGet = (DWORD(WINAPI *)(HKEY, GUID **))GetProcAddress(
        powrprof, "PowerGetActiveScheme");
    auto pfnSet = (DWORD(WINAPI *)(HKEY, const GUID *))GetProcAddress(
        powrprof, "PowerSetActiveScheme");

    if (pfnGet != nullptr && pfnSet != nullptr) {
      GUID *activeGuid = nullptr;
      if (pfnGet(NULL, &activeGuid) == ERROR_SUCCESS && activeGuid != nullptr) {
        previousPowerSchemeGuid = *activeGuid;
        previousPowerSchemeSaved = true;

        static const GUID kHighPerfGuid = {
            0x8c5e7fda,
            0xe8bf,
            0x4a96,
            {0x9a, 0x85, 0xa6, 0xe2, 0x3a, 0x8c, 0x63, 0x5c}};
        static const GUID kUltimatePerfGuid = {
            0xe9a42b02,
            0xd5df,
            0x448d,
            {0xaa, 0x00, 0x03, 0xf1, 0x47, 0x49, 0xeb, 0x61}};

        if (*activeGuid != kHighPerfGuid && *activeGuid != kUltimatePerfGuid) {
          LOG_INFO("Active power scheme is not High Performance. Activating "
                   "High Performance scheme...");
          if (pfnSet(NULL, &kHighPerfGuid) == ERROR_SUCCESS) {
            powerPlanForcedHigh = true;
            LOG_INFO("Successfully activated Windows High Performance power "
                     "scheme.");
          } else {
            WinExec("powercfg /setactive 8c5e7fda-e8bf-4a96-9a85-a6e23a8c635c",
                    SW_HIDE);
            powerPlanForcedHigh = true;
            LOG_INFO("Dispatched powercfg /setactive High Performance.");
          }
        } else {
          LOG_INFO("Windows is already running in High/Ultimate Performance "
                   "power scheme.");
        }
        LocalFree(activeGuid);
      }
    }
    FreeLibrary(powrprof);
  }
}

void MainComponent::restoreWindowsPowerMode() {
  // Restore normal system sleep and display timeout behavior
  SetThreadExecutionState(ES_CONTINUOUS);

  // Restore previous power scheme if we forced High Performance
  if (previousPowerSchemeSaved && powerPlanForcedHigh) {
    HMODULE powrprof = LoadLibraryA("powrprof.dll");
    if (powrprof != nullptr) {
      auto pfnSet = (DWORD(WINAPI *)(HKEY, const GUID *))GetProcAddress(
          powrprof, "PowerSetActiveScheme");
      if (pfnSet != nullptr) {
        pfnSet(NULL, &previousPowerSchemeGuid);
        LOG_INFO("Restored original Windows power scheme on application exit.");
      }
      FreeLibrary(powrprof);
    }
  }
}
#endif

void MainComponent::setRackScrollX(int newX) {
  const int maxScroll =
      juce::jmax(0, rackStripContainer.getWidth() - rackViewport.getWidth());
  const int clamped = juce::jlimit(0, maxScroll, newX);
  if (rackViewport.getViewPositionX() != clamped)
    rackViewport.setViewPosition(clamped, 0);
  rackHScrollBar.setCurrentRange((double)clamped,
                                 (double)rackViewport.getWidth(),
                                 juce::dontSendNotification);
}

void MainComponent::scrollRackBy(int deltaPx) {
  setRackScrollX(rackViewport.getViewPositionX() + deltaPx);
}

void MainComponent::ensureStripVisibleInRack(RackSlotComponent *target) {
  if (target == nullptr || !target->isVisible() || rackViewport.getWidth() <= 0)
    return;
  const int viewX = rackViewport.getViewPositionX();
  const int viewW = rackViewport.getWidth();
  const int stripLeft = target->getX();
  // Include 36px lookahead on the right so the newly revealed "+ ADD" tile is
  // visible too
  const int stripRight =
      juce::jmin(rackStripContainer.getWidth(), target->getRight() + 36);
  if (stripRight > viewX + viewW)
    setRackScrollX(stripRight - viewW);
  else if (stripLeft < viewX)
    setRackScrollX(stripLeft);
}

void MainComponent::logAudioGlitch(double actualMs, double expectedMs,
                                   double cbDurationMs) {
  juce::String timeStr = juce::Time::getCurrentTime().formatted("%H:%M:%S.");
  timeStr += juce::String(juce::Time::getCurrentTime().getMilliseconds())
                 .paddedLeft('0', 3);

  juce::String entry =
      "[" + timeStr +
      "] [AUDIO UNDERRUN GLITCH] Buffer stall: " + juce::String(actualMs, 2) +
      "ms (expected " + juce::String(expectedMs, 2) +
      "ms) | callback took: " + juce::String(cbDurationMs, 2) +
      "ms | CPU: " + cpuLabel.getText() + " | RAM: " + ramLabel.getText() +
      " | " + thermalMonitor.formatGlitchLogTag() + "\n";

  // Batched: appends collect in memory and flush to disk at most once per
  // second (glitchMonitorTimer). During a storm this replaces ~13 file
  // opens/second with a single append - the message thread stays responsive.
  pendingGlitchLogText += entry;
}

void MainComponent::logAudioNanSpike(int channel, int sampleIndex,
                                     float badVal) {
  juce::String timeStr = juce::Time::getCurrentTime().formatted("%H:%M:%S.");
  timeStr += juce::String(juce::Time::getCurrentTime().getMilliseconds())
                 .paddedLeft('0', 3);

  juce::String entry = "[" + timeStr + "] [SAMPLE CORRUPTION / NAN SPIKE] Ch " +
                       juce::String(channel) + " Sample " +
                       juce::String(sampleIndex) +
                       " Value: " + juce::String(badVal) + "\n";

  pendingGlitchLogText += entry;
}

void MainComponent::flushGlitchLog() {
  if (pendingGlitchLogText.isEmpty())
    return;
  auto appData =
      juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
          .getChildFile("Fanfare");
  if (!appData.exists())
    appData.createDirectory();
  auto logFile = appData.getChildFile("audio_underrun_log.txt");
  juce::String batch;
  {
    juce::ScopedLock sl(glitchLogLock);
    batch = pendingGlitchLogText;
    pendingGlitchLogText.clear();
  }
  logFile.appendText(batch);
}

void MainComponent::audioDeviceIOCallbackWithContext(
    const float *const *inputChannelData, int numInputChannels,
    float *const *outputChannelData, int numOutputChannels, int numSamples,
    const juce::AudioIODeviceCallbackContext & /*context*/) {
  // Callback interval timing
  juce::int64 nowTicks = juce::Time::getHighResolutionTicks();
  juce::int64 prevTicks = lastAudioCallbackTicks.exchange(nowTicks);
  if (prevTicks > 0) {
    double elapsedMs =
        juce::Time::highResolutionTicksToSeconds(nowTicks - prevTicks) * 1000.0;
    double expectedMs =
        (double)numSamples /
        (engine.getCurrentSampleRate() > 0.0 ? engine.getCurrentSampleRate()
                                             : 44100.0) *
        1000.0;
    if (expectedMs > 0.0 && elapsedMs > expectedMs * 2.2) {
      const juce::int64 suppressUntil =
          suppressGlitchUntilTicks.load(std::memory_order_relaxed);
      const juce::int64 applyEndTicks = engine.getLastRigApplyEndTicks();
      const bool postApplySettle =
          (applyEndTicks > 0) && (juce::Time::highResolutionTicksToSeconds(
                                      nowTicks - applyEndTicks) < 0.5);
      const double prevDurationMs =
          lastCallbackDurationMs.load(std::memory_order_relaxed);
      const bool isSystemSleepOrSuspendGap =
          (elapsedMs > 5000.0 && prevDurationMs < 500.0);

      if (isSystemSleepOrSuspendGap) {
        // Multi-second/multi-hour timestamp jump across OS sleep/hibernate or
        // USB audio standby: re-arm post-wake settle window without logging a
        // fake underrun.
        suppressGlitchDetectionForMs(2000);
      } else if (nowTicks < suppressUntil || engine.isRigTransitioning() ||
                 postApplySettle) {
        // Suppressed during brief device-open settle or active rig state swap
      } else {
        audioUnderrunCount++;
        lastGlitchStallMs.store(elapsedMs);
        pendingGlitchActualMs.store(elapsedMs);
        pendingGlitchExpectedMs.store(expectedMs);
        pendingGlitchDurationMs.store(prevDurationMs);
        hasPendingGlitchLog.store(true);
      }
    }
  }

  // Per-thread hardening, flag-gated per mechanism (bisect - MinHost runs
  // no MMCSS and no shelter, and is clean where Fanfare storms).
#ifdef _WIN32
  FanfareLog::t_isRealtimeAudioThread = true;
  if constexpr (FanfareConstants::kCpuShelterEnabled) {
    const DWORD_PTR wantedProcMask =
        processShelterMask.load(std::memory_order_relaxed);
    if (wantedProcMask != 0) {
      DWORD_PTR procMask = 0, sysMask = 0;
      if (GetProcessAffinityMask(GetCurrentProcess(), &procMask, &sysMask) &&
          procMask != wantedProcMask) {
        LOG_WARN("Process affinity was modified to 0x" +
                 juce::String::toHexString((juce::int64)procMask) +
                 " - re-applying process shelter mask 0x" +
                 juce::String::toHexString((juce::int64)wantedProcMask));
        SetProcessAffinityMask(GetCurrentProcess(), wantedProcMask);
      }
    }

    const DWORD_PTR wantedDspMask =
        dspThreadAffinityMask.load(std::memory_order_relaxed);
    thread_local DWORD lastConfiguredTid = 0;
    thread_local DWORD_PTR lastThreadMask = 0;
    thread_local bool lastPreventEco = false;
    const DWORD curTid = ::GetCurrentThreadId();
    const bool wantPreventEco = engine.isPreventPowerThrottlingEnabled();
    if (curTid != lastConfiguredTid || lastThreadMask != wantedDspMask ||
        lastPreventEco != wantPreventEco) {
      lastConfiguredTid = curTid;
      lastThreadMask = wantedDspMask;
      lastPreventEco = wantPreventEco;
      if (wantedDspMask != 0)
        SetThreadAffinityMask(GetCurrentThread(), wantedDspMask);
      FanfareEngine::applyThreadHighQoS(wantPreventEco);
    }
  }
  if constexpr (FanfareConstants::kMmcssEnabled) {
    if (mmcssThreadId != ::GetCurrentThreadId()) {
      if (avSetMmThreadFn != nullptr) {
        DWORD taskIndex = 0;
        if (HANDLE h = avSetMmThreadFn("Pro Audio", &taskIndex)) {
          mmcssThreadId = ::GetCurrentThreadId();
          LOG_INFO(
              "Audio thread registered with MMCSS Pro Audio characteristics");
        } else {
          // Log the reason - MMCSS refusals were previously invisible.
          DWORD err = ::GetLastError();
          if (err ==
              1552 /* ERROR_INVALID_THREAD_ID: "already joining a task" */) {
            // Not a failure: ASIO drivers like Yamaha Steinberg USB join their
            // callback thread to the MMCSS Pro Audio task before we run, so the
            // thread already has Pro Audio priority. Don't touch it.
            mmcssThreadId = ::GetCurrentThreadId();
            LOG_INFO("Audio thread already MMCSS-registered by the audio "
                     "driver - Pro Audio priority active");
          } else {
            // Fallback HIGHEST, not TIME_CRITICAL - TIME_CRITICAL can starve
            // the OS when combined with the audio spin-wait barrier.
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
            mmcssThreadId = ::GetCurrentThreadId();
            LOG_WARN("MMCSS unavailable (error " + juce::String((int)err) +
                     ") - audio thread set to THREAD_PRIORITY_HIGHEST");
          }
        }
      } else {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
        mmcssThreadId = ::GetCurrentThreadId();
        LOG_WARN("avrt.dll not loaded - audio thread set to "
                 "THREAD_PRIORITY_HIGHEST");
      }
    }
  } else {
    // When MMCSS is disabled (avoiding Windows 80% MMCSS throttling),
    // explicitly boost the ASIO callback thread to Kernel Priority 15 so it
    // matches dspWorkers and can never be preempted by Priority 9-12 plugin/UI
    // threads or DSP helpers.
    thread_local DWORD lastBoostedAudioTid = 0;
    const DWORD curAudioTid = ::GetCurrentThreadId();
    if (curAudioTid != lastBoostedAudioTid) {
      lastBoostedAudioTid = curAudioTid;
      SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
      LOG_INFO("Audio callback thread boosted to THREAD_PRIORITY_TIME_CRITICAL "
               "(Priority 15)");
    }
  }
#endif

  juce::MidiBuffer incomingMidi;
  midiCollector.removeNextBlockOfMessages(incomingMidi, numSamples);
  engine.processAudio(inputChannelData, numInputChannels, outputChannelData,
                      numOutputChannels, numSamples, incomingMidi);

  lastCallbackDurationMs.store(
      juce::Time::highResolutionTicksToSeconds(
          juce::Time::getHighResolutionTicks() - nowTicks) *
      1000.0);
}

void MainComponent::stripLegacyDirectSoundDeviceType() {
  const auto &types = deviceManager.getAvailableDeviceTypes();
  for (int i = types.size() - 1; i >= 0; --i) {
    if (auto *t = types[i]) {
      if (t->getTypeName().containsIgnoreCase("DirectSound") &&
          deviceManager.getCurrentAudioDeviceType() != t->getTypeName()) {
        deviceManager.removeAudioDeviceType(t);
        LOG_INFO("Removed legacy DirectSound device type (prevents USB hotplug "
                 "scan stalls)");
      }
    }
  }
}

void MainComponent::captureDesiredAudioSetup() {
  if (auto *dev = deviceManager.getCurrentAudioDevice()) {
    if (dev->isOpen() && !dev->getActiveOutputChannels().isZero()) {
      desiredAudioDeviceType = deviceManager.getCurrentAudioDeviceType();
      desiredAudioSetup = deviceManager.getAudioDeviceSetup();
      if (desiredAudioSetup.outputDeviceName.isEmpty())
        desiredAudioSetup.outputDeviceName = dev->getName();
      hasDesiredAudioSetup = true;
    }
  }
}

#ifdef _WIN32
#ifndef DEVICE_NOTIFY_ALL_INTERFACE_CLASSES
#define DEVICE_NOTIFY_ALL_INTERFACE_CLASSES 0x00000004
#endif

void MainComponent::startUsbDeviceWatcher() {
  if (usbNotifyHwnd != nullptr)
    return;

  struct UsbNotifyWndProc {
    static LRESULT CALLBACK proc(HWND hwnd, UINT msg, WPARAM wParam,
                                 LPARAM lParam) {
      if (msg == WM_NCCREATE) {
        auto *cs = reinterpret_cast<CREATESTRUCTW *>(lParam);
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                            reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        return TRUE;
      }
      if (msg == WM_DEVICECHANGE) {
        if (wParam == DBT_DEVICEARRIVAL || wParam == DBT_DEVICEREMOVECOMPLETE) {
          if (auto *self = reinterpret_cast<MainComponent *>(
                  ::GetWindowLongPtrW(hwnd, GWLP_USERDATA))) {
            self->usbTopologyDirty.store(true, std::memory_order_release);
          }
        }
      }
      return ::DefWindowProcW(hwnd, msg, wParam, lParam);
    }
  };

  static const wchar_t *kClassName = L"FanfareUsbNotifyWnd";
  HINSTANCE hInst = ::GetModuleHandleW(nullptr);

  WNDCLASSEXW wc{};
  wc.cbSize = sizeof(WNDCLASSEXW);
  wc.lpfnWndProc = &UsbNotifyWndProc::proc;
  wc.hInstance = hInst;
  wc.lpszClassName = kClassName;
  if (::RegisterClassExW(&wc) == 0 &&
      ::GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
    LOG_WARN("USB watcher: RegisterClassExW failed (err " +
             juce::String((int)::GetLastError()) + ")");
    return;
  }

  // Hidden top-level popup window (receives both WM_DEVICECHANGE broadcasts
  // and RegisterDeviceNotificationW interface arrival/removal messages).
  HWND hwnd =
      ::CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, kClassName, L"",
                        WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, hInst, this);
  if (hwnd == nullptr) {
    LOG_WARN("USB watcher: CreateWindowExW failed (err " +
             juce::String((int)::GetLastError()) + ")");
    return;
  }
  ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
  usbNotifyHwnd = hwnd;

  DEV_BROADCAST_DEVICEINTERFACE_W dbi{};
  dbi.dbcc_size = sizeof(dbi);
  dbi.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
  usbNotifyHandle = ::RegisterDeviceNotificationW(
      hwnd, &dbi,
      DEVICE_NOTIFY_WINDOW_HANDLE | DEVICE_NOTIFY_ALL_INTERFACE_CLASSES);
}

void MainComponent::stopUsbDeviceWatcher() {
  if (usbNotifyHandle != nullptr) {
    ::UnregisterDeviceNotification(static_cast<HDEVNOTIFY>(usbNotifyHandle));
    usbNotifyHandle = nullptr;
  }
  if (usbNotifyHwnd != nullptr) {
    HWND hwnd = static_cast<HWND>(usbNotifyHwnd);
    ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
    ::DestroyWindow(hwnd);
    usbNotifyHwnd = nullptr;
  }
}
#endif

void MainComponent::syncHotplugMidiInputs(bool isInitialSetup) {
  if (isSyncingMidiInputs)
    return;
  juce::ScopedValueSetter<bool> guard(isSyncingMidiInputs, true);

#ifdef _WIN32
  lastWinMmMidiInCount = (int)::midiInGetNumDevs();
#endif
  usbTopologyDirty.store(false, std::memory_order_release);

  const juce::int64 nowMs = (juce::int64)juce::Time::getMillisecondCounter();
  auto currentInputs = juce::MidiInput::getAvailableDevices();
  juce::StringArray currentIds;
  for (const auto &in : currentInputs)
    currentIds.add(in.identifier);

  // Clear backoff state for any port that left the OS device list so it
  // reconnects with zero delay if plugged back in later.
  for (auto it = midiOpenFailCounts.begin(); it != midiOpenFailCounts.end();) {
    if (!currentIds.contains(it->first)) {
      midiOpenRetryAfterMs.erase(it->first);
      it = midiOpenFailCounts.erase(it);
    } else {
      ++it;
    }
  }
  lastEnumeratedMidiIds = currentIds;

  bool anyDisconnected = false;
  bool mutatedDeviceManager = false;
  for (const auto &oldId : activeMidiInputIds) {
    if (!currentIds.contains(oldId)) {
      suppressHotplugChangeBroadcastUntilMs = nowMs + 500;
      deviceManager.setMidiInputDeviceEnabled(oldId, false);
      mutatedDeviceManager = true;
      anyDisconnected = true;
      LOG_WARN("USB MIDI input disconnected: " + oldId);
    }
  }
  if (anyDisconnected && !isInitialSetup) {
    engine.triggerPanic();
    if (auto *dev = deviceManager.getCurrentAudioDevice()) {
      if (dev->getCurrentSampleRate() > 0.0)
        midiCollector.reset(dev->getCurrentSampleRate());
    }
  }

  juce::StringArray nextActiveIds;
  bool anyNewlyConnected = false;
  for (const auto &in : currentInputs) {
    const bool wasActive = activeMidiInputIds.contains(in.identifier);
    const bool isEnabled =
        deviceManager.isMidiInputDeviceEnabled(in.identifier);
    if (wasActive && isEnabled) {
      nextActiveIds.add(in.identifier);
      continue;
    }

    auto retryIt = midiOpenRetryAfterMs.find(in.identifier);
    if (!isInitialSetup && retryIt != midiOpenRetryAfterMs.end() &&
        nowMs < retryIt->second) {
      continue;
    }

    suppressHotplugChangeBroadcastUntilMs = nowMs + 500;
    if (isEnabled && !wasActive) {
      deviceManager.setMidiInputDeviceEnabled(in.identifier, false);
    }
    deviceManager.setMidiInputDeviceEnabled(in.identifier, true);
    mutatedDeviceManager = true;

    const bool nowEnabled =
        deviceManager.isMidiInputDeviceEnabled(in.identifier);
    if (nowEnabled) {
      midiOpenFailCounts.erase(in.identifier);
      midiOpenRetryAfterMs.erase(in.identifier);
      nextActiveIds.add(in.identifier);
      if (!isInitialSetup) {
        anyNewlyConnected = true;
        LOG_INFO("USB MIDI input connected & enabled: " + in.name);
      }
    } else {
      const int fails = ++midiOpenFailCounts[in.identifier];
      if (fails == 1) {
        midiOpenRetryAfterMs[in.identifier] = nowMs + 5000;
        LOG_WARN("MIDI input '" + in.name +
                 "' is listed by Windows but unavailable (offline or in use) - "
                 "backing off auto-enable.");
      } else if (fails == 2) {
        midiOpenRetryAfterMs[in.identifier] = nowMs + 20000;
      } else if (fails == 3) {
        midiOpenRetryAfterMs[in.identifier] = nowMs + 60000;
      } else {
        midiOpenRetryAfterMs[in.identifier] =
            (std::numeric_limits<juce::int64>::max)();
        if (fails == 4) {
          LOG_WARN("MIDI input '" + in.name +
                   "' failed to open 4 times - quarantining until USB hardware "
                   "change or manual Reset Audio.");
        }
      }
    }
  }

  if (mutatedDeviceManager) {
    suppressHotplugChangeBroadcastUntilMs =
        (juce::int64)juce::Time::getMillisecondCounter() + 500;
  }

  activeMidiInputIds = nextActiveIds;

  if (anyNewlyConnected && inAudioRecoveryMode) {
    lastAudioRecoveryAttemptMs =
        (juce::int64)juce::Time::getMillisecondCounter() - 1900;
  }
}

void MainComponent::checkAndRecoverAudioDevice() {
  const juce::int64 nowCheckMs =
      (juce::int64)juce::Time::getMillisecondCounter();
  const bool topologyChanged =
      usbTopologyDirty.exchange(false, std::memory_order_acq_rel);
#ifdef _WIN32
  const int winMmCount = (int)::midiInGetNumDevs();
  const bool midiCountChanged = (winMmCount != lastWinMmMidiInCount);
#else
  const bool midiCountChanged = false;
#endif

  // If a genuine OS hardware event fired (with a 30s cooldown against noisy
  // hubs), allow quarantined virtual/Bluetooth ports one fresh probe.
  if ((topologyChanged || midiCountChanged) &&
      (nowCheckMs - lastTopologyUnquarantineMs >= 30000)) {
    lastTopologyUnquarantineMs = nowCheckMs;
    for (auto &kv : midiOpenRetryAfterMs) {
      if (kv.second == (std::numeric_limits<juce::int64>::max)()) {
        kv.second = nowCheckMs;
      }
    }
  }

  bool retryTimerDue = false;
  for (const auto &kv : midiOpenRetryAfterMs) {
    if (kv.second < (std::numeric_limits<juce::int64>::max)() &&
        nowCheckMs >= kv.second) {
      retryTimerDue = true;
      break;
    }
  }

  // Event-driven hotplug: never call MidiInput::getAvailableDevices() during
  // steady-state playback unless WinMM MIDI device count changed, a finite
  // retry backoff matured, or the audio interface itself is in recovery mode.
  // Probing WinMM/WDMAud on unrelated USB notifications stalls composite USB
  // ASIO drivers (e.g. Roland RD-88 at 128 samples).
  if (midiCountChanged || retryTimerDue || inAudioRecoveryMode) {
    syncHotplugMidiInputs(false);
  }

  if (!hasDesiredAudioSetup || isRecoveringAudioDevice ||
      activeAudioSelector != nullptr ||
      (transitioner != nullptr && transitioner->isBusy()))
    return;

  auto *dev = deviceManager.getCurrentAudioDevice();
  const juce::int64 nowTicks = juce::Time::getHighResolutionTicks();
  const juce::int64 lastCbTicks =
      lastAudioCallbackTicks.load(std::memory_order_relaxed);
  const double msSinceLastCallback =
      (lastCbTicks > 0)
          ? juce::Time::highResolutionTicksToSeconds(nowTicks - lastCbTicks) *
                1000.0
          : 0.0;

  const bool deviceMissingOrClosed = (dev == nullptr) || !dev->isOpen() ||
                                     !dev->isPlaying() ||
                                     dev->getActiveOutputChannels().isZero();
  const bool callbackStalled =
      (dev != nullptr && dev->isOpen() && lastCbTicks > 0 &&
       msSinceLastCallback > 1500.0);

  if (!deviceMissingOrClosed && !callbackStalled) {
    if (inAudioRecoveryMode) {
      inAudioRecoveryMode = false;
      audioRecoveryAttemptCount = 0;
      LOG_INFO("Audio interface streaming normally again.");
    }
    return;
  }

  const juce::int64 nowMs = (juce::int64)juce::Time::getMillisecondCounter();
  if (!inAudioRecoveryMode) {
    inAudioRecoveryMode = true;
    audioRecoveryAttemptCount = 0;
    lastAudioRecoveryAttemptMs = nowMs - 1300;
    engine.triggerPanic();
    LOG_WARN("Audio interface disconnected or stopped responding (" +
             juce::String(deviceMissingOrClosed ? "device closed"
                                                : "callbacks stalled") +
             ") - entering USB auto-recovery mode for '" +
             desiredAudioSetup.outputDeviceName + "'");
    return;
  }

  const juce::int64 retryIntervalMs =
      (audioRecoveryAttemptCount < 3) ? 2000 : 3000;
  if (nowMs - lastAudioRecoveryAttemptMs < retryIntervalMs)
    return;

  lastAudioRecoveryAttemptMs = nowMs;
  ++audioRecoveryAttemptCount;

  juce::ScopedValueSetter<bool> recoveryGuard(isRecoveringAudioDevice, true);
  suppressGlitchDetectionForMs(3500);

  deviceManager.closeAudioDevice();
  if (desiredAudioDeviceType.isNotEmpty() &&
      deviceManager.getCurrentAudioDeviceType() != desiredAudioDeviceType) {
    deviceManager.setCurrentAudioDeviceType(desiredAudioDeviceType, true);
  }

  juce::String err = deviceManager.setAudioDeviceSetup(desiredAudioSetup, true);
  auto *newDev = deviceManager.getCurrentAudioDevice();
  if (err.isEmpty() && newDev != nullptr && newDev->isOpen() &&
      newDev->isPlaying()) {
    inAudioRecoveryMode = false;
    audioRecoveryAttemptCount = 0;
    syncHotplugMidiInputs(false);
    LOG_INFO("USB Audio Auto-Recovery succeeded: '" + newDev->getName() +
             "' is back online!");
  }
}

namespace {
void deduplicateMidiInputsXml(juce::XmlElement &xml,
                              const juce::StringArray &currentlyActiveIds,
                              const std::map<juce::String, int> &failedCounts) {
  juce::StringArray seenIds;
  juce::StringArray seenOfflineNames;
  for (int i = xml.getNumChildElements() - 1; i >= 0; --i) {
    auto *child = xml.getChildElement(i);
    if (child == nullptr || !child->hasTagName("MIDIINPUT"))
      continue;
    const juce::String id = child->getStringAttribute("identifier");
    const juce::String name = child->getStringAttribute("name");
    auto failIt = failedCounts.find(id);
    if (failIt != failedCounts.end() && failIt->second > 0) {
      xml.removeChildElement(child, true);
      continue;
    }
    if (id.isNotEmpty() && seenIds.contains(id)) {
      xml.removeChildElement(child, true);
      continue;
    }
    const bool isCurrentlyActive =
        id.isNotEmpty() && currentlyActiveIds.contains(id);
    if (!isCurrentlyActive && name.isNotEmpty() &&
        seenOfflineNames.contains(name)) {
      xml.removeChildElement(child, true);
      continue;
    }
    if (id.isNotEmpty())
      seenIds.add(id);
    if (name.isNotEmpty())
      seenOfflineNames.add(name);
  }
}
} // namespace

void MainComponent::saveAudioSettings() {
  if (inAudioRecoveryMode || isRecoveringAudioDevice)
    return;
  auto *dev = deviceManager.getCurrentAudioDevice();
  if (dev == nullptr || !dev->isOpen() ||
      dev->getActiveOutputChannels().isZero())
    return;
  captureDesiredAudioSetup();
  auto xml = deviceManager.createStateXml();
  if (xml != nullptr) {
    deduplicateMidiInputsXml(*xml, activeMidiInputIds, midiOpenFailCounts);
    auto file =
        FanfareConstants::getAppDirectory().getChildFile("audio_settings.xml");
    file.getParentDirectory().createDirectory();
    xml->writeTo(file);
  }
}

void MainComponent::loadAudioSettings() {
  auto file =
      FanfareConstants::getAppDirectory().getChildFile("audio_settings.xml");
  if (file.existsAsFile()) {
    auto xml = juce::XmlDocument::parse(file);
    if (xml != nullptr) {
      if (xml->hasTagName("DEVICESETUP")) {
        deduplicateMidiInputsXml(*xml, {}, midiOpenFailCounts);
        desiredAudioDeviceType = xml->getStringAttribute("deviceType", "ASIO");
        juce::String devName = xml->getStringAttribute("audioDeviceName");
        desiredAudioSetup.outputDeviceName =
            devName.isNotEmpty()
                ? devName
                : xml->getStringAttribute("audioOutputDeviceName");
        desiredAudioSetup.inputDeviceName =
            devName.isNotEmpty()
                ? devName
                : xml->getStringAttribute("audioInputDeviceName");
        desiredAudioSetup.sampleRate =
            xml->getDoubleAttribute("audioDeviceRate", 44100.0);
        desiredAudioSetup.bufferSize =
            xml->getIntAttribute("audioDeviceBufferSize", 192);
        desiredAudioSetup.inputChannels.parseString(
            xml->getStringAttribute("audioDeviceInChans", "11"), 2);
        desiredAudioSetup.outputChannels.parseString(
            xml->getStringAttribute("audioDeviceOutChans", "11"), 2);
        desiredAudioSetup.useDefaultInputChannels =
            !xml->hasAttribute("audioDeviceInChans");
        desiredAudioSetup.useDefaultOutputChannels =
            !xml->hasAttribute("audioDeviceOutChans");
        hasDesiredAudioSetup = desiredAudioSetup.outputDeviceName.isNotEmpty();
      }
    }
    deviceManager.initialise(0, 8, xml.get(), !hasDesiredAudioSetup);
    stripLegacyDirectSoundDeviceType();
    captureDesiredAudioSetup();
  } else {
    deviceManager.initialiseWithDefaultDevices(0, 2);
    stripLegacyDirectSoundDeviceType();
    captureDesiredAudioSetup();
  }
}

// Swap the logo area beneath the aux returns. Fully opaque images (JPEGs,
// flattened posters) get their dark background luminance-keyed to
// transparency; images with real alpha (transparent PNGs) are used as-is.
void MainComponent::setAuxLogoImage(const juce::Image &loaded, bool animate) {
  bool fullyOpaque = true;
  {
    juce::Image::BitmapData px(loaded, juce::Image::BitmapData::readOnly);
    for (int y = 0; y < px.height && fullyOpaque; ++y)
      for (int x = 0; x < px.width; ++x)
        if (px.getPixelColour(x, y).getAlpha() < 250) {
          fullyOpaque = false;
          break;
        }
  }

  juce::Image use = loaded;
  if (fullyOpaque) {
    use = juce::Image(juce::Image::ARGB, loaded.getWidth(), loaded.getHeight(),
                      true);
    {
      juce::Graphics g(use);
      g.drawImageAt(loaded, 0, 0);
    }
    juce::Image::BitmapData px(use, juce::Image::BitmapData::readWrite);
    for (int y = 0; y < px.height; ++y)
      for (int x = 0; x < px.width; ++x) {
        auto c = px.getPixelColour(x, y);
        float lum =
            juce::jmax(c.getRed(), juce::jmax(c.getGreen(), c.getBlue())) /
            255.0f;
        px.setPixelColour(x, y,
                          c.withAlpha((juce::uint8)juce::jlimit(
                              0, 255, (int)((lum - 0.08f) * 1.6f * 255.0f))));
      }
  }

  lastWaltzLogoImage = use;
  lastWaltzLogoComponent.setImage(lastWaltzLogoImage,
                                  juce::RectanglePlacement::onlyReduceInSize |
                                      juce::RectanglePlacement::xMid |
                                      juce::RectanglePlacement::yMid);
  if (animate)
    juce::Desktop::getInstance().getAnimator().fadeIn(&lastWaltzLogoComponent,
                                                      250);
}

void MainComponent::chooseAuxLogoImage() {
  fileChooser = std::make_unique<juce::FileChooser>(
      "Choose a logo image...",
      juce::File::getSpecialLocation(juce::File::userPicturesDirectory),
      "*.png;*.jpg;*.jpeg");
  fileChooser->launchAsync(
      juce::FileBrowserComponent::openMode |
          juce::FileBrowserComponent::canSelectFiles,
      [this](const juce::FileChooser &fc) {
        auto file = fc.getResult();
        if (file == juce::File{})
          return;
        auto img = juce::ImageFileFormat::loadFrom(file);
        if (!img.isValid()) {
          juce::AlertWindow::showMessageBoxAsync(
              juce::MessageBoxIconType::WarningIcon, "Couldn't load image",
              "That file didn't decode as an image (PNG or JPG).");
          return;
        }
        auxLogoUserPath = file.getFullPathName();
        saveAuxLogoPreference();
        setAuxLogoImage(img, true);
      });
}

void MainComponent::resetAuxLogoImage() {
  auxLogoUserPath = "";
  saveAuxLogoPreference();
  juce::Image embedded;
#if OPENRIG_HAS_LW_LOGO
  embedded = juce::ImageFileFormat::loadFrom(last_waltz_logo_png,
                                             last_waltz_logo_png_size);
#endif
  if (!embedded.isValid())
    embedded = juce::ImageFileFormat::loadFrom(fanfare_logo_png,
                                               fanfare_logo_png_size);
  if (embedded.isValid())
    setAuxLogoImage(embedded, true);
}

void MainComponent::loadAuxLogoPreference() {
  auto f = FanfareConstants::getAppDirectory().getChildFile("ui_settings.json");
  if (!f.existsAsFile())
    return;
  auto v = juce::JSON::parse(f.loadFileAsString());
  juce::String p = v.getProperty("auxLogoPath", "").toString();
  if (p.isEmpty())
    return;
  juce::File imgFile(p);
  if (!imgFile.existsAsFile())
    return;
  auto img = juce::ImageFileFormat::loadFrom(imgFile);
  if (!img.isValid())
    return;
  auxLogoUserPath = p;
  setAuxLogoImage(img, false);
}

void MainComponent::saveAuxLogoPreference() const {
  auto f = FanfareConstants::getAppDirectory().getChildFile("ui_settings.json");
  juce::var v;
  if (f.existsAsFile()) {
    v = juce::JSON::parse(f.loadFileAsString());
  }
  if (!v.isObject()) {
    v = juce::var(new juce::DynamicObject());
  }
  if (auto *obj = v.getDynamicObject()) {
    obj->setProperty("auxLogoPath", auxLogoUserPath);
  }
  f.getParentDirectory().createDirectory();
  f.replaceWithText(juce::JSON::toString(v));
}

void MainComponent::loadEngineTuningSettings() {
  auto f = FanfareConstants::getAppDirectory().getChildFile("ui_settings.json");
  if (!f.existsAsFile())
    return;
  auto v = juce::JSON::parse(f.loadFileAsString());
  if (v.hasProperty("pCoresOnly"))
    engineTuningSettings.pCoresOnly = (bool)v["pCoresOnly"];
  if (v.hasProperty("shelterCpu0"))
    engineTuningSettings.shelterCpu0 = (bool)v["shelterCpu0"];
  if (v.hasProperty("preventPowerThrottling"))
    engineTuningSettings.preventPowerThrottling =
        (bool)v["preventPowerThrottling"];
  if (v.hasProperty("parallelProcessing"))
    engineTuningSettings.parallelProcessing = (bool)v["parallelProcessing"];
  if (v.hasProperty("preferredStripWidth")) {
    preferredExpandedWidth = juce::jlimit(kMinExpandedWidth, kMaxExpandedWidth,
                                          (int)v["preferredStripWidth"]);
    stripWidthSlider.setValue(preferredExpandedWidth,
                              juce::dontSendNotification);
  }
}

void MainComponent::savePreferredStripWidth() const {
  auto f = FanfareConstants::getAppDirectory().getChildFile("ui_settings.json");
  juce::var v;
  if (f.existsAsFile())
    v = juce::JSON::parse(f.loadFileAsString());
  if (!v.isObject())
    v = juce::var(new juce::DynamicObject());
  if (auto *obj = v.getDynamicObject())
    obj->setProperty("preferredStripWidth", preferredExpandedWidth);
  f.getParentDirectory().createDirectory();
  f.replaceWithText(juce::JSON::toString(v));
}

void MainComponent::saveEngineTuningSettings() {
  auto f = FanfareConstants::getAppDirectory().getChildFile("ui_settings.json");
  juce::var v;
  if (f.existsAsFile()) {
    v = juce::JSON::parse(f.loadFileAsString());
  }
  if (!v.isObject()) {
    v = juce::var(new juce::DynamicObject());
  }
  if (auto *obj = v.getDynamicObject()) {
    obj->setProperty("pCoresOnly", engineTuningSettings.pCoresOnly);
    obj->setProperty("shelterCpu0", engineTuningSettings.shelterCpu0);
    obj->setProperty("preventPowerThrottling",
                     engineTuningSettings.preventPowerThrottling);
    obj->setProperty("parallelProcessing",
                     engineTuningSettings.parallelProcessing);
  }
  f.getParentDirectory().createDirectory();
  f.replaceWithText(juce::JSON::toString(v));
}

void MainComponent::suppressGlitchDetectionForMs(int durationMs) noexcept {
  const juce::int64 now = juce::Time::getHighResolutionTicks();
  const juce::int64 delta =
      juce::Time::secondsToHighResolutionTicks(durationMs * 0.001);
  const juce::int64 target = now + delta;
  juce::int64 prev = suppressGlitchUntilTicks.load(std::memory_order_relaxed);
  while (prev < target && !suppressGlitchUntilTicks.compare_exchange_weak(
                              prev, target, std::memory_order_release,
                              std::memory_order_relaxed)) {
  }
}

void MainComponent::applyEngineTuningSettings() {
#ifdef _WIN32
  const CpuTopologyMasks topo = queryCpuTopology();
  const DWORD_PTR fullSysMask =
      (topo.sysMask != 0) ? topo.sysMask : ~(DWORD_PTR)0;

  // 1. PROCESS AFFINITY: Shelter ONLY CPU 0 (if enabled) while keeping ALL
  // Efficiency
  //    cores open so Windows + Intel Thread Director steer Kontakt disk
  //    threads, Roland background threads, Direct2D GUI, and USB ASIO helper
  //    threads off the P-cores!
  DWORD_PTR procMaskToApply = topo.sysMask;
  if (engineTuningSettings.shelterCpu0 &&
      countBitsInMask(procMaskToApply & ~(DWORD_PTR)1) >= 1) {
    procMaskToApply &= ~(DWORD_PTR)1;
  }
  if (procMaskToApply != 0) {
    if (SetProcessAffinityMask(GetCurrentProcess(), procMaskToApply)) {
      processShelterMask.store(procMaskToApply, std::memory_order_release);
    }
  }

  // 2. REAL-TIME AUDIO & DSP WORKER THREAD AFFINITY: Lock exclusively to fast
  // P-cores
  //    (excluding CPU 0 when sheltered).
  DWORD_PTR rtDspMask = (engineTuningSettings.pCoresOnly && topo.pCoreMask != 0)
                            ? topo.pCoreMask
                            : fullSysMask;
  if (engineTuningSettings.shelterCpu0 &&
      countBitsInMask(rtDspMask & ~(DWORD_PTR)1) >= 1) {
    rtDspMask &= ~(DWORD_PTR)1;
  }
  rtDspMask &= procMaskToApply;
  if (rtDspMask == 0)
    rtDspMask = procMaskToApply;

  // Count distinct physical cores represented in rtDspMask so we assign at most
  // (physicalCores - 1) parallel DSP helpers (+ Audio Thread = 1 worker per
  // physical core!), and pin the ASIO callback thread and each DSP helper onto
  // separate physical cores so two Priority-15 real-time threads never collide
  // on Hyper-Threads of the same P-core.
  juce::Array<DWORD_PTR> pCoreSlices;
  for (DWORD_PTR coreMask : topo.physicalCoreMasks) {
    const DWORD_PTR slice = coreMask & rtDspMask;
    if (slice != 0)
      pCoreSlices.add(slice);
  }
  int dspPhysicalCores = pCoreSlices.size();
  if (dspPhysicalCores <= 0)
    dspPhysicalCores = juce::jmax(1, countBitsInMask(rtDspMask) / 2);

  const int logicalInDspMask = countBitsInMask(rtDspMask);
  const int helperLimit = (logicalInDspMask >= 2 && dspPhysicalCores >= 2)
                              ? juce::jlimit(1, 4, dspPhysicalCores - 1)
                              : 0;

  DWORD_PTR audioThreadMask = rtDspMask;
  DWORD_PTR primaryHelperMask = rtDspMask;
  if (pCoreSlices.size() >= 2) {
    // Give the ASIO master callback thread the last (unsheltered, full)
    // physical P-core, and assign each DSP helper its own separate physical
    // P-core slice.
    audioThreadMask = pCoreSlices.getLast();
    const int workerPoolCores = pCoreSlices.size() - 1;
    primaryHelperMask = pCoreSlices[0];
    for (int h = 0; h < 4; ++h) {
      engine.setDspWorkerAffinityMask(h, pCoreSlices[h % workerPoolCores]);
    }
  } else {
    for (int h = 0; h < 4; ++h) {
      engine.setDspWorkerAffinityMask(h, 0);
    }
  }

  dspThreadAffinityMask.store(audioThreadMask, std::memory_order_release);
  engine.setActiveDspHelperLimit(helperLimit);
  engine.setPreventPowerThrottling(engineTuningSettings.preventPowerThrottling);
  engine.setShelteredAffinityMask(rtDspMask);

  // 3. BACKGROUND / UI / PRELOADER THREAD AFFINITY & PRIORITY:
  //    On hybrid CPUs with P-Cores Only enabled, steer the UI thread and
  //    background RigBuilder / SetlistPreloader threads onto Efficiency cores
  //    so background song preloads and UI repaints never touch the real-time
  //    P-cores!
  DWORD_PTR bgMask = procMaskToApply;
  if (engineTuningSettings.pCoresOnly && topo.eCoreMask != 0) {
    const DWORD_PTR candidateECores = topo.eCoreMask & procMaskToApply;
    if (countBitsInMask(candidateECores) >= 2)
      bgMask = candidateECores;
  }
  engine.setBackgroundAffinityMask(bgMask);
  thermalMonitor.setBackgroundAffinityMask((uintptr_t)bgMask);

  if (juce::MessageManager::getInstanceWithoutCreating() != nullptr &&
      juce::MessageManager::getInstance()->isThisTheMessageThread()) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_NORMAL);
    if (bgMask != 0)
      SetThreadAffinityMask(GetCurrentThread(), bgMask);
  }

  LOG_INFO("Hybrid Affinity Applied -> Process: 0x" +
           juce::String::toHexString((juce::int64)procMaskToApply) +
           " | Audio Thread: 0x" +
           juce::String::toHexString((juce::int64)audioThreadMask) +
           " | DSP Helper[0]: 0x" +
           juce::String::toHexString((juce::int64)primaryHelperMask) + " (" +
           juce::String(dspPhysicalCores) + " phys P-cores, " +
           juce::String(helperLimit) + " helper + audio thread)" +
           " | UI/Background: 0x" +
           juce::String::toHexString((juce::int64)bgMask));

  // 4. Windows 11 Process EcoQoS (Power Throttling)
  PROCESS_POWER_THROTTLING_STATE powerThrottling{};
  powerThrottling.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
  powerThrottling.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
  powerThrottling.StateMask = engineTuningSettings.preventPowerThrottling
                                  ? 0UL
                                  : PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
  if (SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling,
                            &powerThrottling, sizeof(powerThrottling))) {
    LOG_INFO("Windows 11 EcoQoS Power Throttling set to: " +
             juce::String(engineTuningSettings.preventPowerThrottling
                              ? "Disabled (HighQoS)"
                              : "Default"));
  }
#endif

  engine.setParallelProcessingEnabled(engineTuningSettings.parallelProcessing);
  LOG_INFO("DSP Processing pipeline set to: " +
           juce::String(engineTuningSettings.parallelProcessing
                            ? "Parallel Work-Sharing"
                            : "Sequential Inline"));
}

void MainComponent::resetAudioDevice() {
  suppressGlitchDetectionForMs(2500);
#ifdef _WIN32
  if constexpr (FanfareConstants::kPriorityElevationEnabled) {
    if (GetPriorityClass(GetCurrentProcess()) != ABOVE_NORMAL_PRIORITY_CLASS) {
      SetPriorityClass(GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS);
      LOG_INFO("Restored ABOVE_NORMAL_PRIORITY_CLASS after audio driver init");
    }
  }
#endif
  juce::ScopedValueSetter<bool> recoveryGuard(isRecoveringAudioDevice, true);
  auto *device = deviceManager.getCurrentAudioDevice();
  auto setup =
      (device != nullptr && device->isOpen())
          ? deviceManager.getAudioDeviceSetup()
          : (hasDesiredAudioSetup ? desiredAudioSetup
                                  : deviceManager.getAudioDeviceSetup());
  deviceManager.closeAudioDevice();
  if (desiredAudioDeviceType.isNotEmpty() &&
      deviceManager.getCurrentAudioDeviceType() != desiredAudioDeviceType) {
    deviceManager.setCurrentAudioDeviceType(desiredAudioDeviceType, true);
  }
  deviceManager.setAudioDeviceSetup(setup, true);
  midiOpenFailCounts.clear();
  midiOpenRetryAfterMs.clear();
  syncHotplugMidiInputs(false);
  if (auto *newDev = deviceManager.getCurrentAudioDevice()) {
    if (newDev->isOpen() && newDev->isPlaying()) {
      inAudioRecoveryMode = false;
      audioRecoveryAttemptCount = 0;
    }
  }
  if (false) {
    if (!device)
      return;
    auto setup = deviceManager.getAudioDeviceSetup();
    deviceManager.closeAudioDevice();
    deviceManager.initialise(setup.inputChannels.countNumberOfSetBits(),
                             setup.outputChannels.countNumberOfSetBits(),
                             nullptr, true, "", &setup);
  }
}

void MainComponent::changeListenerCallback(juce::ChangeBroadcaster *source) {
  if (source == &deviceManager) {
    const juce::int64 nowMs = (juce::int64)juce::Time::getMillisecondCounter();
    if (isSyncingMidiInputs || isRecoveringAudioDevice)
      return;
    if (activeAudioSelector == nullptr &&
        nowMs < suppressHotplugChangeBroadcastUntilMs)
      return;
    if (activeAudioSelector == nullptr)
      syncHotplugMidiInputs(false);
    saveAudioSettings();
  } else if (source == &ThemeManager::getInstance()) {
    // Theme changed: re-apply LookAndFeel colours, propagate
    // lookAndFeelChanged() to every ComboBox/Slider/TextEditor/DialogWindow on
    // the desktop, and repaint.
    boutiqueLookAndFeel.applyThemeColors();
    refreshThemeDependentStyles();
    sendLookAndFeelChange();
    auto &desktop = juce::Desktop::getInstance();
    for (int i = 0; i < desktop.getNumComponents(); ++i) {
      if (auto *comp = desktop.getComponent(i)) {
        comp->sendLookAndFeelChange();
        comp->repaint();
      }
    }
    repaint();
    for (auto *c : getChildren())
      c->repaint();
  } else if (source == &Fanfare::SetlistManager::getInstance()) {
    updatePreloadStatus();
    if (webServer)
      webServer->notifySetlistChanged();
  }
}

void MainComponent::audioDeviceAboutToStart(juce::AudioIODevice *device) {
  suppressGlitchDetectionForMs(2500);
#ifdef _WIN32
  if constexpr (FanfareConstants::kPriorityElevationEnabled) {
    if (GetPriorityClass(GetCurrentProcess()) != ABOVE_NORMAL_PRIORITY_CLASS) {
      SetPriorityClass(GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS);
      LOG_INFO("Restored ABOVE_NORMAL_PRIORITY_CLASS after audio driver init");
    }
  }
#endif
  if (device == nullptr || !device->isOpen() ||
      device->getCurrentSampleRate() <= 0.0 ||
      device->getCurrentBufferSizeSamples() <= 0) {
    return;
  }
  midiCollector.reset(device->getCurrentSampleRate());
  engine.prepareToPlay(device->getCurrentBufferSizeSamples(),
                       device->getCurrentSampleRate());
  // Forget the previous device session's last callback time: otherwise the
  // first callback of the new session measures a gap against a stale
  // timestamp and logs a giant false "glitch" (the 800ms log entries).
  lastAudioCallbackTicks.store(0);
}

void MainComponent::audioDeviceStopped() {}

void MainComponent::refreshThemeDependentStyles() {
  const auto &t = ThemeManager::getInstance().active();

  auto gearNorm = createGearImage(24, t.textDim);
  auto gearOver = createGearImage(24, t.accent);
  settingsGearBtn.setImages(false, true, true, gearNorm, 1.0f,
                            juce::Colours::transparentBlack, gearOver, 1.0f,
                            juce::Colours::transparentBlack, gearOver, 0.85f,
                            juce::Colours::transparentBlack);

  setupNameLabel.setColour(juce::Label::textColourId, t.text);
  stripWidthLabel.setColour(juce::Label::textColourId, t.engrave);
  midiMonitorLabel.setColour(juce::Label::textColourId, t.midiNote);
  cpuLabel.setColour(juce::Label::textColourId, t.text);
  ramLabel.setColour(juce::Label::textColourId, t.textDim);
  latencyLabel.setColour(juce::Label::textColourId, t.textDim);
  xrunsLabel.setColour(juce::Label::textColourId,
                       (audioUnderrunCount.load() + engineStallCount.load() > 0)
                           ? t.warn
                           : t.ok);
  thermalLabel.setColour(juce::Label::textColourId, juce::Colours::white);
  clockLabel.setColour(juce::Label::textColourId,
                       t.isStudio() ? t.textDim : t.accent);
  nextUpLabel.setColour(juce::Label::textColourId, t.textDim);
  audioDeviceLabel.setColour(juce::Label::textColourId, t.textDim);
  preloadStatusLabel.setColour(juce::Label::textColourId, t.textDim);
  if (t.isStudio()) {
    cpuLabel.setColour(juce::Label::textColourId, t.textDim);
    midiMonitorLabel.setColour(juce::Label::textColourId, t.textDim);
  }
  freshBuildToggle.setColour(juce::ToggleButton::textColourId, t.textDim);
  autoCollapseToggle.setColour(juce::ToggleButton::textColourId, t.textDim);

  fohLabel.setColour(juce::Label::textColourId, t.foh);
  iemLabel.setColour(juce::Label::textColourId, t.iem);

  for (int i = 0; i < 3; ++i) {
    fohFxBtns[i].getProperties().set("slotAccent", (int)t.foh.getARGB());
    iemFxBtns[i].getProperties().set("slotAccent", (int)t.iem.getARGB());
  }

  updatePreloadStatus();
}

void MainComponent::paint(juce::Graphics &g) {
  const auto &t = ThemeManager::getInstance().active();
  g.fillAll(t.canvas);

  // Top bar
  if (!stageHeaderBounds.isEmpty()) {
    g.setColour(t.isStudio() ? t.panelAlt : t.panel);
    g.fillRect(stageHeaderBounds);
    g.setColour(t.isStudio() ? t.border : t.hairline);
    g.drawHorizontalLine(stageHeaderBounds.getBottom() - 1, 0.0f,
                         (float)getWidth());

    // Song title capsule (Nota-style grouped control)
    if (setupNameLabel.isVisible()) {
      auto cap = prevSetlistBtn.getBounds()
                     .getUnion(nextSetlistBtn.getBounds())
                     .getUnion(setupNameLabel.getBounds())
                     .toFloat()
                     .expanded(3.0f, 1.0f);
      cap = cap.withTop((float)stageHeaderBounds.getY() + 5.0f)
                .withBottom((float)stageHeaderBounds.getBottom() - 6.0f);
      g.setColour(t.isStudio() ? t.panel : t.sunken);
      g.fillRoundedRectangle(cap, 5.0f);
      g.setColour(t.border);
      g.drawRoundedRectangle(cap.reduced(0.5f), 5.0f, 1.0f);
    }

    // Keep white Fanfare emblem legible on light themes
    if (logoComponent.isVisible() && !t.dark) {
      g.setColour(juce::Colour(0xFF1F2428));
      g.fillRoundedRectangle(logoComponent.getBounds().toFloat().expanded(1.0f),
                             3.0f);
    }
  }

  // Scene bar
  if (!sceneRowBounds.isEmpty()) {
    g.setColour(t.isStudio() ? t.panel : t.sunken);
    g.fillRect(sceneRowBounds);
    g.setColour(t.isStudio() ? t.border : t.hairlineStrong);
    g.drawHorizontalLine(sceneRowBounds.getBottom() - 1, 0.0f,
                         (float)getWidth());
  }

  // Status bar
  if (!statusBarBounds.isEmpty()) {
    g.setColour(t.isStudio() ? t.panelAlt : t.panel);
    g.fillRect(statusBarBounds);
    g.setColour(t.isStudio() ? t.border : t.hairline);
    g.drawHorizontalLine(statusBarBounds.getY(), 0.0f, (float)getWidth());
    if (thermalLabel.isVisible() && !thermalLabel.getBounds().isEmpty()) {
      auto tb = thermalLabel.getBounds().toFloat().reduced(1.5f, 1.0f);
      g.setColour(t.panic);
      g.fillRoundedRectangle(tb, 3.0f);
    }
    // thin separators between telemetry fields
    g.setColour(t.isStudio() ? t.border : t.hairline);
    for (auto *lbl : {&cpuLabel, &ramLabel, &latencyLabel})
      if (!lbl->getBounds().isEmpty())
        g.drawVerticalLine(lbl->getRight(),
                           (float)statusBarBounds.getY() + 5.0f,
                           (float)statusBarBounds.getBottom() - 5.0f);
  }

  // Master Console Column Face
  if (!masterColumnBounds.isEmpty()) {
    auto mf = masterColumnBounds.toFloat().reduced(1.0f, 0.0f);
    g.setColour(t.panel);
    g.fillRoundedRectangle(mf, 3.0f);
    g.setColour(t.border);
    g.drawRoundedRectangle(mf.reduced(0.5f), 3.0f, 1.0f);

    // Gold FOH / Cyan IEM engraved bus header bars
    auto fohHdr = fohLabel.getBounds().toFloat().reduced(4.0f, 0.0f);
    auto iemHdr = iemLabel.getBounds().toFloat().reduced(4.0f, 0.0f);
    g.setColour(t.foh);
    g.fillRect(fohHdr.getX(), fohHdr.getBottom() - 2.0f, fohHdr.getWidth(),
               2.0f);
    g.setColour(t.iem);
    g.fillRect(iemHdr.getX(), iemHdr.getBottom() - 2.0f, iemHdr.getWidth(),
               2.0f);
  }

  // Segmented Master FOH & IEM Stereo LED Meters
  auto fohBounds = masterFohSlider.getBounds().toFloat();
  if (!fohBounds.isEmpty()) {
    auto fohMeter = juce::Rectangle<float>(
        fohBounds.getRight() + 2.0f, fohBounds.getY() + 4.0f, 13.0f,
        juce::jmax(16.0f, fohBounds.getHeight() - 8.0f));
    int segs = juce::jlimit(12, 24, (int)(fohMeter.getHeight() / 9.0f));
    BoutiqueLookAndFeel::drawStereoSegmentedMeter(g, fohMeter, masterFohL,
                                                  masterFohR, segs, t.meterLow,
                                                  t.meterMid, t.meterPeak);
  }

  auto iemBounds = masterIemSlider.getBounds().toFloat();
  if (!iemBounds.isEmpty()) {
    auto iemMeter = juce::Rectangle<float>(
        iemBounds.getRight() + 2.0f, iemBounds.getY() + 4.0f, 13.0f,
        juce::jmax(16.0f, iemBounds.getHeight() - 8.0f));
    int segs = juce::jlimit(12, 24, (int)(iemMeter.getHeight() / 9.0f));
    BoutiqueLookAndFeel::drawStereoSegmentedMeter(g, iemMeter, masterIemL,
                                                  masterIemR, segs, t.iem,
                                                  t.meterMid, t.meterPeak);
  }
}

void MainComponent::resized() {
  auto r = getLocalBounds();

  // ===== Studio shell layout (Nov 2026) =====
  // Row 1 (top bar): logo, menu bar, < song title >, update badge, clock, PANIC
  // Row 2 (scene bar): scene tiles, + scene, scene menu
  // Bottom: status bar (MIDI activity, preload status, telemetry)
  // Left: tool rail + context sidebar (Songs / Strips / Setlist / Queue / MIDI)
  // The old queue row lives in the Queue sidebar context now.
  auto topBar = r.removeFromTop(38);
  stageHeaderBounds = topBar;
  auto sh = topBar.reduced(6, 0);

  if (logoComponent.isVisible()) {
    logoComponent.setBounds(sh.removeFromLeft(30).reduced(2, 6));
    sh.removeFromLeft(4);
  }
  if (menuBar) {
    menuBar->setBounds(
        sh.removeFromLeft(juce::jmin(250, sh.getWidth() / 4)).reduced(0, 7));
    sh.removeFromLeft(8);
  }

  // Right side first so the title never pushes PANIC off screen.
  panicBtn.setBounds(sh.removeFromRight(88).reduced(0, 5));
  sh.removeFromRight(20); // keep-clear gap around PANIC
  clockLabel.setBounds(sh.removeFromRight(92).reduced(0, 7));
  if (updateBadgeBtn.isVisible()) {
    sh.removeFromRight(6);
    updateBadgeBtn.setBounds(sh.removeFromRight(104).reduced(0, 7));
  }
  sh.removeFromRight(10);

  prevSetlistBtn.setBounds(sh.removeFromLeft(30).reduced(0, 7));
  sh.removeFromLeft(4);
  const int titleW = juce::jlimit(140, 460, sh.getWidth() - 40);
  setupNameLabel.setBounds(sh.removeFromLeft(titleW).reduced(4, 4));
  sh.removeFromLeft(4);
  nextSetlistBtn.setBounds(sh.removeFromLeft(30).reduced(0, 7));

  // Next-up capsule occupies the remaining center of the top bar
  if (nextUpLabel.isVisible()) {
    const int capW = juce::jmin(320, sh.getWidth());
    nextUpLabel.setBounds(sh.withSizeKeepingCentre(capW, 24).reduced(10, 0));
  } else {
    nextUpLabel.setBounds({});
  }

  // Row 2: scene bar
  auto sceneRow = r.removeFromTop(32);
  sceneRowBounds = sceneRow;
  controlBarBounds = {};
  queueRowBounds = {};
  midiWellBounds = {};
  telemetryWellBounds = {};
  headerBounds = stageHeaderBounds.getUnion(sceneRowBounds);
  {
    auto sr = sceneRow.reduced(6, 4);
    sceneMenuBtn.setBounds(sr.removeFromRight(30));
    sr.removeFromRight(4);
    addSceneBtn.setBounds(sr.removeFromRight(74));
    sr.removeFromRight(8);
    hiddenSceneIndices.clear();
    if (!sceneButtons.isEmpty()) {
      const int n = sceneButtons.size();
      const int minW = 56;
      const int maxW = 180;
      for (int i = 0; i < n; ++i) {
        if (sr.getWidth() < minW) {
          sceneButtons[i]->setVisible(false);
          sceneButtons[i]->setBounds({});
          hiddenSceneIndices.add(i);
          continue;
        }
        sceneButtons[i]->setVisible(true);
        const int fitCount =
            juce::jmax(1, juce::jmin(n - i, sr.getWidth() / minW));
        const int w = juce::jlimit(minW, maxW, sr.getWidth() / fitCount);
        sceneButtons[i]->setBounds(sr.removeFromLeft(w).reduced(2, 0));
      }
    }
    // Scenes that did not fit go into the overflow button instead of vanishing
    if (hiddenSceneIndices.size() > 0) {
      sceneOverflowBtn.setVisible(true);
      sceneOverflowBtn.setBounds(sr.removeFromLeft(30).reduced(2, 0));
    } else {
      sceneOverflowBtn.setVisible(false);
      sceneOverflowBtn.setBounds({});
    }
  }

  // Bottom status bar
  statusBarBounds = r.removeFromBottom(22);
  {
    auto sb = statusBarBounds.reduced(8, 0);
    midiMonitorLabel.setBounds(
        sb.removeFromLeft(juce::jmin(150, sb.getWidth() / 6)));
    sb.removeFromLeft(6);
    audioDeviceLabel.setBounds(
        sb.removeFromLeft(juce::jmin(210, sb.getWidth() / 5)));
    const bool showThermal = thermalLabel.isVisible();
    if (showThermal)
      thermalLabel.setBounds(sb.removeFromRight(110).reduced(0, 2));
    else
      thermalLabel.setBounds({});
    xrunsLabel.setBounds(sb.removeFromRight(118));
    latencyLabel.setBounds(sb.removeFromRight(104));
    ramLabel.setBounds(sb.removeFromRight(168));
    cpuLabel.setBounds(sb.removeFromRight(92));
    sb.removeFromRight(12);
    preloadStatusLabel.setBounds(sb);
  }

  r.removeFromTop(2);

  // Tool rail + context sidebar
  toolRail.setBounds(r.removeFromLeft(44));
  if (sidebarContext >= 0 && libraryPanel) {
    libraryPanel->setBounds(r.removeFromLeft(256));
    libraryPanel->setVisible(true);
    r.removeFromLeft(3);
  } else if (libraryPanel) {
    libraryPanel->setVisible(false);
  }

  // Master Console Column (right side)
  auto masterArea = r.removeFromRight(176);
  masterColumnBounds = masterArea;
  auto ma = masterArea.reduced(4, 3);

  // 1. Dual Analog VU Meters
  auto vuArea = ma.removeFromTop(62);
  auto vuL = vuArea.removeFromLeft(vuArea.getWidth() / 2);
  masterVuMeterL.setBounds(vuL.reduced(2, 1));
  masterVuMeterR.setBounds(vuArea.reduced(2, 1));

  // 2. Engraved Bus Headers
  ma.removeFromTop(2);
  auto labels = ma.removeFromTop(20);
  fohLabel.setBounds(labels.removeFromLeft(labels.getWidth() / 2));
  iemLabel.setBounds(labels);

  // 3. Master FX Slots (3 per bus)
  ma.removeFromTop(2);
  auto fxArea = ma.removeFromTop(64);
  auto fohFxArea = fxArea.removeFromLeft(fxArea.getWidth() / 2);
  auto iemFxArea = fxArea;

  for (int i = 0; i < 3; ++i) {
    auto fRow = fohFxArea.removeFromTop(21).reduced(2, 1);
    fohEditGuiBtns[i].setBounds(fRow.removeFromRight(18));
    fRow.removeFromRight(1);
    fohFxBtns[i].setBounds(fRow);

    auto iRow = iemFxArea.removeFromTop(21).reduced(2, 1);
    iemEditGuiBtns[i].setBounds(iRow.removeFromRight(18));
    iRow.removeFromRight(1);
    iemFxBtns[i].setBounds(iRow);
  }

  // 4. Master Faders (leaving 16px on each bus's right side for its stereo
  // segmented meter)
  ma.removeFromTop(4);
  auto fohFaderCol = ma.removeFromLeft(ma.getWidth() / 2);
  auto iemFaderCol = ma;
  masterFohSlider.setBounds(fohFaderCol.withTrimmedRight(16).reduced(6, 2));
  masterIemSlider.setBounds(iemFaderCol.withTrimmedRight(16).reduced(6, 2));

  // Channel Strips (horizontal rack in center) + pinned Aux Returns on right
  int numMain = rackSlotComponents.size();
  int numReturns = auxReturnComponents.size();

  if (numMain > 0 || numReturns > 0) {
    const int mutedCollapsedWidth = 28;
    const int emptyCollapsedWidth = 30;
    const int expandedWidth = juce::jlimit(kMinExpandedWidth, kMaxExpandedWidth,
                                           preferredExpandedWidth);

    // 1. Pin Aux Returns on the right: collapsed Aux Returns shrink to 28px!
    const int returnExpandedW =
        juce::jlimit(68, 84, juce::jmax(68, r.getWidth() / 11));
    if (numReturns > 0) {
      int returnsTotalW = 0;
      for (auto *c : auxReturnComponents)
        returnsTotalW +=
            c->getIsCollapsed() ? mutedCollapsedWidth : returnExpandedW;

      auto returnsArea =
          r.removeFromRight(juce::jmin(r.getWidth() / 2, returnsTotalW));
      juce::Rectangle<int> lwLogoArea;
      for (auto *c : auxReturnComponents) {
        const int cw =
            c->getIsCollapsed() ? mutedCollapsedWidth : returnExpandedW;
        auto col =
            returnsArea.removeFromLeft(juce::jmin(cw, returnsArea.getWidth()));
        int stripH = juce::jmax(240, col.getHeight() / 2);
        c->setBounds(col.removeFromTop(stripH).reduced(1, 2));
        lwLogoArea = lwLogoArea.isEmpty() ? col : lwLogoArea.getUnion(col);
      }
      lastWaltzLogoComponent.setVisible(lastWaltzLogoImage.isValid() &&
                                        lwLogoArea.getWidth() >= 48 &&
                                        lwLogoArea.getHeight() >= 48);
      if (lastWaltzLogoComponent.isVisible())
        lastWaltzLogoComponent.setBounds(lwLogoArea.reduced(2));
    } else {
      lastWaltzLogoComponent.setVisible(false);
    }

    // 2. When Stage Compact is ON, collapse all unused empty strips into a
    // single
    //    "+ ADD" next-strip tile right after the last active/expanded strip.
    const bool compactOn = autoCollapseToggle.getToggleState();
    int nextEmptyIdx = -1;
    if (compactOn) {
      int lastUsedOrExpandedIdx = 0;
      for (int i = 1; i < rackSlotComponents.size(); ++i) {
        auto *comp = rackSlotComponents[i];
        if (!(comp->getIsCollapsed() && comp->isUnusedEmptyStrip()))
          lastUsedOrExpandedIdx = i;
      }
      for (int i = lastUsedOrExpandedIdx + 1; i < rackSlotComponents.size();
           ++i) {
        auto *comp = rackSlotComponents[i];
        if (comp->getIsCollapsed() && comp->isUnusedEmptyStrip()) {
          nextEmptyIdx = i;
          break;
        }
      }
      if (nextEmptyIdx < 0) {
        for (int i = 1; i < rackSlotComponents.size(); ++i) {
          auto *comp = rackSlotComponents[i];
          if (comp->getIsCollapsed() && comp->isUnusedEmptyStrip()) {
            nextEmptyIdx = i;
            break;
          }
        }
      }
    }

    int totalStripsWidth = 0;
    for (int i = 1; i < rackSlotComponents.size(); ++i) {
      auto *comp = rackSlotComponents[i];
      const bool isEmptyCollapsed =
          comp->getIsCollapsed() && comp->isUnusedEmptyStrip();
      const bool hideExtraEmpty =
          compactOn && isEmptyCollapsed && (i != nextEmptyIdx);
      comp->setVisible(!hideExtraEmpty);
      if (!hideExtraEmpty) {
        const int cw =
            isEmptyCollapsed ? emptyCollapsedWidth : mutedCollapsedWidth;
        totalStripsWidth += comp->getIsCollapsed() ? cw : expandedWidth;
      }
    }
    if (rackSlotComponents.size() > 0) {
      auto *comp = rackSlotComponents[0];
      comp->setVisible(true);
      totalStripsWidth +=
          comp->getIsCollapsed() ? mutedCollapsedWidth : expandedWidth;
    }

    // 3. Dedicated high-contrast bottom navigation bar [ < ] [===] [ > ] when
    // needed
    const bool needsHScroll = totalStripsWidth > r.getWidth();
    if (needsHScroll) {
      auto navRow = r.removeFromBottom(20).reduced(1, 2);
      rackScrollLeftBtn.setBounds(navRow.removeFromLeft(30));
      navRow.removeFromLeft(3);
      rackScrollRightBtn.setBounds(navRow.removeFromRight(30));
      navRow.removeFromRight(3);
      rackHScrollBar.setBounds(navRow);
    }
    rackScrollLeftBtn.setVisible(needsHScroll);
    rackScrollRightBtn.setVisible(needsHScroll);
    rackHScrollBar.setVisible(needsHScroll);

    rackViewport.setBounds(r);
    const int stripAreaH = juce::jmax(120, r.getHeight());
    rackStripContainer.setSize(juce::jmax(r.getWidth(), totalStripsWidth),
                               stripAreaH);

    int xPos = 0;
    for (int i = 1; i < rackSlotComponents.size(); ++i) {
      auto *comp = rackSlotComponents[i];
      if (!comp->isVisible())
        continue;
      const bool isEmptyCollapsed =
          comp->getIsCollapsed() && comp->isUnusedEmptyStrip();
      const int cw =
          isEmptyCollapsed ? emptyCollapsedWidth : mutedCollapsedWidth;
      const int w = comp->getIsCollapsed() ? cw : expandedWidth;
      comp->setBounds(
          juce::Rectangle<int>(xPos, 0, w, stripAreaH).reduced(1, 0));
      xPos += w;
    }

    // Place Monitor In (index 0) after the main channels, adjacent to Aux
    // Returns
    if (rackSlotComponents.size() > 0) {
      auto *comp = rackSlotComponents[0];
      const int w =
          comp->getIsCollapsed() ? mutedCollapsedWidth : expandedWidth;
      comp->setBounds(
          juce::Rectangle<int>(xPos, 0, w, stripAreaH).reduced(1, 0));
      xPos += w;
    }

    // Sync scrollbar range and clamp scroll offset if rack shrank
    rackHScrollBar.setRangeLimits(0.0, (double)rackStripContainer.getWidth(),
                                  juce::dontSendNotification);
    setRackScrollX(rackViewport.getViewPositionX());
  }

  if (channelStripOverlay)
    channelStripOverlay->centreWithSize(980, 520);
  if (midiEffectsOverlay)
    midiEffectsOverlay->centreWithSize(620, 410);
  if (samplerOverlay)
    samplerOverlay->centreWithSize(720, 600);
  if (synthOverlay)
    synthOverlay->updateForHostBounds(getLocalBounds());
  if (fmOverlay)
    fmOverlay->updateForHostBounds(getLocalBounds());
  if (mp3Overlay)
    mp3Overlay->centreWithSize(780, 580);
  if (setupBuilderOverlay)
    setupBuilderOverlay->centreWithSize(400, 480);
  if (resourceInspectorOverlay)
    resourceInspectorOverlay->centreWithSize(740, 520);
  if (rigDoctorOverlay)
    rigDoctorOverlay->centreWithSize(juce::jmin(getWidth() - 28, 920),
                                     juce::jmin(getHeight() - 28, 620));
  if (engineTuningModal)
    engineTuningModal->centreWithSize(juce::jmin(getWidth() - 32, 640),
                                      juce::jmin(getHeight() - 32, 560));
  if (configOverlay)
    configOverlay->centreWithSize(configOverlay->getWidth(),
                                  configOverlay->getHeight());
  if (stageRemoteOverlay)
    stageRemoteOverlay->setBounds(getLocalBounds());
  if (welcomeOverlay)
    welcomeOverlay->setBounds(getLocalBounds());
  if (missingPluginsToast)
    updateMissingPluginsToastBounds();
}

void MainComponent::updateStripCollapseStates() {
  if (!autoCollapseToggle.getToggleState())
    return;

  for (int i = 0; i < rackSlotComponents.size(); ++i) {
    auto *comp = rackSlotComponents[i];
    auto *slot = engine.getSlot(i);
    if (!slot || !comp)
      continue;

    bool hasPlugins = false;
    for (int c = 0; c < 3; ++c) {
      if (slot->getPluginInstance(c) != nullptr ||
          slot->isChainSlotMidiOut(c) || slot->isChainSlotSynth(c) ||
          slot->isChainSlotFm(c)) {
        hasPlugins = true;
        break;
      }
    }
    bool hasInput = (slot->getInputChannelIndex() >= 0);
    bool isBypassed = slot->isBypassed();

    // Active if not muted/bypassed AND has plugins, hardware input, or is the
    // dedicated monitor channel (slot 0)
    bool isActive = !isBypassed && (hasPlugins || hasInput || i == 0);

    comp->setCollapsed(!isActive);
  }

  resized();
}

void MainComponent::noteMidiStatus(const juce::MidiMessage &msg) {
  // Transport/realtime floods (clock, active sense) would drown real events.
  if (msg.isMidiClock() || msg.isActiveSense() || msg.isMidiStart() ||
      msg.isMidiStop() || msg.isMidiContinue() || msg.isSongPositionPointer() ||
      msg.isQuarterFrame())
    return;

  juce::String text =
      "MIDI: Ch" + juce::String(juce::jlimit(1, 16, msg.getChannel()));
  if (msg.isNoteOn())
    text += " " +
            juce::MidiMessage::getMidiNoteName(msg.getNoteNumber(), true, false,
                                               3) +
            " v" + juce::String(msg.getVelocity());
  else if (msg.isNoteOff())
    text += " " +
            juce::MidiMessage::getMidiNoteName(msg.getNoteNumber(), true, false,
                                               3) +
            " off";
  else if (msg.isController())
    text += " CC" + juce::String(msg.getControllerNumber()) + "=" +
            juce::String(msg.getControllerValue());
  else if (msg.isProgramChange())
    text += " PC" + juce::String(msg.getProgramChangeNumber());
  else if (msg.isPitchWheel())
    text += " Bend";
  else if (msg.isAftertouch())
    text += " AT" + juce::String(msg.getNoteNumber());
  else if (msg.isChannelPressure())
    text += " Press " + juce::String(msg.getChannelPressureValue());

  const juce::SpinLock::ScopedLockType sl(midiStatusLock);
  midiStatusText.swapWith(text);
}

void MainComponent::timerCallback() {
  // Left status segment: last MIDI event, drained from the MIDI-thread stash.
  if (juce::String midiText = takeMidiStatusText(); midiText.isNotEmpty())
    midiMonitorLabel.setText(midiText, juce::dontSendNotification);

  double cpu = deviceManager.getCpuUsage() * 100.0;
  thermalMonitor.setAudioCpuLoadPercent(cpu);
  cpuLabel.setText("CPU: " + juce::String(cpu, 1) + "%",
                   juce::dontSendNotification);
  // Threshold colours (nov8mistral.md 1.6): amber > 70%, red > 90%
  cpuLabel.setColour(juce::Label::textColourId,
                     cpu > 90.0   ? ThemeManager::get(Theme::Role::danger)
                     : cpu > 70.0 ? ThemeManager::get(Theme::Role::warn)
                                  : ThemeManager::get(Theme::Role::textDim));

  static int memUpdateTicks = 19;
  if (++memUpdateTicks >= 20) {
    memUpdateTicks = 0;
#ifdef _WIN32
    if constexpr (FanfareConstants::kPriorityElevationEnabled) {
      const DWORD curPri = GetPriorityClass(GetCurrentProcess());
      if (curPri != 0 && curPri != ABOVE_NORMAL_PRIORITY_CLASS) {
        SetPriorityClass(GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS);
        LOG_INFO("Driver/plugin altered PriorityClass (0x" +
                 juce::String::toHexString((int)curPri) +
                 ") - restored ABOVE_NORMAL_PRIORITY_CLASS");
      }
    }
#endif
    FanfareLog::flushLog();
    checkAndRecoverAudioDevice();
    auto memStats = FanfareLog::getMemoryStats();
    juce::String appRamStr =
        ResourceInspectorModal::formatBytes(memStats.workingSetBytes);
    ramLabel.setText("RAM: " + appRamStr + " (" +
                         juce::String(memStats.systemRamLoadPercent) + "%)",
                     juce::dontSendNotification);

    const auto thermSnap = thermalMonitor.getSnapshot();
    if (thermSnap.tempCelsius > 0) {
      cpuLabel.setTooltip(
          "CPU Temp: " + juce::String(thermSnap.tempCelsius) + " C (Peak " +
          juce::String(thermSnap.peakTempCelsius) +
          " C) | Click to view Per-VST Memory & Resource Inspector");
    }
    const bool wasThermalVis = thermalLabel.isVisible();
    if (thermSnap.warningActive) {
      thermalLabel.setText(thermSnap.badgeText, juce::dontSendNotification);
      thermalLabel.setTooltip("CPU THROTTLED: " + thermSnap.reasonText +
                              " (Click for details)");
    }
    if (wasThermalVis != thermSnap.warningActive) {
      thermalLabel.setVisible(thermSnap.warningActive);
      resized();
    }
  }

  // Round-trip latency readout (input + output buffer latency)
  if (auto *dev = deviceManager.getCurrentAudioDevice()) {
    double sr = dev->getCurrentSampleRate();
    int inL = dev->getInputLatencyInSamples();
    int outL = dev->getOutputLatencyInSamples();
    const int buf = dev->getCurrentBufferSizeSamples();
    double ms = 0.0;
    if (sr > 0.0) {
      if (inL + outL > 0)
        ms = (inL + outL) / sr * 1000.0;
      else
        ms = buf * 2.0 / sr * 1000.0;
    }
    latencyLabel.setText(inAudioRecoveryMode
                             ? "LAT: RECONNECT..."
                             : ("LAT: " + juce::String(ms, 1) + "ms"),
                         juce::dontSendNotification);
    // LAT beyond one buffer period deserves a glance (nov8mistral.md 1.6)
    const double bufferMs = (sr > 0.0) ? buf / sr * 1000.0 : 0.0;
    latencyLabel.setColour(
        juce::Label::textColourId,
        (!inAudioRecoveryMode && bufferMs > 0.0 && ms > bufferMs)
            ? ThemeManager::get(Theme::Role::warn)
            : ThemeManager::get(Theme::Role::textDim));
    audioDeviceLabel.setText(dev->getName() + " | " +
                                 juce::String(sr / 1000.0, 1) + " kHz | " +
                                 juce::String(buf) + " smp",
                             juce::dontSendNotification);
    audioDeviceLabel.setColour(juce::Label::textColourId,
                               ThemeManager::get(Theme::Role::textDim));
  } else {
    latencyLabel.setText(inAudioRecoveryMode ? "LAT: RECONNECT..." : "LAT: --",
                         juce::dontSendNotification);
    audioDeviceLabel.setText("audio: no device", juce::dontSendNotification);
    audioDeviceLabel.setColour(juce::Label::textColourId,
                               ThemeManager::get(Theme::Role::danger));
  }

  // Surface any audio underruns: callback-gap count + engine-side stalls,
  // tallied separately so the meter matches what the ears hear while the
  // two pathways stay distinguishable.
  int xruns = audioUnderrunCount.load();
  int stalls = engineStallCount.load();
  if (xruns + stalls > 0) {
    juce::String txt = "XRUNS: " + juce::String(xruns + stalls);
    if (stalls > 0)
      txt += " (" + juce::String(xruns) + " gap / " + juce::String(stalls) +
             " stall)";
    txt += " [" + juce::String(lastGlitchStallMs.load(), 1) + "ms]";
    xrunsLabel.setText(txt, juce::dontSendNotification);
    xrunsLabel.setColour(juce::Label::textColourId,
                         ThemeManager::get(Theme::Role::warn));
  } else {
    xrunsLabel.setText("XRUNS: 0", juce::dontSendNotification);
    xrunsLabel.setColour(juce::Label::textColourId,
                         ThemeManager::get(Theme::Role::ok));
  }

  if (engine.consumeAudioUnderrun()) {
    // Engine-side stall (worker didn't finish, rack mid-transition): the block
    // was genuinely silent - count it in XRUNS so the meter matches the ears,
    // but tally it separately from callback-gap underruns.
    engineStallCount++;
    FanfareLog::log(FanfareLog::Level::Warning,
                    "Audio underrun: a block emitted silence "
                    "(worker stall or rack not prepared).");
  }

  if (hasPendingGlitchLog.exchange(false)) {
    double actual = pendingGlitchActualMs.exchange(0.0);
    double expected = pendingGlitchExpectedMs.exchange(0.0);
    double cbDur = pendingGlitchDurationMs.exchange(0.0);
    if (actual > 0.0) {
      logAudioGlitch(actual, expected, cbDur);
    }
  }

  // Flush batched glitch-log appends at most once per second (this timer's
  // cadence). During a storm the entries pile up in memory and land as one
  // file append instead of one open/write per glitch.
  flushGlitchLog();

  // Update live performance clock
  auto now = juce::Time::getCurrentTime();
  clockLabel.setText(now.formatted("%I:%M:%S %p"), juce::dontSendNotification);

  // Sync Master FX buttons
  for (int i = 0; i < 3; ++i) {
    auto fohName = engine.getMasterPluginName(true, i);
    fohFxBtns[i].getProperties().set("hasPlugin", fohName.isNotEmpty());
    fohFxBtns[i].setButtonText(fohName.isEmpty() ? "FOH " + juce::String(i + 1)
                                                 : fohName);

    auto iemName = engine.getMasterPluginName(false, i);
    iemFxBtns[i].getProperties().set("hasPlugin", iemName.isNotEmpty());
    iemFxBtns[i].setButtonText(iemName.isEmpty() ? "IEM " + juce::String(i + 1)
                                                 : iemName);
  }

  // Update master meters & analog VU meters
  masterVuMeterL.setLevel(engine.getFohPeakL());
  masterVuMeterR.setLevel(engine.getFohPeakR());

  float decay = 0.85f;
  masterFohL = std::max(FanfareLog::amplitudeToLogScale(engine.getFohPeakL()),
                        masterFohL * decay);
  masterFohR = std::max(FanfareLog::amplitudeToLogScale(engine.getFohPeakR()),
                        masterFohR * decay);
  masterIemL = std::max(FanfareLog::amplitudeToLogScale(engine.getIemPeakL()),
                        masterIemL * decay);
  masterIemR = std::max(FanfareLog::amplitudeToLogScale(engine.getIemPeakR()),
                        masterIemR * decay);

  // Repaint header (labels, FX buttons) AND master column (VU meters).
  repaint(headerBounds);
  if (!masterColumnBounds.isEmpty())
    repaint(masterColumnBounds);
}

void MainComponent::mouseDown(const juce::MouseEvent &e) {
  if (e.originalComponent == &logoComponent) {
    showAboutDialog();
    return;
  }
  if (e.originalComponent == &lastWaltzLogoComponent) {
    if (e.mods.isRightButtonDown())
      resetAuxLogoImage();
    else
      chooseAuxLogoImage();
    return;
  }
  if (e.originalComponent == &ramLabel || e.originalComponent == &cpuLabel) {
    showResourceInspectorModal();
    return;
  }
  if (e.originalComponent == &xrunsLabel) {
    audioUnderrunCount.store(0);
    engineStallCount.store(0);
    auto appData =
        juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
            .getChildFile("Fanfare");
    auto logFile = appData.getChildFile("audio_underrun_log.txt");
    if (!logFile.existsAsFile())
      logFile.replaceWithText("No audio underruns detected yet.\n");
    logFile.startAsProcess();
    return;
  }
  if (e.originalComponent == &nextUpLabel ||
      e.originalComponent == &preloadStatusLabel) {
    if (nextSetlistBtn.onClick)
      nextSetlistBtn.onClick();
    return;
  }
  if (e.originalComponent == &midiMonitorLabel) {
    setSidebarContext(sidebarContext == Fanfare::ToolRail::Midi
                          ? -1
                          : Fanfare::ToolRail::Midi);
    return;
  }
  if (e.originalComponent == &audioDeviceLabel) {
    if (audioSettingsBtn.onClick)
      audioSettingsBtn.onClick();
    return;
  }
  if (e.originalComponent == &thermalLabel) {
    const auto snap = thermalMonitor.getSnapshot();
    thermalMonitor.acknowledgeLatch();
    const bool stillActive = thermalMonitor.isWarningActive();
    if (!stillActive && thermalLabel.isVisible()) {
      thermalLabel.setVisible(false);
      resized();
    }

    juce::String msg;
    msg << "Status: "
        << (snap.activelyThrottled ? "ACTIVELY THROTTLED RIGHT NOW"
                                   : "Recent throttle event (clearing latch)")
        << "\n";
    msg << "Reason: " << snap.reasonText << "\n\n";
    if (snap.tempCelsius > 0) {
      msg << "CPU Temperature: " << snap.tempCelsius
          << " C  (Session Peak: " << snap.peakTempCelsius << " C)\n";
    }
    if (snap.effectiveMhz > 0) {
      msg << "Effective Clock: ~" << snap.effectiveMhz << " MHz\n";
    }
    if (snap.osTurboCapMhz > 0) {
      msg << "OS Power Plan Turbo Cap: " << snap.osTurboCapMhz << " MHz\n";
    } else if (snap.nominalMaxMhz > 0 && snap.minMhzLimit > 0) {
      msg << "Firmware Clock Ceiling: " << snap.minMhzLimit << " / "
          << snap.nominalMaxMhz << " MHz base\n";
    }
    msg << "\nStage Cooling Checklist:\n"
        << "- Keep bottom intake vents clear (never sit flat on padded/nylon "
           "case lids or cloth).\n"
        << "- Prop the back edge of the laptop up 0.5 - 1 inch for "
           "under-chassis airflow.\n"
        << "- Plug AC power directly into the laptop (avoid "
           "unpowered/low-wattage USB-C hubs).";

    juce::AlertWindow::showMessageBoxAsync(
        juce::MessageBoxIconType::WarningIcon,
        "CPU Thermal / Firmware Throttle Monitor", msg);
    return;
  }
}

void MainComponent::setSidebarContext(int ctx) {
  sidebarContext = ctx;
  if (ctx == Fanfare::ToolRail::Midi && libraryPanel != nullptr) {
    if (!midiMonitorPanel)
      midiMonitorPanel = std::make_unique<MidiMonitorComponent>();
    libraryPanel->setMidiMonitorContent(midiMonitorPanel.get());
  }
  midiMonitorVisible = (ctx == Fanfare::ToolRail::Midi);
  if (ctx >= 0 && libraryPanel)
    libraryPanel->setContext((LibraryPanel::Context)ctx);
  toolRail.setActiveContext(ctx);
  resized();
  repaint();
}

void MainComponent::onToolRail(int tool) {
  using R = Fanfare::ToolRail;
  if (tool < R::kNumContexts) {
    setSidebarContext(sidebarContext == tool ? -1 : tool);
    return;
  }
  switch (tool) {
  case R::Notepad:
    showGigNotepadWindow();
    break;
  case R::Inspector:
    showResourceInspectorModal();
    break;
  case R::Remote:
    showWebRemoteDialog();
    break;
  case R::Help:
    Fanfare::openHelpManual("#main-mixer");
    break;
  case R::Settings:
    showConfigOverlay();
    break;
  default:
    break;
  }
}
