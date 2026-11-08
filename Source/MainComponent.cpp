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
#include "HelpLauncher.h"
#include "MainComponent.h"
#include "NoteRangeComponent.h"
#include "RackSlotComponent.h"
#include "SaveHistory.h"
#include "ThemeManager.h"
#include "FanfareLogoData.h"
// Optional personal gig art. The file is deliberately gitignored - public
// checkouts build without it and fall back to the committed OpenRig logo.
#if __has_include("LastWaltzLogoData.h")
  #include "LastWaltzLogoData.h"
  #define OPENRIG_HAS_LW_LOGO 1
#else
  #define OPENRIG_HAS_LW_LOGO 0
#endif

namespace {
// Procedurally renders a gear icon for the settings ImageButton (no asset dep).
juce::Image createGearImage(int size, juce::Colour colour) {
  juce::Image img(juce::Image::ARGB, size, size, true);
  juce::Graphics g(img);
  g.setColour(colour);

  const float cx = size * 0.5f;
  const float cy = size * 0.5f;
  const float bodyR = size * 0.34f;
  const float toothR = size * 0.46f;
  const int teeth = 8;

  juce::Path gear;
  for (int i = 0; i < teeth; ++i) {
    float a0 = juce::MathConstants<float>::twoPi * (i + 0.08f) / teeth;
    float a1 = juce::MathConstants<float>::twoPi * (i + 0.42f) / teeth;
    float a2 = juce::MathConstants<float>::twoPi * (i + 0.5f) / teeth;
    float a3 = juce::MathConstants<float>::twoPi * (i + 0.92f) / teeth;
    auto p = [&](float ang, float r) {
      gear.lineTo(cx + r * std::cos(ang), cy + r * std::sin(ang));
    };
    if (i == 0)
      gear.startNewSubPath(cx + bodyR * std::cos(a0), cy + bodyR * std::sin(a0));
    p(a0, bodyR);
    p(a1, toothR);
    p(a2, toothR);
    p(a3, bodyR);
  }
  gear.closeSubPath();
  g.fillPath(gear);

  // Center hole (drawn in panel colour to read as a hole on the dark header)
  g.setColour(ThemeManager::get(Theme::Role::background));
  g.fillEllipse(cx - size * 0.16f, cy - size * 0.16f, size * 0.32f,
                size * 0.32f);
  return img;
}
} // namespace

// Self-deleting window for plugin editors
// Windows managed by MainComponent
class PluginWindow : public juce::DocumentWindow {
public:
  std::function<void()> onWindowClosed;
  juce::AudioPluginInstance* pluginInstance = nullptr;

  PluginWindow(const juce::String &name, juce::Component *content, juce::AudioPluginInstance* pi)
      : DocumentWindow(name, ThemeManager::get(Theme::Role::panel),
                       DocumentWindow::closeButton |
                           DocumentWindow::minimiseButton),
        pluginInstance(pi) {
    setContentOwned(content, true);
    setResizable(true, false);
    setUsingNativeTitleBar(true);
    centreWithSize(content->getWidth(), content->getHeight());
    setVisible(true);
    toFront(true);
  }

  // Tear down the hosted VST3 IPlugView / AudioProcessorEditor synchronously
  // while both the parent DocumentWindow HWND and the owning AudioPluginInstance
  // are still alive (avoiding use-after-free when unloading plugins).
  void destroyEditorNow() {
    if (getContentComponent() != nullptr) {
      FanfareLog::safeExecutePluginCall([this]() {
        clearContentComponent();
      }, "PluginEditor destructor");
    }
    setVisible(false);
    pluginInstance = nullptr;
  }

  ~PluginWindow() override {
    destroyEditorNow();
  }

  void closeButtonPressed() override {
    if (onWindowClosed)
      onWindowClosed();
  }

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PluginWindow)
};

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
  auto* info = reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buffer.data());
  if (!GetLogicalProcessorInformationEx(RelationProcessorCore, info, &length))
    return topo;

  BYTE maxEfficiency = 0;
  DWORD offset = 0;
  while (offset < length) {
    auto* cur = reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buffer.data() + offset);
    if (cur->Relationship == RelationProcessorCore) {
      if (cur->Processor.EfficiencyClass > maxEfficiency)
        maxEfficiency = cur->Processor.EfficiencyClass;
    }
    offset += cur->Size;
  }

  offset = 0;
  while (offset < length) {
    auto* cur = reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buffer.data() + offset);
    if (cur->Relationship == RelationProcessorCore && cur->Processor.GroupCount > 0) {
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
    avSetMmThreadFn = (PAvSetMmThreadCharacteristicsA)GetProcAddress(avrtModule, "AvSetMmThreadCharacteristicsA");
  }

  suppressGlitchDetectionForMs(3000);

  if constexpr (FanfareConstants::kPriorityElevationEnabled) {
  // Elevate process to ABOVE_NORMAL_PRIORITY_CLASS (Base Priority 10) rather than
  // HIGH_PRIORITY_CLASS (Base 13). In HIGH_PRIORITY_CLASS, internal background threads
  // spawned inside heavy VST3s (Kontakt 8, Roland ZENOLOGY/XV-5080) get boosted to
  // Kernel Priority 15, tying and preempting DSP worker threads! In ABOVE_NORMAL,
  // Fanfare still beats normal Windows apps (8), plugin background threads cap at 12,
  // DSP helpers run undisturbed at THREAD_PRIORITY_TIME_CRITICAL (15), and ASIO runs at 24.
  if (!SetPriorityClass(GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS)) {
    LOG_WARN("Failed to set ABOVE_NORMAL_PRIORITY_CLASS for process");
  } else {
    LOG_INFO("Process set to ABOVE_NORMAL_PRIORITY_CLASS (clean 10 vs 15 vs 24 priority tiering)");
  }
  }

  loadEngineTuningSettings();
  applyEngineTuningSettings();

  if constexpr (FanfareConstants::kWorkingSetLockEnabled) {
  // Lock a generous working set minimum so Windows cannot page out VST sample
  // libraries when other applications demand RAM. The 2 GB max we used before
  // Oct 2026 starved massive rigs (many Kontakt multis) during state restore -
  // Windows kept trimming sample data, making loads take minutes. 8 GB covers
  // the biggest setups; the OS still trims beyond it if RAM is truly short.
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

  LOG_INFO(juce::String("Bisect build flags: mp3=") +
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
  Fanfare::RigSerializer::ensureLibraryLayout();

  // Async rig transitioner (Pillar B: trustworthy, non-freezing song switches)
  transitioner = std::make_unique<Fanfare::RigTransitioner>(engine);

  // Listen for audio/MIDI/setlist changes
  deviceManager.addChangeListener(this);
  Fanfare::SetlistManager::getInstance().addChangeListener(this);

  // Load persisted theme before applying the look and feel so the LnF reflects
  // the user's saved choice on first paint.
  ThemeManager::getInstance().loadFromSettings();
  ThemeManager::getInstance().addChangeListener(this);
  Fanfare::FanfareSplashScreen::reportProgress(0.32f, "Opening audio & MIDI devices...");
  loadAudioSettings();

  // Apply look and feel
  setLookAndFeel(&boutiqueLookAndFeel);
  juce::LookAndFeel::setDefaultLookAndFeel(&boutiqueLookAndFeel);
  boutiqueLookAndFeel.applyThemeColors();

  // Preload skeuomorphic assets for performance
  AssetLoader::getInstance().preloadAssets();

  // Setup UI components (extracted for readability)
  Fanfare::FanfareSplashScreen::reportProgress(0.56f, "Building channel strips & buses...");
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
  libraryPanel->onSetDoubleClicked = [this](const juce::File &file) {
    loadSetFile(file);
  };
  if (auto* slp = libraryPanel->getSetlistPanel()) {
    slp->onAddCurrentRequested = [this] {
      juce::String currentSetupName = setupNameLabel.getText();
      juce::File file = Fanfare::RigLibrary::getSongsDirectory().getChildFile(currentSetupName + ".json");
      if (file.existsAsFile()) {
        Fanfare::SetlistManager::getInstance().addSetup(file);
      } else {
        juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::InfoIcon, "Setlist", "Please save the current song first before adding it to the setlist.");
      }
    };
    slp->onLoadSetupRequested = [this](const juce::File &file, int row) {
      loadRigFromFile(file, row);
    };
  }

  Fanfare::FanfareSplashScreen::reportProgress(0.78f, "Preparing Studio workspace...");
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
    updateChecker.checkOnceAsync([safeThis](const UpdateChecker::ReleaseInfo &info) {
      if (safeThis == nullptr)
        return;
      safeThis->latestReleaseUrl = info.htmlUrl;
      juce::String label = "UPDATE v" + juce::String(info.major) + "." + juce::String(info.minor);
      safeThis->updateBadgeBtn.setButtonText(label);
      safeThis->updateBadgeBtn.setTooltip(
          "New Fanfare milestone release available on GitHub (" + info.tagName +
          "). Click to view release notes and download.");
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
  Fanfare::FanfareSplashScreen::reportProgress(0.90f, "Starting stage remote & telemetry...");
  webServer = std::make_unique<OpenRig::WebServer>(engine, this);
  webServer->startServer(8080);
  hotspotController = std::make_unique<OpenRig::StageHotspotController>();
  engine.onNotesChanged = [this]() {
    if (webServer) webServer->notifyNotesChanged();
  };

  if (auto* mp3 = engine.getMp3Player()) {
    mp3->onPlayStarted = [this]() {
      if (webServer) webServer->notifyMp3Changed();
    };
    mp3->onTrackChanged = [this]() {
      if (webServer) webServer->notifyMp3Changed();
    };
    mp3->onPlaylistChanged = [this]() {
      if (webServer) webServer->notifyMp3Changed();
    };
    mp3->onBanksChanged = [this]() {
      if (webServer) webServer->notifyMp3Changed();
    };
  }

  LOG_INFO("OpenRig initialized successfully");
  setWantsKeyboardFocus (true);
  addKeyListener (this);

  // Startup Welcome window (respects "Show on startup" toggle; includes 1-click
  // VST3 scan when no plugins have been scanned yet).
  if (Fanfare::WelcomePrefs::getShowOnStartup()) {
    juce::Timer::callAfterDelay(520, [safeThis = juce::Component::SafePointer<MainComponent>(this)]() {
      if (safeThis != nullptr)
        safeThis->showWelcomeModal();
    });
  } else if (!engine.hasEverScannedPlugins()) {
    juce::Timer::callAfterDelay(650, [safeThis = juce::Component::SafePointer<MainComponent>(this)]() {
      if (safeThis != nullptr && !safeThis->engine.hasEverScannedPlugins()) {
        safeThis->showQuickStartDialog(true);
      }
    });
  }
}

MainComponent::~MainComponent() {
  if (menuBar)
    menuBar->setModel(nullptr);
  rackHScrollBar.removeListener(&rackScrollListener);
  if (webServer)
    webServer->stopServer();
  removeKeyListener (this);
  stopTimer();
  updateChecker.stop();
  thermalMonitor.stopMonitoring();
  flushGlitchLog(); // land any batched glitch entries before we vanish
  if (transitioner)
    transitioner->stopTransition();
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
    SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED | ES_DISPLAY_REQUIRED);
  LOG_INFO("Windows system sleep & display sleep disabled for live audio performance");

  // 2. Check battery vs AC power status
  SYSTEM_POWER_STATUS sps;
  if (GetSystemPowerStatus(&sps)) {
    if (sps.ACLineStatus == 0) { // 0 = Offline (Running on Battery)
      int pct = (sps.BatteryLifePercent != 255) ? (int)sps.BatteryLifePercent : -1;
      LOG_WARN("System is running on BATTERY (" + (pct >= 0 ? juce::String(pct) + "%" : "unknown charge") +
               "). Connecting AC power is strongly recommended for live performance.");

      juce::Timer::callAfterDelay(1500, [pct] {
        juce::String msg = "The laptop is currently running on BATTERY" +
                           (pct >= 0 ? " (" + juce::String(pct) + "% remaining).\n\n" : ".\n\n") +
                           "Windows aggressively throttles CPU clock speeds on battery power, which can cause " +
                           "audio buffer underruns, clicks, and dropouts during live performance.\n\n" +
                           "Please plug in AC mains power.";
        juce::AlertWindow::showMessageBoxAsync(
            juce::MessageBoxIconType::WarningIcon,
            "AC Power Disconnected",
            msg,
            "OK");
      });
    } else if (sps.ACLineStatus == 1) {
      LOG_INFO("AC power verified: plugged into mains.");
    }
  }

  // 3. Automatically force Windows into High Performance power mode
  HMODULE powrprof = LoadLibraryA("powrprof.dll");
  if (powrprof != nullptr) {
    auto pfnGet = (DWORD(WINAPI*)(HKEY, GUID**))GetProcAddress(powrprof, "PowerGetActiveScheme");
    auto pfnSet = (DWORD(WINAPI*)(HKEY, const GUID*))GetProcAddress(powrprof, "PowerSetActiveScheme");

    if (pfnGet != nullptr && pfnSet != nullptr) {
      GUID* activeGuid = nullptr;
      if (pfnGet(NULL, &activeGuid) == ERROR_SUCCESS && activeGuid != nullptr) {
        previousPowerSchemeGuid = *activeGuid;
        previousPowerSchemeSaved = true;

        static const GUID kHighPerfGuid = { 0x8c5e7fda, 0xe8bf, 0x4a96, { 0x9a, 0x85, 0xa6, 0xe2, 0x3a, 0x8c, 0x63, 0x5c } };
        static const GUID kUltimatePerfGuid = { 0xe9a42b02, 0xd5df, 0x448d, { 0xaa, 0x00, 0x03, 0xf1, 0x47, 0x49, 0xeb, 0x61 } };

        if (*activeGuid != kHighPerfGuid && *activeGuid != kUltimatePerfGuid) {
          LOG_INFO("Active power scheme is not High Performance. Activating High Performance scheme...");
          if (pfnSet(NULL, &kHighPerfGuid) == ERROR_SUCCESS) {
            powerPlanForcedHigh = true;
            LOG_INFO("Successfully activated Windows High Performance power scheme.");
          } else {
            WinExec("powercfg /setactive 8c5e7fda-e8bf-4a96-9a85-a6e23a8c635c", SW_HIDE);
            powerPlanForcedHigh = true;
            LOG_INFO("Dispatched powercfg /setactive High Performance.");
          }
        } else {
          LOG_INFO("Windows is already running in High/Ultimate Performance power scheme.");
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
      auto pfnSet = (DWORD(WINAPI*)(HKEY, const GUID*))GetProcAddress(powrprof, "PowerSetActiveScheme");
      if (pfnSet != nullptr) {
        pfnSet(NULL, &previousPowerSchemeGuid);
        LOG_INFO("Restored original Windows power scheme on application exit.");
      }
      FreeLibrary(powrprof);
    }
  }
}
#endif

void MainComponent::setupSlotComponents() {
  // Build input selector data
  std::vector<std::pair<juce::String, int>> inputItems;
  inputItems.push_back({"No Input", -1});
  auto *device = deviceManager.getCurrentAudioDevice();
  if (false) {

  juce::ScopedValueSetter<bool> recoveryGuard(isRecoveringAudioDevice, true);
  auto *device = deviceManager.getCurrentAudioDevice();
  auto setup = (device != nullptr && device->isOpen())
                   ? deviceManager.getAudioDeviceSetup()
                   : (hasDesiredAudioSetup ? desiredAudioSetup : deviceManager.getAudioDeviceSetup());
  deviceManager.closeAudioDevice();
  if (desiredAudioDeviceType.isNotEmpty() &&
      deviceManager.getCurrentAudioDeviceType() != desiredAudioDeviceType) {
    deviceManager.setCurrentAudioDeviceType(desiredAudioDeviceType, true);
  }
  deviceManager.setAudioDeviceSetup(setup, true);
  syncHotplugMidiInputs(false);
  if (auto *newDev = deviceManager.getCurrentAudioDevice()) {
    if (newDev->isOpen() && newDev->isPlaying()) {
      inAudioRecoveryMode = false;
      audioRecoveryAttemptCount = 0;
    }
  }
  if (false) {
  } }
  if (device != nullptr) {
    auto inputs = device->getInputChannelNames();
    for (int hw = 0; hw < inputs.size(); ++hw) {
      inputItems.push_back(
          {"In " + juce::String(hw + 1) + ": " + inputs[hw], hw});
    }
  }

  rackStripContainer.setOpaque(false);
  rackViewport.setViewedComponent(&rackStripContainer, false);
  rackViewport.setScrollBarsShown(false, false);
  rackViewport.onVisibleAreaChanged = [this](const juce::Rectangle<int> &vis) {
    rackHScrollBar.setCurrentRange((double)vis.getX(), (double)vis.getWidth(),
                                   juce::dontSendNotification);
  };
  addAndMakeVisible(rackViewport);

  rackScrollListener.owner = this;
  rackHScrollBar.setAutoHide(false);
  rackHScrollBar.addListener(&rackScrollListener);
  addChildComponent(rackHScrollBar);

  Fanfare::setButtonRole(rackScrollLeftBtn, Fanfare::ButtonRole::Standard);
  rackScrollLeftBtn.setTooltip("Scroll channel strips left (Tip: mousewheel over strip headers also pans horizontally)");
  rackScrollLeftBtn.setRepeatSpeed(280, 75);
  rackScrollLeftBtn.onClick = [this] {
    scrollRackBy(-juce::jmax(160, preferredExpandedWidth * 2));
  };
  addChildComponent(rackScrollLeftBtn);

  Fanfare::setButtonRole(rackScrollRightBtn, Fanfare::ButtonRole::Standard);
  rackScrollRightBtn.setTooltip("Scroll channel strips right (Tip: mousewheel over strip headers also pans horizontally)");
  rackScrollRightBtn.setRepeatSpeed(280, 75);
  rackScrollRightBtn.onClick = [this] {
    scrollRackBy(juce::jmax(160, preferredExpandedWidth * 2));
  };
  addChildComponent(rackScrollRightBtn);

  for (int i = 0; i < engine.getNumSlots(); ++i) {
    auto *slot = engine.getSlot(i);
    auto *comp = new RackSlotComponent(*slot, i, boutiqueLookAndFeel);

    comp->getSlotName = [this](int idx) {
      if (auto* s = engine.getSlot(idx))
        return s->getName();
      return juce::String();
    };

    comp->onRename = [this]() {
      saveButtonMappings();
      for (auto* c : rackSlotComponents) {
        c->updateOutputSelector();
      }
    };

    // Populate Input Selector
    auto &selector = comp->getInputSelector();
    selector.clear();
    for (const auto &item : inputItems) {
      selector.addItem(item.first, item.second + 2);
    }
    selector.setSelectedId(slot->getInputChannelIndex() + 2,
                           juce::dontSendNotification);

    // Specialized modes
    bool isMonitorIn = (i == 0); // First slot is Monitor In
    bool isAccordion = (slot->getName() == "Accordion");
    comp->setSpecialModes(isMonitorIn, isAccordion, false);

    // Plugin selection menu callback
    comp->onShowPluginMenu = [this, i, slot, comp](int chainIndex) {
      juce::PopupMenu menu;
      menu.addSectionHeader("Select Plugin for Chain " +
                            juce::String(chainIndex + 1));
      if (engine.getNumAvailablePlugins() == 0) {
        menu.addItem(-3, "Scan for VST3 Plugins...");
      } else {
        for (int p = 0; p < engine.getNumAvailablePlugins(); ++p)
          menu.addItem(p + 1, engine.getAvailablePluginName(p));
      }
      menu.addSeparator();
      menu.addItem(-2, "Send MIDI OUT...", slot != nullptr && !slot->isChainSlotMidiOut(chainIndex));
      menu.addItem(-1, "(No Plugin)");

      menu.showMenuAsync(juce::PopupMenu::Options(), [this, i, slot, comp,
                                                      chainIndex](int result) {
        if (result == -3) {
          if (scanButton.onClick)
            scanButton.onClick();
          return;
        }
        if (result == -2) {
          showSlotMidiOutDialog(i, chainIndex);
          return;
        }
        if (result > 0 || result == -1) {
          if (auto *oldInstance = slot->getPluginInstance(chainIndex)) {
            closePluginWindowForInstance(oldInstance);
          }
        }
        if (result > 0) {
          loadingOverlay.reset(new LoadingOverlay());
          addAndMakeVisible(loadingOverlay.get());

          engine.loadPluginIntoSlot(
              i, chainIndex, result - 1,
              [this, comp](bool success, const juce::String &err) {
                loadingOverlay.reset();
                if (!success)
                  juce::AlertWindow::showMessageBoxAsync(
                      juce::MessageBoxIconType::WarningIcon,
                      "Plugin Load Failed", err);
                else {
                  if (comp->getIsCollapsed()) {
                    comp->setCollapsed(false);
                    resized();
                  }
                  comp->repaint();
                }
              });
        } else if (result == -1) {
          engine.clearSlotChainMidiOut(i, chainIndex);
          juce::ScopedLock sl(engine.getCallbackLock());
          slot->setPluginInChain(chainIndex, nullptr);
          comp->repaint();
        }
      });
    };

    // MIDI OUT destination editor (reuses the slot's chain config + dialog)
    comp->onEditMidiOut = [this, i](int chainIndex) {
      showSlotMidiOutDialog(i, chainIndex);
    };

    // Editor open callback
    comp->onOpenEditor = [this, slot](int chainIndex) {
      auto *plugin = slot->getPluginInstance(chainIndex);
      if (!plugin || !plugin->hasEditor())
        return;
      for (auto *w : activePluginWindows) {
        if (auto *pw = dynamic_cast<PluginWindow*>(w)) {
          if (pw->pluginInstance == plugin) {
            pw->toFront(true);
            return;
          }
        }
      }
      showLoadingOverlay("OPENING EDITOR", "Creating editor... (heavy plugins may take a while)");
      juce::MessageManager::callAsync([this, slot, chainIndex]() {
        auto *plugin = slot->getPluginInstance(chainIndex);
        if (!plugin) {
          hideLoadingOverlay();
          return;
        }
        juce::AudioProcessorEditor* editor = nullptr;
        bool ok = FanfareLog::safeExecutePluginCall([&]() {
          editor = plugin->createEditor();
        }, "createEditor (" + plugin->getName() + ")");
        hideLoadingOverlay();
        if (ok && editor != nullptr) {
          auto *w = new PluginWindow(slot->getPluginName(chainIndex), editor, plugin);
          w->onWindowClosed = [this, w] {
            if (activePluginWindows.contains(w)) {
              activePluginWindows.removeObject(w, false);
              w->destroyEditorNow();
              juce::MessageManager::callAsync([w]() { delete w; });
            }
          };
          activePluginWindows.add(w);
        } else if (!ok) {
          juce::AlertWindow::showMessageBoxAsync(
              juce::MessageBoxIconType::WarningIcon, "Editor Error",
              "Plugin editor could not be created safely.");
        }
      });
    };

    // Customize Channel / Channel Strip Callback
    comp->onShowChannelStrip = [this, i, slot] {
      // Create overlay
      channelStripOverlay.reset(new ChannelStripComponent(*slot, [this] {
        // Close callback
        channelStripOverlay.reset();
        resized(); // Re-layout to remove gap
      }));
      addAndMakeVisible(channelStripOverlay.get());
      channelStripOverlay->centreWithSize(980, 520);
    };

    // Arpeggiator Dialog
    comp->onShowArpeggiator = [this, i, slot] {
      juce::StringArray slotNames;
      for (int s = 0; s < engine.getNumSlots(); ++s)
        slotNames.add(engine.getSlot(s)->getName());
      midiEffectsOverlay.reset(new MidiEffectsComponent(
          slot->getArpeggiator(), slot->getHarmonizer(), slotNames, i, *slot,
          [this] {
            midiEffectsOverlay.reset();
            resized();
          }));
      addAndMakeVisible(midiEffectsOverlay.get());
      midiEffectsOverlay->centreWithSize(620, 410);
    };

    // Sampler Dialog
    comp->onShowSampler = [this, slot] {
      samplerOverlay.reset(new SamplerComponent(*slot, [this] {
        samplerOverlayOpen.store(false, std::memory_order_release);
        samplerOverlay.reset();
        resized();
      }));
      samplerOverlayOpen.store(true, std::memory_order_release);
      addAndMakeVisible(samplerOverlay.get());
      samplerOverlay->centreWithSize(720, 600);
    };

    // MP3 Player Dialog
    comp->onShowMp3Player = [this, slot] {
      mp3Overlay.reset(new Mp3PlayerComponent(slot->getMp3Player(), [this] {
        mp3Overlay.reset();
        resized();
      }));
      addAndMakeVisible(mp3Overlay.get());
      mp3Overlay->centreWithSize(780, 580);
    };

    // Note Range Dialog (CallOutBox)
    comp->onShowNoteRangeDialog = [this, slot, comp] {
      auto *nrComp = new NoteRangeComponent(*slot);
      nrComp->setSize(800, 180);
      setActiveMidiNoteLearner(nrComp);
      juce::CallOutBox::launchAsynchronously(
          std::unique_ptr<juce::Component>(nrComp),
          getLocalArea(comp, comp->getNoteRangeButtonBounds()), this);
    };

    // MIDI CC Dialog Callback (Advanced UI with Learn). autoArmLearn:
    // 0 = none, 1 = FOH fader, 2 = IEM fader, 3 = mute toggle (long-press).
    comp->onShowCCDialog = [this, slot, comp](int autoArmLearn) {
      auto *ccComp = new CCMappingComponent(*slot, autoArmLearn);
      ccComp->setSize(660, 510);
      auto &box = juce::CallOutBox::launchAsynchronously(
          std::unique_ptr<juce::Component>(ccComp),
          getLocalArea(comp, comp->getNoteRangeButtonBounds()), this);
      box.setDismissalMouseClicksAreAlwaysConsumed(true);
    };

    // Save Strip to File
    comp->onSaveStrip = [this, i] {
      fileChooser = std::make_unique<juce::FileChooser>(
          "Save Strip As...",
          FanfareConstants::getAppDirectory().getChildFile("strips"),
          "*.orstrip");
      fileChooser->launchAsync(
          juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles,
          [this, i](const juce::FileChooser &fc) {
            auto file = fc.getResult();
            if (file == juce::File{})
              return;
            file.getParentDirectory().createDirectory();
            engine.saveStripToFile(i, file);
          });
    };

    // Load Strip from File
    comp->onLoadStrip = [this, i, slot, comp] {
      juce::File selectedFile = libraryPanel ? libraryPanel->getSelectedPresetFile() : juce::File{};
      if (selectedFile.existsAsFile()) {
          bool isSlotEmpty = true;
          for (int chainIdx = 0; chainIdx < 3; ++chainIdx) {
              if (slot->getPluginInstance(chainIdx) != nullptr) {
                  isSlotEmpty = false;
                  break;
              }
          }
          auto loadPreset = [this, i, slot, comp, selectedFile] {
              Fanfare::SongSlot loadedSlot;
              if (Fanfare::RigSerializer::readStripFromFile(selectedFile, loadedSlot)) {
                  closePluginWindowsForSlot(slot);
                  engine.loadSlotPreset(i, loadedSlot);
                  if (comp->getIsCollapsed())
                      comp->setCollapsed(false);
                  resized();
                  comp->repaint();
              }
          };
          if (isSlotEmpty) {
              loadPreset();
          } else {
              juce::AlertWindow::showOkCancelBox(
                  juce::MessageBoxIconType::QuestionIcon,
                  "Overwrite Strip?",
                  "This strip already has plugins loaded. Do you want to overwrite it with the selected strip?",
                  "Overwrite",
                  "Cancel",
                  nullptr,
                  juce::ModalCallbackFunction::create([loadPreset](int result) {
                      if (result == 1) {
                          loadPreset();
                      }
                  })
              );
          }
      } else {
          fileChooser = std::make_unique<juce::FileChooser>(
              "Load Strip...",
              FanfareConstants::getAppDirectory().getChildFile("strips"),
              "*.orstrip");
          fileChooser->launchAsync(
              juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
              [this, i, slot, comp](const juce::FileChooser &fc) {
                auto file = fc.getResult();
                if (file == juce::File{})
                  return;
                Fanfare::SongSlot loadedSlot;
                if (Fanfare::RigSerializer::readStripFromFile(file, loadedSlot)) {
                    closePluginWindowsForSlot(slot);
                    engine.loadSlotPreset(i, loadedSlot);
                    if (comp->getIsCollapsed())
                        comp->setCollapsed(false);
                    resized();
                    comp->repaint();
                }
              });
      }
    };

    // Quick-load a specific strip file (from right-click submenu)
    comp->onLoadStripFile = [this, i, slot, comp](const juce::File &file) {
      closePluginWindowsForSlot(slot);
      engine.loadStripFromFile(i, file);
      if (comp->getIsCollapsed())
        comp->setCollapsed(false);
      resized();
    };

    // Drag-and-drop strip preset from the library sidebar
    comp->onLoadPresetFile = [this, i, slot, comp](const juce::File &file) {
      Fanfare::SongSlot loadedSlot;
      if (Fanfare::RigSerializer::readStripFromFile(file, loadedSlot)) {
        closePluginWindowsForSlot(slot);
        engine.loadSlotPreset(i, loadedSlot);
      }
      if (comp->getIsCollapsed())
        comp->setCollapsed(false);
      resized();
      comp->repaint();
    };

    // Scene-aware collapse, horizontal scroll & resize callbacks
    comp->onCollapseChanged = [this, comp](bool collapsed) {
      resized();
      if (!collapsed)
        ensureStripVisibleInRack(comp);
    };
    comp->onHorizontalScroll = [this](int deltaPx) {
      scrollRackBy(deltaPx);
    };
    comp->onRightClickUnusedEmpty = [this, comp] {
      juce::PopupMenu m;
      m.addSectionHeader("Expand Empty Strip");
      for (int sIdx = 1; sIdx < rackSlotComponents.size(); ++sIdx) {
        auto *sc = rackSlotComponents[sIdx];
        if (sc != nullptr && sc->getIsCollapsed() && sc->isUnusedEmptyStrip()) {
          m.addItem(sIdx, "Expand Strip " + juce::String(sIdx + 1) +
                              (sc == comp ? " (Next)" : ""));
        }
      }
      m.addSeparator();
      m.addItem(1000, "Expand All Unused Strips (Disable Stage Compact)");
      m.showMenuAsync(juce::PopupMenu::Options(), [this](int res) {
        if (res >= 1 && res < rackSlotComponents.size()) {
          auto *sc = rackSlotComponents[res];
          if (sc != nullptr) {
            sc->setCollapsed(false);
            resized();
            ensureStripVisibleInRack(sc);
          }
        } else if (res == 1000) {
          autoCollapseToggle.setToggleState(false, juce::dontSendNotification);
          for (auto *sc : rackSlotComponents)
            sc->setCollapsed(false);
          resized();
        }
      });
    };
    comp->onBypassChanged = [this](bool) {
      if (autoCollapseToggle.getToggleState())
        updateStripCollapseStates();
    };
    comp->onStartDragWidth = [this]() {
      dragBaseExpandedWidth = preferredExpandedWidth;
    };
    comp->onDragWidth = [this](int deltaX) {
      preferredExpandedWidth = juce::jlimit(kMinExpandedWidth, kMaxExpandedWidth, dragBaseExpandedWidth + deltaX);
      stripWidthSlider.setValue(preferredExpandedWidth, juce::dontSendNotification);
      resized();
      savePreferredStripWidth();
    };

    rackSlotComponents.add(comp);
    rackStripContainer.addAndMakeVisible(comp);
  }

  // Setup Aux Returns
  for (int i = 0; i < engine.getNumAuxReturns(); ++i) {
    auto *slot = engine.getAuxReturn(i);
    auto *comp = new RackSlotComponent(*slot, 100 + i, boutiqueLookAndFeel);

    comp->setSpecialModes(false, false, true);

    comp->onShowPluginMenu = [this, i, slot, comp](int chainIndex) {
      juce::PopupMenu menu;
      if (false) { int dummy = 0; (void)dummy; }
      for (int p = 0; p < engine.getNumAvailablePlugins(); ++p) menu.addItem(p + 1, engine.getAvailablePluginName(p));
      menu.addSeparator();
      menu.addItem(-1, "(No Plugin)");

      menu.showMenuAsync(juce::PopupMenu::Options(),
                         [this, i, slot, comp, chainIndex](int result) {
                           if (result > 0 || result == -1) {
                             if (auto *oldInstance = slot->getPluginInstance(chainIndex))
                               closePluginWindowForInstance(oldInstance);
                           }
                           if (result > 0) {
                             loadingOverlay.reset(new LoadingOverlay());
                             addAndMakeVisible(loadingOverlay.get());
                             engine.loadPluginIntoSlot(
                                 (100 + i), chainIndex, result - 1,
                                 [this, comp](bool s, const juce::String &e) {
                                   juce::ignoreUnused(s, e);
                                   loadingOverlay.reset();
                                   comp->repaint();
                                 });
                           } else if (result == -1) {
                             juce::ScopedLock sl(engine.getCallbackLock());
                             slot->setPluginInChain(chainIndex, nullptr);
                             comp->repaint();
                           }
                         });
    };

    // Editor open callback
    comp->onOpenEditor = [this, slot](int chainIndex) {
      auto *plugin = slot->getPluginInstance(chainIndex);
      if (!plugin || !plugin->hasEditor())
        return;
      for (auto *w : activePluginWindows) {
        if (auto *pw = dynamic_cast<PluginWindow*>(w)) {
          if (pw->pluginInstance == plugin) {
            pw->toFront(true);
            return;
          }
        }
      }
      showLoadingOverlay("OPENING EDITOR", "Creating editor... (heavy plugins may take a while)");
      juce::MessageManager::callAsync([this, slot, chainIndex]() {
        auto *plugin = slot->getPluginInstance(chainIndex);
        if (!plugin) {
          hideLoadingOverlay();
          return;
        }
        juce::AudioProcessorEditor* editor = nullptr;
        bool ok = FanfareLog::safeExecutePluginCall([&]() {
          editor = plugin->createEditor();
        }, "createEditor (" + plugin->getName() + ")");
        hideLoadingOverlay();
        if (ok && editor != nullptr) {
          auto *w = new PluginWindow(slot->getPluginName(chainIndex), editor, plugin);
          w->onWindowClosed = [this, w] {
            if (activePluginWindows.contains(w)) {
              activePluginWindows.removeObject(w, false);
              w->destroyEditorNow();
              juce::MessageManager::callAsync([w]() { delete w; });
            }
          };
          activePluginWindows.add(w);
        } else if (!ok) {
          juce::AlertWindow::showMessageBoxAsync(
              juce::MessageBoxIconType::WarningIcon, "Editor Error",
              "Plugin editor could not be created safely.");
        }
      });
    };

    // Note Range Dialog (CallOutBox)
    comp->onShowNoteRangeDialog = [this, slot, comp] {
      auto *nrComp = new NoteRangeComponent(*slot);
      nrComp->setSize(800, 180);
      setActiveMidiNoteLearner(nrComp);
      juce::CallOutBox::launchAsynchronously(
          std::unique_ptr<juce::Component>(nrComp),
          getLocalArea(comp, comp->getNoteRangeButtonBounds()), this);
    };

    comp->onCollapseChanged = [this](bool) {
      resized();
    };
    comp->onHorizontalScroll = [this](int deltaPx) {
      scrollRackBy(deltaPx);
    };

    comp->setCollapsed(true);
    auxReturnComponents.add(comp);
    addAndMakeVisible(comp);
  }
}

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
  // Include 36px lookahead on the right so the newly revealed "+ ADD" tile is visible too
  const int stripRight =
      juce::jmin(rackStripContainer.getWidth(), target->getRight() + 36);
  if (stripRight > viewX + viewW)
    setRackScrollX(stripRight - viewW);
  else if (stripLeft < viewX)
    setRackScrollX(stripLeft);
}

void MainComponent::setupHeaderButtons() {
  // Scan button
  addAndMakeVisible(scanButton);
  Fanfare::setButtonRole(scanButton, Fanfare::ButtonRole::Primary);
  scanButton.onClick = [this] {
    scanButton.setButtonText("SCANNING...");
    scanButton.setEnabled(false);
    engine.scanForPlugins();
  };
  engine.onScanFinished = [this](FanfareEngine::ScanResults results) {
    scanButton.setButtonText("SCAN FOR PLUGINS");
    scanButton.setEnabled(true);
    engine.applyScanResults(results.newPlugins, results.missingPlugins);
    showScanResultsDialog(results);
  };

  // Panic button
  addAndMakeVisible(panicBtn);
  Fanfare::setButtonRole(panicBtn, Fanfare::ButtonRole::Panic);
  panicBtn.setTooltip("PANIC: Stage emergency button. Instantly silences all stuck MIDI notes & flushes audio buffers.");
  panicBtn.onClick = [this] {
    engine.triggerPanic();
    LOG_INFO("PANIC: All notes killed & audio buffers flushed. Take a breath, you've got this!");
  };

  // Reset Audio button
  addAndMakeVisible(resetAudioBtn);
  Fanfare::setButtonRole(resetAudioBtn, Fanfare::ButtonRole::Danger);
  resetAudioBtn.setTooltip("Reset Audio: Re-initializes audio driver threads & clears sample-rate locks mid-gig.");
  resetAudioBtn.onClick = [this] { resetAudioDevice(); };

  // Exit button
  addAndMakeVisible(exitBtn);
  Fanfare::setButtonRole(exitBtn, Fanfare::ButtonRole::Subtle);
  exitBtn.setTooltip("Quit Fanfare");
  exitBtn.onClick = [] { juce::JUCEApplication::quit(); };

  // Audio Settings button
  addAndMakeVisible(audioSettingsBtn);
  Fanfare::setButtonRole(audioSettingsBtn, Fanfare::ButtonRole::Primary);
  audioSettingsBtn.setTooltip("Audio & MIDI Setup: Low-latency driver configuration, sample rates & MIDI routing.");
  audioSettingsBtn.onClick = [this] {
    auto *selector = new juce::AudioDeviceSelectorComponent(
        deviceManager, 0, FanfareConstants::kDefaultInputChannels, 0,
        FanfareConstants::kDefaultOutputChannels, true, true, true, false);
    selector->setSize(500, 400);
    activeAudioSelector = selector;
    juce::DialogWindow::LaunchOptions options;
    options.content.setOwned(selector);
    options.dialogTitle = "Audio & MIDI Settings";
    options.dialogBackgroundColour = ThemeManager::get(Theme::Role::panel);
    options.escapeKeyTriggersCloseButton = true;
    options.useNativeTitleBar = true;
    options.resizable = false;
    options.launchAsync();
  };

  // MIDI Monitor
  addAndMakeVisible(midiMonitorLabel);
  midiMonitorLabel.setFont(BoutiqueLookAndFeel::monoFont(11.5f, true));
  midiMonitorLabel.setJustificationType(juce::Justification::centredLeft);

  // MIDI Monitor toggle button
  addAndMakeVisible(midiMonitorToggle);
  Fanfare::setButtonRole(midiMonitorToggle, Fanfare::ButtonRole::Standard);
  midiMonitorToggle.onClick = [this] {
    midiMonitorVisible = !midiMonitorVisible;
    if (midiMonitorVisible) {
      if (!midiMonitorPanel) {
        midiMonitorPanel = std::make_unique<MidiMonitorComponent>();
        addChildComponent(midiMonitorPanel.get());
      }
      midiMonitorPanel->setVisible(true);
      midiMonitorToggle.setToggleState(true, juce::dontSendNotification);
    } else {
      if (midiMonitorPanel)
        midiMonitorPanel->setVisible(false);
      midiMonitorToggle.setToggleState(false, juce::dontSendNotification);
    }
    resized();
  };

  // Legacy aboutBtn replaced by clickable Fanfare logo
  aboutBtn.setVisible(false);
  aboutBtn.onClick = [this] { showAboutDialog(); };

  // Settings gear button — centralizes the settings buttons into one overlay
  addAndMakeVisible(settingsGearBtn);
  auto gearImg = createGearImage(28, ThemeManager::get(Theme::Role::accent));
  settingsGearBtn.setImages(false, true, true, gearImg, 1.0f,
                            juce::Colours::transparentBlack, gearImg, 1.0f,
                            juce::Colours::white.withAlpha(0.25f), gearImg,
                            1.0f, juce::Colours::white.withAlpha(0.5f));
  settingsGearBtn.onClick = [this] { showConfigOverlay(); };
  settingsGearBtn.setTooltip("Settings");

  // The individual settings buttons are now surfaced through the gear overlay.
  // Their handlers are retained so the overlay can invoke them.
  scanButton.setVisible(false);
  audioSettingsBtn.setVisible(false);
  busRoutingBtn.setVisible(false);
  resetAudioBtn.setVisible(false);

  // Bus Routing button
  addAndMakeVisible(busRoutingBtn);
  Fanfare::setButtonRole(busRoutingBtn, Fanfare::ButtonRole::Standard);
  busRoutingBtn.onClick = [this] {
    juce::PopupMenu menu;
    menu.addSectionHeader("FOH Output Channels");
    menu.addItem(1, "Outputs 1+2", true, engine.getFohOutputOffset() == 0);
    menu.addItem(2, "Outputs 3+4", true, engine.getFohOutputOffset() == 2);
    menu.addItem(3, "Outputs 5+6", true, engine.getFohOutputOffset() == 4);
    menu.addItem(4, "Outputs 7+8", true, engine.getFohOutputOffset() == 6);
    menu.addSeparator();
    menu.addSectionHeader("IEM Output Channels");
    menu.addItem(11, "Outputs 1+2", true, engine.getIemOutputOffset() == 0);
    menu.addItem(12, "Outputs 3+4", true, engine.getIemOutputOffset() == 2);
    menu.addItem(13, "Outputs 5+6", true, engine.getIemOutputOffset() == 4);
    menu.addItem(14, "Outputs 7+8", true, engine.getIemOutputOffset() == 6);

    menu.addSeparator();
    menu.addSectionHeader("MIDI Channel (Global Default)");
    int curCh = engine.getDefaultMidiChannel();
    menu.addItem(100, "Omni (all channels)", true, curCh == 0);
    for (int ch = 1; ch <= 16; ++ch)
      menu.addItem(100 + ch, "Channel " + juce::String(ch), true, curCh == ch);

    menu.showMenuAsync(juce::PopupMenu::Options(), [this](int result) {
      if (result >= 1 && result <= 4) {
        engine.setFohOutputOffset((result - 1) * 2);
      } else if (result >= 11 && result <= 14) {
        engine.setIemOutputOffset((result - 11) * 2);
      } else if (result == 100) {
        engine.setDefaultMidiChannel(0);
      } else if (result >= 101 && result <= 116) {
        engine.setDefaultMidiChannel(result - 100);
      }
    });
  };

  // Save/Load buttons
  addAndMakeVisible(saveBtn);
  Fanfare::setButtonRole(saveBtn, Fanfare::ButtonRole::Standard);
  saveBtn.onClick = [this] {
    auto chooser = std::make_shared<juce::FileChooser>(
        "Save Song Configuration",
        Fanfare::RigLibrary::getSongsDirectory(),
        "*.json");

    chooser->launchAsync(
        juce::FileBrowserComponent::saveMode |
            juce::FileBrowserComponent::canSelectFiles,
        [this, chooser](const juce::FileChooser &fc) {
          auto file = fc.getResult();
          if (file != juce::File()) {
            if (file.getFileExtension() != ".json")
              file = file.withFileExtension(".json");

            juce::String json = engine.exportRigToJson();
            SaveHistory::recordSave(file);
            if (Fanfare::RigSerializer::save(file, json)) {
              SaveHistory::recordSave(file);
              currentRigFile = file;
              setupNameLabel.setText(file.getFileNameWithoutExtension(), juce::dontSendNotification);
              Fanfare::RecentSongs::record(file);
              LOG_INFO("Rig saved to: " + file.getFullPathName());
            } else {
              LOG_ERROR("Failed to save rig to: " + file.getFullPathName());
            }
          }
        });
  };

  addAndMakeVisible(loadBtn);
  Fanfare::setButtonRole(loadBtn, Fanfare::ButtonRole::Standard);
  loadBtn.onClick = [this] {
    auto chooser = std::make_shared<juce::FileChooser>(
        "Load Song Configuration",
        Fanfare::RigLibrary::getSongsDirectory(),
        "*.json");

    chooser->launchAsync(juce::FileBrowserComponent::openMode |
                             juce::FileBrowserComponent::canSelectFiles,
                          [this, chooser](const juce::FileChooser &fc) {
                            auto file = fc.getResult();
                            if (file.existsAsFile())
                              loadRigAsync(file);
                         });
  };

  // Gig Notepad & Stage Notes button
  addAndMakeVisible(gigNotepadBtn);
  Fanfare::setButtonRole(gigNotepadBtn, Fanfare::ButtonRole::Standard);
  gigNotepadBtn.setTooltip("Open Gig Notepad & Stage Notes for Current Song (Ctrl+N)");
  gigNotepadBtn.onClick = [this] { showGigNotepadWindow(); };

  // Web Remote HUD button
  addAndMakeVisible(webRemoteBtn);
  Fanfare::setButtonRole(webRemoteBtn, Fanfare::ButtonRole::Standard);
  webRemoteBtn.setTooltip("Open Web Remote HUD Settings & Tablet Connection URL");
  webRemoteBtn.onClick = [this] { showWebRemoteDialog(); };

  // Quick-Start [?] First Steps button
  addAndMakeVisible(quickStartBtn);
  quickStartBtn.setButtonText("?");
  Fanfare::setButtonRole(quickStartBtn, Fanfare::ButtonRole::Primary);
  quickStartBtn.setTooltip("First Steps: Quick-Start Checklist & VST3 Plugin Scan");
  quickStartBtn.onClick = [this] { showQuickStartDialog(false); };

  Fanfare::styleHelpButton(mainHelpBtn, "#main-mixer", "HAAALP! Open the Fanfare User's Manual");
  addAndMakeVisible(mainHelpBtn);

  addChildComponent(updateBadgeBtn);
  Fanfare::setButtonRole(updateBadgeBtn, Fanfare::ButtonRole::Primary);
  updateBadgeBtn.onClick = [this] {
    if (latestReleaseUrl.isNotEmpty())
      juce::URL(latestReleaseUrl).launchInDefaultBrowser();
  };

  // Prev / Next Setlist buttons
  addAndMakeVisible(prevSetlistBtn);
  Fanfare::setButtonRole(prevSetlistBtn, Fanfare::ButtonRole::Standard);
  prevSetlistBtn.setTooltip("Previous Song in Setlist");
  prevSetlistBtn.onClick = [this] {
      auto& sm = Fanfare::SetlistManager::getInstance();
      if (sm.hasPrev()) {
          int idx = sm.getActiveIndex() - 1;
          loadRigFromFile(sm.getSetups()[idx], idx);
      }
  };

  addAndMakeVisible(nextSetlistBtn);
  Fanfare::setButtonRole(nextSetlistBtn, Fanfare::ButtonRole::Standard);
  nextSetlistBtn.setTooltip("Next Song in Setlist (Background Preloaded)");
  nextSetlistBtn.onClick = [this] {
      auto& sm = Fanfare::SetlistManager::getInstance();
      if (sm.hasNext()) {
          int idx = sm.getActiveIndex() + 1;
          loadRigFromFile(sm.getSetups()[idx], idx);
      }
  };

  // CPU/RAM/Latency/XRUNS monitors (monospace hardware LCD telemetry)
  addAndMakeVisible(cpuLabel);
  cpuLabel.setFont(BoutiqueLookAndFeel::monoFont(11.0f, true));
  cpuLabel.setJustificationType(juce::Justification::centred);
  cpuLabel.setTooltip("Click to view Per-VST Memory & Resource Inspector");
  cpuLabel.addMouseListener(this, false);

  addAndMakeVisible(ramLabel);
  ramLabel.setFont(BoutiqueLookAndFeel::monoFont(11.0f, true));
  ramLabel.setJustificationType(juce::Justification::centred);
  ramLabel.setTooltip("Click to view Per-VST Memory & Resource Inspector");
  ramLabel.addMouseListener(this, false);

  addAndMakeVisible(latencyLabel);
  latencyLabel.setFont(BoutiqueLookAndFeel::monoFont(11.0f, true));
  latencyLabel.setJustificationType(juce::Justification::centred);
  latencyLabel.setTooltip("Round-trip audio interface latency (Input + Output)");

  addAndMakeVisible(xrunsLabel);
  xrunsLabel.setFont(BoutiqueLookAndFeel::monoFont(11.0f, true));
  xrunsLabel.setJustificationType(juce::Justification::centred);
  xrunsLabel.setTooltip("Audio Glitch / Underrun Counter (Click to view log)");
  xrunsLabel.addMouseListener(this, false);

  addChildComponent(thermalLabel);
  thermalLabel.setFont(BoutiqueLookAndFeel::monoFont(10.5f, true));
  thermalLabel.setJustificationType(juce::Justification::centred);
  thermalLabel.setColour(juce::Label::textColourId, juce::Colours::white);
  thermalLabel.setMouseCursor(juce::MouseCursor::PointingHandCursor);
  thermalLabel.setTooltip("CPU Thermal / Firmware Throttle Alert (Click for details)");
  thermalLabel.addMouseListener(this, false);

  addAndMakeVisible(setupNameLabel);
  setupNameLabel.setFont(BoutiqueLookAndFeel::condensedFont(20.0f, 0.04f));
  setupNameLabel.setJustificationType(juce::Justification::centredLeft);

  addAndMakeVisible(freshBuildToggle);
  freshBuildToggle.setColour(juce::ToggleButton::textColourId, ThemeManager::get(Theme::Role::accent));
  freshBuildToggle.setTooltip("Clean Slate: When enabled, loading this song unloads all prior plugins first and builds from the ground up.");
  freshBuildToggle.onClick = [this] {
    bool isFresh = freshBuildToggle.getToggleState();
    engine.setCurrentRigFreshBuild(isFresh);

    juce::File targetFile = currentRigFile;
    if (!targetFile.existsAsFile()) {
      targetFile = Fanfare::RigLibrary::getSongsDirectory().getChildFile(setupNameLabel.getText() + ".json");
    }

    if (targetFile.existsAsFile()) {
      SaveHistory::recordSave(targetFile);
      auto parsed = juce::JSON::parse(targetFile);
      if (auto *obj = parsed.getDynamicObject()) {
        obj->setProperty("freshBuild", isFresh);
        if (Fanfare::RigSerializer::save(targetFile, juce::JSON::toString(parsed)))
          SaveHistory::recordSave(targetFile);
        LOG_INFO("Setup flagged on disk: " + targetFile.getFileName() + " -> freshBuild=" + juce::String((int)isFresh));
      }
    }
  };

  addAndMakeVisible(autoCollapseToggle);
  autoCollapseToggle.setToggleState(false, juce::dontSendNotification);
  autoCollapseToggle.setColour(juce::ToggleButton::textColourId, ThemeManager::get(Theme::Role::accent));
  autoCollapseToggle.setTooltip("Stage Compact: Automatically collapses muted/inactive strips to save screen space, expanding active channels.");
  autoCollapseToggle.onClick = [this] {
    if (autoCollapseToggle.getToggleState()) {
      updateStripCollapseStates();
    } else {
      for (auto *c : rackSlotComponents)
        c->setCollapsed(false);
      resized();
    }
  };

  addAndMakeVisible(stripWidthLabel);
  stripWidthLabel.setFont(BoutiqueLookAndFeel::condensedFont(10.5f, 0.06f));
  stripWidthLabel.setJustificationType(juce::Justification::centredRight);

  addAndMakeVisible(stripWidthSlider);
  stripWidthSlider.setRange(kMinExpandedWidth, kMaxExpandedWidth, 2);
  stripWidthSlider.setValue(preferredExpandedWidth, juce::dontSendNotification);
  stripWidthSlider.setSliderStyle(juce::Slider::LinearHorizontal);
  stripWidthSlider.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
  stripWidthSlider.setTooltip("Adjust expanded channel strip width (85px - 260px). You can also drag the right edge of any strip directly!");
  stripWidthSlider.onValueChange = [this] {
    preferredExpandedWidth = (int)stripWidthSlider.getValue();
    resized();
    savePreferredStripWidth();
  };

  // Load embedded Fanfare white logo (for dark stage UI)
  logoImage = juce::ImageFileFormat::loadFrom(fanfare_logo_png, fanfare_logo_png_size);
  if (logoImage.isValid()) {
    logoComponent.setImage(logoImage, juce::RectanglePlacement::onlyReduceInSize | juce::RectanglePlacement::xLeft | juce::RectanglePlacement::yMid);
    logoComponent.setMouseCursor(juce::MouseCursor::PointingHandCursor);
    logoComponent.setTooltip("About Fanfare (v" OPENRIG_VERSION_STRING ")");
    logoComponent.addMouseListener(this, false);
    addAndMakeVisible(logoComponent);
  }

  // Load The Last Waltz gig logo (shown beneath the aux return strips).
  // Click the area to fly in a custom image; right-click restores this one.
  lastWaltzLogoComponent.setMouseCursor(juce::MouseCursor::PointingHandCursor);
  lastWaltzLogoComponent.setTooltip(
      "Click: fly in your own logo image (PNG/JPG). Right-click: restore the "
      "Last Waltz logo.");
  lastWaltzLogoComponent.addMouseListener(this, false);
  lastWaltzLogoComponent.setInterceptsMouseClicks(true, false);
  addAndMakeVisible(lastWaltzLogoComponent);
  lastWaltzLogoComponent.setVisible(false);
  {
    juce::Image embedded;
#if OPENRIG_HAS_LW_LOGO
    embedded = juce::ImageFileFormat::loadFrom(last_waltz_logo_png,
                                               last_waltz_logo_png_size);
#endif
    if (!embedded.isValid())
      embedded = juce::ImageFileFormat::loadFrom(fanfare_logo_png,
                                                 fanfare_logo_png_size);
    if (embedded.isValid())
      setAuxLogoImage(embedded, false);
  }
  loadAuxLogoPreference();

  addAndMakeVisible(clockLabel);
  clockLabel.setFont(BoutiqueLookAndFeel::monoFont(11.5f, true));
  clockLabel.setJustificationType(juce::Justification::centred);

  addAndMakeVisible(preloadStatusLabel);
  preloadStatusLabel.setFont(BoutiqueLookAndFeel::condensedFont(12.5f, 0.04f));
  preloadStatusLabel.setJustificationType(juce::Justification::centredRight);
  updatePreloadStatus();
}


void MainComponent::setupSetupButtons() {
  addAndMakeVisible(saveSetBtn);
  addAndMakeVisible(loadSetBtn);
  addAndMakeVisible(setupBuilderBtn);
  
  Fanfare::setButtonRole(saveSetBtn, Fanfare::ButtonRole::Standard);
  Fanfare::setButtonRole(loadSetBtn, Fanfare::ButtonRole::Standard);
  Fanfare::setButtonRole(setupBuilderBtn, Fanfare::ButtonRole::Primary);
  
  saveSetBtn.onClick = [this] { saveSetToFile(); };
  loadSetBtn.onClick = [this] { loadSetFromFile(); };
  setupBuilderBtn.onClick = [this] { showSetupBuilderOverlay(); };

  queueButtons.clear();
  for (int i = 0; i < numSetupButtons; ++i) {
    auto* btn = new Fanfare::QueueButtonComponent(i);
    btn->onClick = [this, i] { loadRigFile(i); };
    btn->onFileDropped = [this](int slotIdx, const juce::File& file) {
      if (slotIdx >= 0 && slotIdx < numSetupButtons) {
        setupFilePaths[slotIdx] = file.getFullPathName();
        saveButtonMappings();
        auto& sm = Fanfare::SetlistManager::getInstance();
        sm.setSlotSetup(slotIdx, file);
        updatePreloadStatus();
      }
    };
    btn->onAssignRequested = [this](int slotIdx) { assignJsonToButton(slotIdx); };
    btn->onClearRequested = [this](int slotIdx) {
      if (slotIdx >= 0 && slotIdx < numSetupButtons) {
        setupFilePaths[slotIdx] = "";
        saveButtonMappings();
        auto& sm = Fanfare::SetlistManager::getInstance();
        sm.setSlotSetup(slotIdx, juce::File{});
        updatePreloadStatus();
      }
    };
    addAndMakeVisible(btn);
    queueButtons.add(btn);
  }

  // Scene management buttons
  addAndMakeVisible(addSceneBtn);
  addAndMakeVisible(saveSceneBtn);
  addAndMakeVisible(renameSceneBtn);
  addAndMakeVisible(deleteSceneBtn);

  Fanfare::setButtonRole(addSceneBtn, Fanfare::ButtonRole::Standard);
  Fanfare::setButtonRole(saveSceneBtn, Fanfare::ButtonRole::Standard);
  Fanfare::setButtonRole(renameSceneBtn, Fanfare::ButtonRole::Standard);
  Fanfare::setButtonRole(deleteSceneBtn, Fanfare::ButtonRole::Danger);

  addSceneBtn.onClick = [this] {
    engine.createNewScene("SCENE " + juce::String(engine.getNumScenes() + 1));
    refreshSceneButtons();
  };

  saveSceneBtn.onClick = [this] {
    engine.saveCurrentStateToScene(engine.getCurrentSceneIndex());
  };

  renameSceneBtn.onClick = [this] {
    int idx = engine.getCurrentSceneIndex();
    if (idx < 0 || idx >= engine.getNumScenes())
      return;
    auto *alert = new juce::AlertWindow("Rename Scene", "Enter new name:",
                                        juce::AlertWindow::QuestionIcon);
    alert->addTextEditor("name", engine.getSceneName(idx));
    alert->addButton("Rename", 1, juce::KeyPress(juce::KeyPress::returnKey));
    alert->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
    alert->enterModalState(true,
        juce::ModalCallbackFunction::create([this, idx, alert](int result) {
          if (result == 1) {
            juce::String newName = alert->getTextEditorContents("name");
            if (newName.isNotEmpty()) {
              engine.renameScene(idx, newName);
              refreshSceneButtons();
            }
          }
          delete alert;
        }));
  };

  deleteSceneBtn.onClick = [this] {
    int idx = engine.getCurrentSceneIndex();
    if (idx >= 0 && idx < engine.getNumScenes()) {
      if (engine.getNumScenes() <= 1) {
        juce::AlertWindow::showMessageBoxAsync(
            juce::MessageBoxIconType::InfoIcon, "Delete Scene",
            "Cannot delete the last remaining scene.");
        return;
      }
      engine.deleteScene(idx);
      refreshSceneButtons();
    }
  };

  loadButtonMappings();
}

void MainComponent::refreshSceneButtons() {
  sceneButtons.clear();
  int numScenes = engine.getNumScenes();

  while (sceneSetupFilePaths.size() < numScenes)
    sceneSetupFilePaths.add("");

  for (int i = 0; i < numScenes; ++i) {
    auto *btn = new Fanfare::SceneButtonComponent(i, engine.getSceneName(i));

    juce::File assignedFile(sceneSetupFilePaths[i]);
    if (assignedFile.existsAsFile()) {
      btn->setAssignedFile(assignedFile);
    }

    int assignedPC = engine.getSceneMidiPC(i);
    int assignedCh = engine.getSceneMidiChannel(i);
    btn->setMidiTriggerInfo(assignedPC, assignedCh);

    if (i == engine.getCurrentSceneIndex()) {
      btn->setActive(true);
    }

    int sceneIdx = i;
    btn->onFileDropped = [this](int idx, const juce::File& file) {
      while (sceneSetupFilePaths.size() <= idx) sceneSetupFilePaths.add("");
      sceneSetupFilePaths.set(idx, file.getFullPathName());
      saveButtonMappings();
      refreshSceneButtons();
    };

    btn->onClicked = [this, sceneIdx](int) {
      if (sceneIdx >= 0 && sceneIdx < sceneSetupFilePaths.size()) {
        juce::File f(sceneSetupFilePaths[sceneIdx]);
        if (f.existsAsFile()) {
          loadRigAsync(f);
          return;
        }
      }
      engine.saveCurrentStateToScene(engine.getCurrentSceneIndex());
      engine.loadScene(sceneIdx);
      for (int j = 0; j < sceneButtons.size(); ++j)
        sceneButtons[j]->setActive(j == sceneIdx);
      if (webServer)
        webServer->notifySceneChanged(sceneIdx, engine.getSceneName(sceneIdx));
      updateStripCollapseStates();
    };

    btn->onRightClicked = [this, sceneIdx](int, const juce::MouseEvent&) {
      juce::PopupMenu menu;
      juce::File assigned(sceneSetupFilePaths.size() > sceneIdx ? sceneSetupFilePaths[sceneIdx] : "");

      menu.addSectionHeader("SCENE " + juce::String(sceneIdx + 1));
      menu.addItem(1, "Assign Song File...");
      if (assigned.existsAsFile()) {
        menu.addItem(2, "Clear Song Assignment");
      }
      menu.addSeparator();

      int currentPC = engine.getSceneMidiPC(sceneIdx);
      if (currentPC >= 0) {
        int currentCh = engine.getSceneMidiChannel(sceneIdx);
        juce::String info = "MIDI Trigger: PC " + juce::String(currentPC);
        if (currentCh > 0) info += " / Ch " + juce::String(currentCh);
        menu.addItem(-1, info, false, false);
        menu.addSeparator();
      }
      menu.addItem(3, "Assign MIDI Trigger (Learn)...");
      if (currentPC >= 0)
        menu.addItem(4, "Clear MIDI Trigger");

      auto* b = sceneButtons[sceneIdx];
      menu.showMenuAsync(juce::PopupMenu::Options{}.withTargetComponent(b),
        [this, sceneIdx](int result) {
          if (result == 1) {
            auto fileChooser = std::make_shared<juce::FileChooser>(
              "Select Song File...", Fanfare::RigLibrary::getSongsDirectory(), "*.json");
            fileChooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
              [this, sceneIdx, fileChooser](const juce::FileChooser& fc) {
                auto file = fc.getResult();
                if (file.existsAsFile()) {
                  while (sceneSetupFilePaths.size() <= sceneIdx) sceneSetupFilePaths.add("");
                  sceneSetupFilePaths.set(sceneIdx, file.getFullPathName());
                  saveButtonMappings();
                  refreshSceneButtons();
                }
              });
          } else if (result == 2) {
            if (sceneIdx < sceneSetupFilePaths.size())
              sceneSetupFilePaths.set(sceneIdx, "");
            saveButtonMappings();
            refreshSceneButtons();
          } else if (result == 3) {
            if (sceneLearnAlert != nullptr) {
              sceneLearnAlert->exitModalState(0);
              sceneLearnAlert = nullptr;
            }
            auto* alert = new juce::AlertWindow(
              "MIDI Learn",
              "Send a Program Change from your controller now...",
              juce::AlertWindow::InfoIcon);
            alert->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));

            sceneMidiLearnArmed.store(sceneIdx, std::memory_order_release);
            juce::Component::SafePointer<MainComponent> safeThis(this);
            alert->enterModalState(true,
              juce::ModalCallbackFunction::create([safeThis](int) {
                if (safeThis != nullptr)
                  safeThis->sceneMidiLearnArmed.store(-1, std::memory_order_release);
              }),
              true);
            sceneLearnAlert = alert;
          } else if (result == 4) {
            engine.clearSceneMidiTrigger(sceneIdx);
            saveButtonMappings();
            refreshSceneButtons();
          }
        });
    };

    addAndMakeVisible(btn);
    sceneButtons.add(btn);
  }
  updateStripCollapseStates();
  resized();
}

void MainComponent::setupMasterSection() {
  // Master FOH slider
  addAndMakeVisible(masterFohSlider);
  masterFohSlider.setComponentID("master_foh");
  masterFohSlider.setSliderStyle(juce::Slider::LinearVertical);
  masterFohSlider.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
  masterFohSlider.setRange(0.0, 1.0);
  masterFohSlider.setValue(1.0);
  masterFohSlider.onValueChange = [this] {
    engine.setFohMasterLevel((float)masterFohSlider.getValue());
  };

  // Master IEM slider
  addAndMakeVisible(masterIemSlider);
  masterIemSlider.setComponentID("master_iem");
  masterIemSlider.setSliderStyle(juce::Slider::LinearVertical);
  masterIemSlider.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
  masterIemSlider.setRange(0.0, 1.0);
  masterIemSlider.setValue(1.0);
  masterIemSlider.onValueChange = [this] {
    engine.setIemMasterLevel((float)masterIemSlider.getValue());
  };

  // Labels & VU Meters
  addAndMakeVisible(fohLabel);
  fohLabel.setFont(BoutiqueLookAndFeel::condensedFont(13.0f, 0.08f));
  fohLabel.setJustificationType(juce::Justification::centred);

  addAndMakeVisible(iemLabel);
  iemLabel.setFont(BoutiqueLookAndFeel::condensedFont(13.0f, 0.08f));
  iemLabel.setJustificationType(juce::Justification::centred);

  addAndMakeVisible(masterVuMeterL);
  addAndMakeVisible(masterVuMeterR);

  // FX Slots for FOH / IEM masters
  for (int i = 0; i < 3; ++i) {
    addAndMakeVisible(fohFxBtns[i]);
    bool fohHas = !engine.getMasterPluginName(true, i).isEmpty();
    Fanfare::setButtonRole(fohFxBtns[i], Fanfare::ButtonRole::PluginSlot);
    fohFxBtns[i].getProperties().set("isPluginSlot", true);
    fohFxBtns[i].getProperties().set("hasPlugin", fohHas);
    fohFxBtns[i].getProperties().set("slotAccent", (int)ThemeManager::get(Theme::Role::foh).getARGB());
    fohFxBtns[i].setButtonText(fohHas ? engine.getMasterPluginName(true, i) : ("FOH " + juce::String(i + 1)));
    fohFxBtns[i].onClick = [this, i] { showMasterPluginMenu(true, i); };

    addAndMakeVisible(fohEditGuiBtns[i]);
    fohEditGuiBtns[i].setButtonText("E");
    Fanfare::setButtonRole(fohEditGuiBtns[i], Fanfare::ButtonRole::Subtle);
    fohEditGuiBtns[i].onClick = [this, i] { openMasterPluginEditor(true, i); };

    addAndMakeVisible(iemFxBtns[i]);
    bool iemHas = !engine.getMasterPluginName(false, i).isEmpty();
    Fanfare::setButtonRole(iemFxBtns[i], Fanfare::ButtonRole::PluginSlot);
    iemFxBtns[i].getProperties().set("isPluginSlot", true);
    iemFxBtns[i].getProperties().set("hasPlugin", iemHas);
    iemFxBtns[i].getProperties().set("slotAccent", (int)ThemeManager::get(Theme::Role::iem).getARGB());
    iemFxBtns[i].setButtonText(iemHas ? engine.getMasterPluginName(false, i) : ("IEM " + juce::String(i + 1)));
    iemFxBtns[i].onClick = [this, i] { showMasterPluginMenu(false, i); };

    addAndMakeVisible(iemEditGuiBtns[i]);
    iemEditGuiBtns[i].setButtonText("E");
    Fanfare::setButtonRole(iemEditGuiBtns[i], Fanfare::ButtonRole::Subtle);
    iemEditGuiBtns[i].onClick = [this, i] { openMasterPluginEditor(false, i); };
  }
}

void MainComponent::logAudioGlitch(double actualMs, double expectedMs,
                                   double cbDurationMs) {
  juce::String timeStr = juce::Time::getCurrentTime().formatted("%H:%M:%S.");
  timeStr += juce::String(juce::Time::getCurrentTime().getMilliseconds()).paddedLeft('0', 3);

  juce::String entry = "[" + timeStr + "] [AUDIO UNDERRUN GLITCH] Buffer stall: " +
                       juce::String(actualMs, 2) + "ms (expected " +
                       juce::String(expectedMs, 2) + "ms) | callback took: " +
                       juce::String(cbDurationMs, 2) + "ms | CPU: " +
                       cpuLabel.getText() + " | RAM: " + ramLabel.getText() +
                       " | " + thermalMonitor.formatGlitchLogTag() + "\n";

  // Batched: appends collect in memory and flush to disk at most once per
  // second (glitchMonitorTimer). During a storm this replaces ~13 file
  // opens/second with a single append - the message thread stays responsive.
  pendingGlitchLogText += entry;
}

void MainComponent::logAudioNanSpike(int channel, int sampleIndex, float badVal) {
  juce::String timeStr = juce::Time::getCurrentTime().formatted("%H:%M:%S.");
  timeStr += juce::String(juce::Time::getCurrentTime().getMilliseconds()).paddedLeft('0', 3);

  juce::String entry = "[" + timeStr + "] [SAMPLE CORRUPTION / NAN SPIKE] Ch " +
                       juce::String(channel) + " Sample " + juce::String(sampleIndex) +
                       " Value: " + juce::String(badVal) + "\n";

  pendingGlitchLogText += entry;
}

void MainComponent::flushGlitchLog() {
  if (pendingGlitchLogText.isEmpty())
    return;
  auto appData = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory).getChildFile("Fanfare");
  if (!appData.exists()) appData.createDirectory();
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
    double elapsedMs = juce::Time::highResolutionTicksToSeconds(nowTicks - prevTicks) * 1000.0;
    double expectedMs = (double)numSamples / (engine.getCurrentSampleRate() > 0.0 ? engine.getCurrentSampleRate() : 44100.0) * 1000.0;
    if (expectedMs > 0.0 && elapsedMs > expectedMs * 2.2) {
      const juce::int64 suppressUntil = suppressGlitchUntilTicks.load(std::memory_order_relaxed);
      const juce::int64 applyEndTicks = engine.getLastRigApplyEndTicks();
      const bool postApplySettle =
          (applyEndTicks > 0) &&
          (juce::Time::highResolutionTicksToSeconds(nowTicks - applyEndTicks) < 0.5);

      if (nowTicks < suppressUntil || engine.isRigTransitioning() || postApplySettle) {
        // Suppressed during brief device-open settle or active rig state swap
      } else {
        audioUnderrunCount++;
        lastGlitchStallMs.store(elapsedMs);
        pendingGlitchActualMs.store(elapsedMs);
        pendingGlitchExpectedMs.store(expectedMs);
        pendingGlitchDurationMs.store(lastCallbackDurationMs.load());
        hasPendingGlitchLog.store(true);
      }
    }
  }

  // Per-thread hardening, flag-gated per mechanism (bisect - MinHost runs
  // no MMCSS and no shelter, and is clean where Fanfare storms).
#ifdef _WIN32
  FanfareLog::t_isRealtimeAudioThread = true;
  if constexpr (FanfareConstants::kCpuShelterEnabled) {
    const DWORD_PTR wantedProcMask = processShelterMask.load(std::memory_order_relaxed);
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

    const DWORD_PTR wantedDspMask = dspThreadAffinityMask.load(std::memory_order_relaxed);
    thread_local DWORD lastConfiguredTid = 0;
    thread_local DWORD_PTR lastThreadMask = 0;
    thread_local bool lastPreventEco = false;
    const DWORD curTid = ::GetCurrentThreadId();
    const bool wantPreventEco = engine.isPreventPowerThrottlingEnabled();
    if (curTid != lastConfiguredTid || lastThreadMask != wantedDspMask || lastPreventEco != wantPreventEco) {
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
        LOG_INFO("Audio thread registered with MMCSS Pro Audio characteristics");
      } else {
        // Log the reason - MMCSS refusals were previously invisible.
        DWORD err = ::GetLastError();
        if (err == 1552 /* ERROR_INVALID_THREAD_ID: "already joining a task" */) {
          // Not a failure: ASIO drivers like Yamaha Steinberg USB join their
          // callback thread to the MMCSS Pro Audio task before we run, so the
          // thread already has Pro Audio priority. Don't touch it.
          mmcssThreadId = ::GetCurrentThreadId();
          LOG_INFO("Audio thread already MMCSS-registered by the audio driver - Pro Audio priority active");
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
      LOG_WARN("avrt.dll not loaded - audio thread set to THREAD_PRIORITY_HIGHEST");
    }
  }
  } else {
    // When MMCSS is disabled (avoiding Windows 80% MMCSS throttling), explicitly
    // boost the ASIO callback thread to Kernel Priority 15 so it matches dspWorkers
    // and can never be preempted by Priority 9-12 plugin/UI threads or DSP helpers.
    thread_local DWORD lastBoostedAudioTid = 0;
    const DWORD curAudioTid = ::GetCurrentThreadId();
    if (curAudioTid != lastBoostedAudioTid) {
      lastBoostedAudioTid = curAudioTid;
      SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
      LOG_INFO("Audio callback thread boosted to THREAD_PRIORITY_TIME_CRITICAL (Priority 15)");
    }
  }
#endif

  juce::MidiBuffer incomingMidi;
  midiCollector.removeNextBlockOfMessages(incomingMidi, numSamples);
  engine.processAudio(inputChannelData, numInputChannels, outputChannelData,
                      numOutputChannels, numSamples, incomingMidi);

  lastCallbackDurationMs.store(
      juce::Time::highResolutionTicksToSeconds(
          juce::Time::getHighResolutionTicks() - nowTicks) * 1000.0);
}

void MainComponent::stripLegacyDirectSoundDeviceType() {
  const auto &types = deviceManager.getAvailableDeviceTypes();
  for (int i = types.size() - 1; i >= 0; --i) {
    if (auto *t = types[i]) {
      if (t->getTypeName().containsIgnoreCase("DirectSound") &&
          deviceManager.getCurrentAudioDeviceType() != t->getTypeName()) {
        deviceManager.removeAudioDeviceType(t);
        LOG_INFO("Removed legacy DirectSound device type (prevents USB hotplug scan stalls)");
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
    static LRESULT CALLBACK proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
      if (msg == WM_NCCREATE) {
        auto *cs = reinterpret_cast<CREATESTRUCTW *>(lParam);
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                            reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        return TRUE;
      }
      if (msg == WM_DEVICECHANGE) {
        if (wParam == DBT_DEVNODES_CHANGED ||
            wParam == DBT_DEVICEARRIVAL ||
            wParam == DBT_DEVICEREMOVECOMPLETE) {
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
  if (::RegisterClassExW(&wc) == 0 && ::GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
    LOG_WARN("USB watcher: RegisterClassExW failed (err " +
             juce::String((int)::GetLastError()) + ")");
    return;
  }

  // Hidden top-level popup window (receives both WM_DEVICECHANGE broadcasts
  // and RegisterDeviceNotificationW interface arrival/removal messages).
  HWND hwnd = ::CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, kClassName,
                                L"", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr,
                                hInst, this);
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
    const bool isEnabled = deviceManager.isMidiInputDeviceEnabled(in.identifier);
    if (wasActive && isEnabled) {
      nextActiveIds.add(in.identifier);
      continue;
    }

    auto retryIt = midiOpenRetryAfterMs.find(in.identifier);
    if (!isInitialSetup && retryIt != midiOpenRetryAfterMs.end() && nowMs < retryIt->second) {
      continue;
    }

    suppressHotplugChangeBroadcastUntilMs = nowMs + 500;
    if (isEnabled && !wasActive) {
      deviceManager.setMidiInputDeviceEnabled(in.identifier, false);
    }
    deviceManager.setMidiInputDeviceEnabled(in.identifier, true);
    mutatedDeviceManager = true;

    const bool nowEnabled = deviceManager.isMidiInputDeviceEnabled(in.identifier);
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
                 "' is listed by Windows but unavailable (offline or in use) - backing off auto-enable.");
      } else if (fails == 2) {
        midiOpenRetryAfterMs[in.identifier] = nowMs + 20000;
      } else if (fails == 3) {
        midiOpenRetryAfterMs[in.identifier] = nowMs + 60000;
      } else {
        midiOpenRetryAfterMs[in.identifier] = (std::numeric_limits<juce::int64>::max)();
        if (fails == 4) {
          LOG_WARN("MIDI input '" + in.name +
                   "' failed to open 4 times - quarantining until USB hardware change or manual Reset Audio.");
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
    lastAudioRecoveryAttemptMs = (juce::int64)juce::Time::getMillisecondCounter() - 1900;
  }
}

void MainComponent::checkAndRecoverAudioDevice() {
  const juce::int64 nowCheckMs = (juce::int64)juce::Time::getMillisecondCounter();
  const bool topologyChanged = usbTopologyDirty.exchange(false, std::memory_order_acq_rel);
#ifdef _WIN32
  const int winMmCount = (int)::midiInGetNumDevs();
  const bool midiCountChanged = (winMmCount != lastWinMmMidiInCount);
#else
  const bool midiCountChanged = false;
#endif

  // If a genuine OS hardware event fired (with a 30s cooldown against noisy hubs),
  // allow quarantined virtual/Bluetooth ports one fresh probe.
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
    if (kv.second < (std::numeric_limits<juce::int64>::max)() && nowCheckMs >= kv.second) {
      retryTimerDue = true;
      break;
    }
  }

  // Event-driven hotplug: never call MidiInput::getAvailableDevices() during steady-state
  // playback unless OS USB topology changed, WinMM device count changed, a finite retry
  // backoff matured, or the audio interface itself is in recovery mode.
  if (topologyChanged || midiCountChanged || retryTimerDue || inAudioRecoveryMode) {
    syncHotplugMidiInputs(false);
  }

  if (!hasDesiredAudioSetup || isRecoveringAudioDevice ||
      activeAudioSelector != nullptr || (transitioner != nullptr && transitioner->isBusy()))
    return;

  auto *dev = deviceManager.getCurrentAudioDevice();
  const juce::int64 nowTicks = juce::Time::getHighResolutionTicks();
  const juce::int64 lastCbTicks = lastAudioCallbackTicks.load(std::memory_order_relaxed);
  const double msSinceLastCallback =
      (lastCbTicks > 0)
          ? juce::Time::highResolutionTicksToSeconds(nowTicks - lastCbTicks) * 1000.0
          : 0.0;

  const bool deviceMissingOrClosed =
      (dev == nullptr) || !dev->isOpen() || !dev->isPlaying() ||
      dev->getActiveOutputChannels().isZero();
  const bool callbackStalled =
      (dev != nullptr && dev->isOpen() && lastCbTicks > 0 && msSinceLastCallback > 1500.0);

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
             juce::String(deviceMissingOrClosed ? "device closed" : "callbacks stalled") +
             ") - entering USB auto-recovery mode for '" +
             desiredAudioSetup.outputDeviceName + "'");
    return;
  }

  const juce::int64 retryIntervalMs = (audioRecoveryAttemptCount < 3) ? 2000 : 3000;
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
  if (err.isEmpty() && newDev != nullptr && newDev->isOpen() && newDev->isPlaying()) {
    inAudioRecoveryMode = false;
    audioRecoveryAttemptCount = 0;
    syncHotplugMidiInputs(false);
    LOG_INFO("USB Audio Auto-Recovery succeeded: '" + newDev->getName() + "' is back online!");
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
    const bool isCurrentlyActive = id.isNotEmpty() && currentlyActiveIds.contains(id);
    if (!isCurrentlyActive && name.isNotEmpty() && seenOfflineNames.contains(name)) {
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
  if (dev == nullptr || !dev->isOpen() || dev->getActiveOutputChannels().isZero())
    return;
  captureDesiredAudioSetup();
  auto xml = deviceManager.createStateXml();
  if (xml != nullptr) {
    deduplicateMidiInputsXml(*xml, activeMidiInputIds, midiOpenFailCounts);
    auto file = FanfareConstants::getAppDirectory().getChildFile("audio_settings.xml");
    file.getParentDirectory().createDirectory();
    xml->writeTo(file);
  }
}

void MainComponent::loadAudioSettings() {
  auto file = FanfareConstants::getAppDirectory().getChildFile("audio_settings.xml");
  if (file.existsAsFile()) {
    auto xml = juce::XmlDocument::parse(file);
    if (xml != nullptr) {
      if (xml->hasTagName("DEVICESETUP")) {
        deduplicateMidiInputsXml(*xml, {}, midiOpenFailCounts);
        desiredAudioDeviceType = xml->getStringAttribute("deviceType", "ASIO");
        juce::String devName = xml->getStringAttribute("audioDeviceName");
        desiredAudioSetup.outputDeviceName = devName.isNotEmpty()
            ? devName
            : xml->getStringAttribute("audioOutputDeviceName");
        desiredAudioSetup.inputDeviceName = devName.isNotEmpty()
            ? devName
            : xml->getStringAttribute("audioInputDeviceName");
        desiredAudioSetup.sampleRate = xml->getDoubleAttribute("audioDeviceRate", 44100.0);
        desiredAudioSetup.bufferSize = xml->getIntAttribute("audioDeviceBufferSize", 192);
        desiredAudioSetup.inputChannels.parseString(xml->getStringAttribute("audioDeviceInChans", "11"), 2);
        desiredAudioSetup.outputChannels.parseString(xml->getStringAttribute("audioDeviceOutChans", "11"), 2);
        desiredAudioSetup.useDefaultInputChannels = !xml->hasAttribute("audioDeviceInChans");
        desiredAudioSetup.useDefaultOutputChannels = !xml->hasAttribute("audioDeviceOutChans");
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
        float lum = juce::jmax(c.getRed(),
                               juce::jmax(c.getGreen(), c.getBlue())) / 255.0f;
        px.setPixelColour(
            x, y, c.withAlpha((juce::uint8)juce::jlimit(
                      0, 255, (int)((lum - 0.08f) * 1.6f * 255.0f))));
      }
  }

  lastWaltzLogoImage = use;
  lastWaltzLogoComponent.setImage(
      lastWaltzLogoImage, juce::RectanglePlacement::onlyReduceInSize |
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
  if (auto* obj = v.getDynamicObject()) {
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
    engineTuningSettings.preventPowerThrottling = (bool)v["preventPowerThrottling"];
  if (v.hasProperty("parallelProcessing"))
    engineTuningSettings.parallelProcessing = (bool)v["parallelProcessing"];
  if (v.hasProperty("preferredStripWidth")) {
    preferredExpandedWidth = juce::jlimit(
        kMinExpandedWidth, kMaxExpandedWidth, (int)v["preferredStripWidth"]);
    stripWidthSlider.setValue(preferredExpandedWidth, juce::dontSendNotification);
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
  if (auto* obj = v.getDynamicObject()) {
    obj->setProperty("pCoresOnly", engineTuningSettings.pCoresOnly);
    obj->setProperty("shelterCpu0", engineTuningSettings.shelterCpu0);
    obj->setProperty("preventPowerThrottling", engineTuningSettings.preventPowerThrottling);
    obj->setProperty("parallelProcessing", engineTuningSettings.parallelProcessing);
  }
  f.getParentDirectory().createDirectory();
  f.replaceWithText(juce::JSON::toString(v));
}

void MainComponent::suppressGlitchDetectionForMs(int durationMs) noexcept {
  const juce::int64 now = juce::Time::getHighResolutionTicks();
  const juce::int64 delta = juce::Time::secondsToHighResolutionTicks(durationMs * 0.001);
  const juce::int64 target = now + delta;
  juce::int64 prev = suppressGlitchUntilTicks.load(std::memory_order_relaxed);
  while (prev < target &&
         !suppressGlitchUntilTicks.compare_exchange_weak(prev, target, std::memory_order_release, std::memory_order_relaxed)) {
  }
}

void MainComponent::applyEngineTuningSettings() {
#ifdef _WIN32
  const CpuTopologyMasks topo = queryCpuTopology();
  const DWORD_PTR fullSysMask = (topo.sysMask != 0) ? topo.sysMask : ~(DWORD_PTR)0;

  // 1. PROCESS AFFINITY: Shelter ONLY CPU 0 (if enabled) while keeping ALL Efficiency
  //    cores open so Windows + Intel Thread Director steer Kontakt disk threads,
  //    Roland background threads, Direct2D GUI, and USB ASIO helper threads off the P-cores!
  DWORD_PTR procMaskToApply = topo.sysMask;
  if (engineTuningSettings.shelterCpu0 && countBitsInMask(procMaskToApply & ~(DWORD_PTR)1) >= 1) {
    procMaskToApply &= ~(DWORD_PTR)1;
  }
  if (procMaskToApply != 0) {
    if (SetProcessAffinityMask(GetCurrentProcess(), procMaskToApply)) {
      processShelterMask.store(procMaskToApply, std::memory_order_release);
    }
  }

  // 2. REAL-TIME AUDIO & DSP WORKER THREAD AFFINITY: Lock exclusively to fast P-cores
  //    (excluding CPU 0 when sheltered).
  DWORD_PTR rtDspMask = (engineTuningSettings.pCoresOnly && topo.pCoreMask != 0)
                            ? topo.pCoreMask
                            : fullSysMask;
  if (engineTuningSettings.shelterCpu0 && countBitsInMask(rtDspMask & ~(DWORD_PTR)1) >= 1) {
    rtDspMask &= ~(DWORD_PTR)1;
  }
  rtDspMask &= procMaskToApply;
  if (rtDspMask == 0)
    rtDspMask = procMaskToApply;

  // Count distinct physical cores represented in rtDspMask so we assign at most
  // (physicalCores - 1) parallel DSP helpers (+ Audio Thread = 1 worker per physical core!),
  // and pin the ASIO callback thread and each DSP helper onto separate physical cores
  // so two Priority-15 real-time threads never collide on Hyper-Threads of the same P-core.
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
    // Give the ASIO master callback thread the last (unsheltered, full) physical P-core,
    // and assign each DSP helper its own separate physical P-core slice.
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
  //    On hybrid CPUs with P-Cores Only enabled, steer the UI thread and background
  //    RigBuilder / SetlistPreloader threads onto Efficiency cores so background song
  //    preloads and UI repaints never touch the real-time P-cores!
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
           juce::String::toHexString((juce::int64)primaryHelperMask) +
           " (" + juce::String(dspPhysicalCores) + " phys P-cores, " +
           juce::String(helperLimit) + " helper + audio thread)" +
           " | UI/Background: 0x" +
           juce::String::toHexString((juce::int64)bgMask));

  // 4. Windows 11 Process EcoQoS (Power Throttling)
  PROCESS_POWER_THROTTLING_STATE powerThrottling{};
  powerThrottling.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
  powerThrottling.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
  powerThrottling.StateMask =
      engineTuningSettings.preventPowerThrottling ? 0UL : PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
  if (SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling,
                            &powerThrottling, sizeof(powerThrottling))) {
    LOG_INFO("Windows 11 EcoQoS Power Throttling set to: " +
             juce::String(engineTuningSettings.preventPowerThrottling ? "Disabled (HighQoS)" : "Default"));
  }
#endif

  engine.setParallelProcessingEnabled(engineTuningSettings.parallelProcessing);
  LOG_INFO("DSP Processing pipeline set to: " +
           juce::String(engineTuningSettings.parallelProcessing ? "Parallel Work-Sharing" : "Sequential Inline"));
}

void MainComponent::showEngineTuningModal() {
  if (configOverlay) {
    configOverlay.reset();
  }
  engineTuningModal = std::make_unique<EngineTuningModal>(
    engineTuningSettings,
    [this](const EngineTuningModal::Settings& s) {
      engineTuningSettings = s;
      saveEngineTuningSettings();
      applyEngineTuningSettings();
    },
    [this] {
      engineTuningModal.reset();
      resized();
    });
  addAndMakeVisible(engineTuningModal.get());
  engineTuningModal->centreWithSize(juce::jmin(getWidth() - 32, 640),
                                    juce::jmin(getHeight() - 32, 560));
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
  auto setup = (device != nullptr && device->isOpen())
                   ? deviceManager.getAudioDeviceSetup()
                   : (hasDesiredAudioSetup ? desiredAudioSetup : deviceManager.getAudioDeviceSetup());
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
                           setup.outputChannels.countNumberOfSetBits(), nullptr,
                           true, "", &setup);
  }
}

void MainComponent::showAboutDialog() {
  auto *w = new juce::AlertWindow(
      "About Fanfare",
      "",
      juce::MessageBoxIconType::InfoIcon);

  class AboutCustomComponent : public juce::Component {
  public:
    AboutCustomComponent() {
      infoLabel.setText(
          "Fanfare Live Performance Host  |  v" OPENRIG_VERSION_STRING "\n"
          "The Sovereign Stage Engine for Keyboardists\n\n"
          "Built for one job: getting you through a live set without a single dropout, stuck note, or fumbled song switch.",
          juce::dontSendNotification);
      infoLabel.setFont(juce::FontOptions(13.5f));
      infoLabel.setJustificationType(juce::Justification::topLeft);
      addAndMakeVisible(infoLabel);

      statementLabel.setText("Design decisions for this app are strictly guided by ", juce::dontSendNotification);
      statementLabel.setFont(juce::FontOptions(13.5f));
      statementLabel.setJustificationType(juce::Justification::topLeft);
      addAndMakeVisible(statementLabel);

      linkButton.setButtonText("this philosophy.");
      linkButton.setURL(juce::URL("https://youtu.be/WPc-VEqBPHI?si=H_G9lJGNPGONcPLP&t=108"));
      linkButton.setColour(juce::HyperlinkButton::textColourId, ThemeManager::get(Theme::Role::accent));
      linkButton.setFont(juce::FontOptions(13.5f, juce::Font::underlined), false, juce::Justification::topLeft);
      addAndMakeVisible(linkButton);

      footerLabel.setText("No virtual patch cords. No node graphs. Just pure stage-tested horsepower.", juce::dontSendNotification);
      footerLabel.setFont(juce::FontOptions(12.0f, juce::Font::italic));
      footerLabel.setColour(juce::Label::textColourId, ThemeManager::get(Theme::Role::textDim));
      footerLabel.setJustificationType(juce::Justification::topLeft);
      addAndMakeVisible(footerLabel);

      setSize(470, 150);
    }

    void resized() override {
      auto bounds = getLocalBounds();
      infoLabel.setBounds(bounds.removeFromTop(75));

      auto lineBounds = bounds.removeFromTop(22);
      juce::Font font(juce::FontOptions(13.5f));
      juce::String text = "Design decisions for this app are strictly guided by ";
      
      juce::GlyphArrangement ga1;
      ga1.addLineOfText(font, text, 0.0f, 0.0f);
      int prefixW = juce::roundToInt(ga1.getBoundingBox(0, -1, true).getWidth()) + 2;
      statementLabel.setBounds(lineBounds.getX(), lineBounds.getY(), prefixW, lineBounds.getHeight());

      juce::GlyphArrangement ga2;
      ga2.addLineOfText(font, "this philosophy.", 0.0f, 0.0f);
      int linkW = juce::roundToInt(ga2.getBoundingBox(0, -1, true).getWidth()) + 10;
      linkButton.setBounds(lineBounds.getX() + prefixW, lineBounds.getY() - 1, linkW, lineBounds.getHeight());

      bounds.removeFromTop(10);
      footerLabel.setBounds(bounds.removeFromTop(20));
    }

  private:
    juce::Label infoLabel;
    juce::Label statementLabel;
    juce::HyperlinkButton linkButton;
    juce::Label footerLabel;
  };

  w->addCustomComponent(new AboutCustomComponent());
  w->addButton("OK", 1, juce::KeyPress(juce::KeyPress::returnKey), juce::KeyPress(juce::KeyPress::escapeKey));

  juce::Component::SafePointer<juce::AlertWindow> safeW(w);
  w->enterModalState(
      true,
      juce::ModalCallbackFunction::create([safeW](int) {
        if (safeW != nullptr)
          delete safeW.getComponent();
      }));
}

void MainComponent::showQuickStartDialog(bool isFirstRunPrompt) {
  juce::String title = isFirstRunPrompt
      ? "Welcome to Fanfare! (First-Run Setup)"
      : "First Steps: Quick-Start Guide";

  juce::String body;
  if (isFirstRunPrompt) {
    body << "It looks like this is your first time running Fanfare on this computer.\n\n"
         << "Would you like Fanfare to scan your main VST3 folder now?\n"
         << "(" << FanfareConstants::getVst3Directory().getFullPathName() << ")\n\n";
  }

  body << "GETTING SOUND IN TWO STEPS:\n\n"
       << "1. Scan for VST3s\n"
       << "   Open Rig -> Scan for plugins (or click 'Scan VST3 Folder Now' below) so Fanfare registers your installed instruments & effects. Choose your ASIO audio driver & MIDI keyboard in Rig -> Audio & MIDI settings.\n\n"
       << "2. Add a VST to a Strip Slot\n"
       << "   On any channel strip (start with Strip 2: Keyboard), click one of the 3 plugin slots (Slot 1, 2, or 3) to load an instrument, and click [E] to open its editor window.\n\n"
       << "Tip: Need help on any screen? Click the Help button (or press F1) to jump straight to that chapter of the User Manual.";

  auto *w = new juce::AlertWindow(title, body, juce::MessageBoxIconType::InfoIcon);
  w->addButton("Scan VST3 Folder Now", 1, juce::KeyPress(juce::KeyPress::returnKey));
  w->addButton("Audio & MIDI Settings", 2);
  w->addButton("Close", 0, juce::KeyPress(juce::KeyPress::escapeKey));

  juce::Component::SafePointer<juce::AlertWindow> safeW(w);
  juce::Component::SafePointer<MainComponent> safeThis(this);
  w->enterModalState(
      true,
      juce::ModalCallbackFunction::create([safeW, safeThis](int result) {
        if (safeW != nullptr)
          delete safeW.getComponent();
        if (safeThis == nullptr)
          return;
        if (result == 1) {
          if (safeThis->scanButton.onClick)
            safeThis->scanButton.onClick();
        } else if (result == 2) {
          if (safeThis->audioSettingsBtn.onClick)
            safeThis->audioSettingsBtn.onClick();
        }
      }));
}

void MainComponent::showSlotMidiOutDialog(int slotIdx, int chainIdx) {
  auto *slot = engine.getSlot(slotIdx);
  if (slot == nullptr)
    return;

  auto *w = new juce::AlertWindow(
      "MIDI OUT Destination",
      "Forward this chain slot's MIDI (after note-range + transpose filtering) "
      "to a hardware/software output device instead of a plugin.",
      juce::MessageBoxIconType::NoIcon);

  auto devices = juce::MidiOutput::getAvailableDevices();
  juce::StringArray deviceNames;
  juce::StringArray deviceIds;
  deviceNames.add("(none)");
  deviceIds.add("");
  for (const auto &d : devices) {
    deviceNames.add(d.name);
    deviceIds.add(d.identifier);
  }

  juce::String currentId = slot->getChainSlotMidiOutIdentifier(chainIdx);
  int curDev = juce::jmax(0, deviceIds.indexOf(currentId));

  w->addComboBox("device", deviceNames, "Output device:");
  w->getComboBoxComponent("device")->setSelectedItemIndex(curDev,
                                                          juce::dontSendNotification);

  juce::StringArray channels;
  for (int c = 1; c <= 16; ++c)
    channels.add("Channel " + juce::String(c));
  w->addComboBox("channel", channels, "MIDI channel:");
  w->getComboBoxComponent("channel")->setSelectedItemIndex(
      juce::jlimit(0, 15, slot->getChainSlotMidiOutChannel(chainIdx) - 1),
      juce::dontSendNotification);

  w->addButton("OK", 1, juce::KeyPress(juce::KeyPress::returnKey));
  w->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));

  juce::Component::SafePointer<juce::AlertWindow> safeW(w);
  w->enterModalState(
      true,
      juce::ModalCallbackFunction::create(
          [this, slotIdx, chainIdx, deviceIds, safeW](int result) {
            if (safeW != nullptr && result == 1) {
              int devSel = safeW->getComboBoxComponent("device")->getSelectedItemIndex();
              int ch = safeW->getComboBoxComponent("channel")->getSelectedItemIndex() + 1;
              if (devSel <= 0) {
                engine.clearSlotChainMidiOut(slotIdx, chainIdx);
              } else {
                engine.setSlotChainMidiOut(slotIdx, chainIdx,
                                            deviceIds[devSel], ch);
                if (slotIdx >= 0 && slotIdx < rackSlotComponents.size() &&
                    rackSlotComponents[slotIdx]->getIsCollapsed()) {
                  rackSlotComponents[slotIdx]->setCollapsed(false);
                  resized();
                }
              }
              repaint();
            }
            if (safeW != nullptr)
              delete safeW.getComponent();
          }));
}

void MainComponent::showConfigOverlay() {
  // Safe invoker: only call a button's onClick if it has been wired.
  auto invoke = [](juce::Button &b) {
    if (b.onClick)
      b.onClick();
  };

  ConfigModalOverlay::Actions a;
  a.onAudioSettings = [this, invoke] { invoke(audioSettingsBtn); };
  a.onEngineTuning = [this] { showEngineTuningModal(); };
  a.onScanPlugins = [this, invoke] { invoke(scanButton); };
  a.onBusRouting = [this, invoke] { invoke(busRoutingBtn); };
  a.onResetAudio = [this, invoke] { invoke(resetAudioBtn); };
  a.onAbout = [this] { showAboutDialog(); };
  a.onClose = [this] {
    configOverlay.reset();
    resized();
  };
  a.onStressTestToggle = [this] {
    engine.stressTestActive.store(!engine.stressTestActive.load());
  };
  a.isStressTestActive = [this] {
    return engine.stressTestActive.load();
  };
  a.getInfo = [this] {
    juce::String s;
    s << "Fanfare v" << OPENRIG_VERSION_STRING << "   |   Plugins: " << engine.getNumAvailablePlugins() << "\n";
    int ch = engine.getDefaultMidiChannel();
    s << "Global MIDI: "
      << (ch == 0 ? juce::String("Omni") : juce::String(ch)) << "\n";
    int foh = engine.getFohOutputOffset();
    int iem = engine.getIemOutputOffset();
    s << "FOH out: " << (foh + 1) << "+" << (foh + 2) << "   "
      << "IEM out: " << (iem + 1) << "+" << (iem + 2);
    return s;
  };

  configOverlay = std::make_unique<ConfigModalOverlay>(std::move(a));
  addAndMakeVisible(configOverlay.get());
  configOverlay->centreWithSize(configOverlay->getWidth(),
                                configOverlay->getHeight());
}

void MainComponent::showSetupBuilderOverlay() {
  if (!setupBuilderOverlay) {
    SetupBuilderOverlay::Actions a;
    a.onBuildComplete = [this](const juce::File& file) {
      setupBuilderOverlay.reset();
      resized();
      loadRigFromFile(file);
    };
    a.onClose = [this] {
      setupBuilderOverlay.reset();
      resized();
    };
    setupBuilderOverlay = std::make_unique<SetupBuilderOverlay>(a);
    addAndMakeVisible(setupBuilderOverlay.get());
  }
  setupBuilderOverlay->centreWithSize(setupBuilderOverlay->getWidth(),
                                      setupBuilderOverlay->getHeight());
  setupBuilderOverlay->toFront(true);
}

void MainComponent::changeListenerCallback(juce::ChangeBroadcaster* source) {
  if (source == &deviceManager) {
    const juce::int64 nowMs = (juce::int64)juce::Time::getMillisecondCounter();
    if (isSyncingMidiInputs || isRecoveringAudioDevice)
      return;
    if (activeAudioSelector == nullptr && nowMs < suppressHotplugChangeBroadcastUntilMs)
      return;
    if (activeAudioSelector == nullptr)
      syncHotplugMidiInputs(false);
    saveAudioSettings();
  } else if (source == &ThemeManager::getInstance()) {
    // Theme changed: re-apply LookAndFeel colours, propagate lookAndFeelChanged()
    // to every ComboBox/Slider/TextEditor/DialogWindow on the desktop, and repaint.
    boutiqueLookAndFeel.applyThemeColors();
    refreshThemeDependentStyles();
    sendLookAndFeelChange();
    auto& desktop = juce::Desktop::getInstance();
    for (int i = 0; i < desktop.getNumComponents(); ++i) {
      if (auto* comp = desktop.getComponent(i)) {
        comp->sendLookAndFeelChange();
        comp->repaint();
      }
    }
    repaint();
    for (auto* c : getChildren())
      c->repaint();
  } else if (source == &Fanfare::SetlistManager::getInstance()) {
    updatePreloadStatus();
    if (webServer)
      webServer->notifySetlistChanged();
  }
}

void MainComponent::updatePreloadStatus() {
  auto& sm = Fanfare::SetlistManager::getInstance();
  juce::File preloadTarget = sm.getNextPreloadTarget();
  int activeIdx = sm.getActiveIndex();

  for (int i = 0; i < queueButtons.size(); ++i) {
    if (i < numSetupButtons) {
      juce::File slotFile(setupFilePaths[i]);
      queueButtons[i]->setAssignedFile(slotFile);

      bool isActive = (i == activeIdx) || (activeIdx < 0 && i == currentSetupIndex && slotFile.existsAsFile());
      bool isPreloaded = sm.isPreloaded() && (preloadTarget == slotFile) && slotFile.existsAsFile();
      bool isPreloading = sm.isPreloading() && (preloadTarget == slotFile) && slotFile.existsAsFile();

      queueButtons[i]->setSlotStates(isActive, isPreloaded, isPreloading);
    }
  }

  if (preloadTarget.existsAsFile()) {
    preloadStatusLabel.setVisible(true);
    juce::String nextName = preloadTarget.getFileNameWithoutExtension();
    if (sm.isPreloading()) {
      preloadStatusLabel.setText("Preloading next: " + nextName + "...", juce::dontSendNotification);
      preloadStatusLabel.setColour(juce::Label::textColourId, ThemeManager::get(Theme::Role::warn));
    } else if (sm.isPreloaded()) {
      preloadStatusLabel.setText("Next Ready: " + nextName, juce::dontSendNotification);
      preloadStatusLabel.setColour(juce::Label::textColourId, ThemeManager::get(Theme::Role::ok));
    } else if (sm.isPreloadFailed()) {
      preloadStatusLabel.setText("Preload Failed: " + nextName, juce::dontSendNotification);
      preloadStatusLabel.setColour(juce::Label::textColourId, ThemeManager::get(Theme::Role::danger));
    } else {
      preloadStatusLabel.setText("Next: " + nextName + " (Not Preloaded)", juce::dontSendNotification);
      preloadStatusLabel.setColour(juce::Label::textColourId, ThemeManager::get(Theme::Role::textDim));
    }
  } else {
    preloadStatusLabel.setText("", juce::dontSendNotification);
    preloadStatusLabel.setVisible(false);
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
  const auto& t = ThemeManager::getInstance().active();

  auto gearNorm = createGearImage(24, t.textDim);
  auto gearOver = createGearImage(24, t.accent);
  settingsGearBtn.setImages(false, true, true,
                            gearNorm, 1.0f, juce::Colours::transparentBlack,
                            gearOver, 1.0f, juce::Colours::transparentBlack,
                            gearOver, 0.85f, juce::Colours::transparentBlack);

  setupNameLabel.setColour(juce::Label::textColourId, t.text);
  stripWidthLabel.setColour(juce::Label::textColourId, t.engrave);
  midiMonitorLabel.setColour(juce::Label::textColourId, t.midiNote);
  cpuLabel.setColour(juce::Label::textColourId, t.text);
  ramLabel.setColour(juce::Label::textColourId, t.textDim);
  latencyLabel.setColour(juce::Label::textColourId, t.textDim);
  xrunsLabel.setColour(juce::Label::textColourId,
                       (audioUnderrunCount.load() + engineStallCount.load() > 0) ? t.warn : t.ok);
  thermalLabel.setColour(juce::Label::textColourId, juce::Colours::white);
  clockLabel.setColour(juce::Label::textColourId, t.isStudio() ? t.textDim : t.accent);
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
  const auto& t = ThemeManager::getInstance().active();
  g.fillAll(t.canvas);

  // Top bar
  if (!stageHeaderBounds.isEmpty()) {
    g.setColour(t.isStudio() ? t.panelAlt : t.panel);
    g.fillRect(stageHeaderBounds);
    g.setColour(t.isStudio() ? t.border : t.hairline);
    g.drawHorizontalLine(stageHeaderBounds.getBottom() - 1, 0.0f, (float)getWidth());

    // Song title capsule (Nota-style grouped control)
    if (setupNameLabel.isVisible()) {
      auto cap = prevSetlistBtn.getBounds().getUnion(nextSetlistBtn.getBounds())
                     .getUnion(setupNameLabel.getBounds()).toFloat().expanded(3.0f, 1.0f);
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
      g.fillRoundedRectangle(logoComponent.getBounds().toFloat().expanded(1.0f), 3.0f);
    }
  }

  // Scene bar
  if (!sceneRowBounds.isEmpty()) {
    g.setColour(t.isStudio() ? t.panel : t.sunken);
    g.fillRect(sceneRowBounds);
    g.setColour(t.isStudio() ? t.border : t.hairlineStrong);
    g.drawHorizontalLine(sceneRowBounds.getBottom() - 1, 0.0f, (float)getWidth());
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
    for (auto* lbl : {&cpuLabel, &ramLabel, &latencyLabel})
      if (!lbl->getBounds().isEmpty())
        g.drawVerticalLine(lbl->getRight(), (float)statusBarBounds.getY() + 5.0f,
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
    g.fillRect(fohHdr.getX(), fohHdr.getBottom() - 2.0f, fohHdr.getWidth(), 2.0f);
    g.setColour(t.iem);
    g.fillRect(iemHdr.getX(), iemHdr.getBottom() - 2.0f, iemHdr.getWidth(), 2.0f);
  }

  // Segmented Master FOH & IEM Stereo LED Meters
  auto fohBounds = masterFohSlider.getBounds().toFloat();
  if (!fohBounds.isEmpty()) {
    auto fohMeter = juce::Rectangle<float>(fohBounds.getRight() + 2.0f, fohBounds.getY() + 4.0f,
                                           13.0f, juce::jmax(16.0f, fohBounds.getHeight() - 8.0f));
    int segs = juce::jlimit(12, 24, (int)(fohMeter.getHeight() / 9.0f));
    BoutiqueLookAndFeel::drawStereoSegmentedMeter(g, fohMeter, masterFohL, masterFohR,
                                                  segs, t.meterLow, t.meterMid, t.meterPeak);
  }

  auto iemBounds = masterIemSlider.getBounds().toFloat();
  if (!iemBounds.isEmpty()) {
    auto iemMeter = juce::Rectangle<float>(iemBounds.getRight() + 2.0f, iemBounds.getY() + 4.0f,
                                           13.0f, juce::jmax(16.0f, iemBounds.getHeight() - 8.0f));
    int segs = juce::jlimit(12, 24, (int)(iemMeter.getHeight() / 9.0f));
    BoutiqueLookAndFeel::drawStereoSegmentedMeter(g, iemMeter, masterIemL, masterIemR,
                                                  segs, t.iem, t.meterMid, t.meterPeak);
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
    menuBar->setBounds(sh.removeFromLeft(juce::jmin(250, sh.getWidth() / 4)).reduced(0, 7));
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
    if (!sceneButtons.isEmpty()) {
      const int n = sceneButtons.size();
      const int maxW = 180;
      for (int i = 0; i < n; ++i) {
        int w = juce::jmin(maxW, sr.getWidth() / (n - i));
        sceneButtons[i]->setBounds(sr.removeFromLeft(w).reduced(2, 0));
      }
    }
  }

  // Bottom status bar
  statusBarBounds = r.removeFromBottom(22);
  {
    auto sb = statusBarBounds.reduced(8, 0);
    midiMonitorLabel.setBounds(sb.removeFromLeft(juce::jmin(200, sb.getWidth() / 5)));
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

  // 4. Master Faders (leaving 16px on each bus's right side for its stereo segmented meter)
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
    const int expandedWidth =
        juce::jlimit(kMinExpandedWidth, kMaxExpandedWidth, preferredExpandedWidth);

    // 1. Pin Aux Returns on the right: collapsed Aux Returns shrink to 28px!
    const int returnExpandedW =
        juce::jlimit(68, 84, juce::jmax(68, r.getWidth() / 11));
    if (numReturns > 0) {
      int returnsTotalW = 0;
      for (auto *c : auxReturnComponents)
        returnsTotalW += c->getIsCollapsed() ? mutedCollapsedWidth : returnExpandedW;

      auto returnsArea =
          r.removeFromRight(juce::jmin(r.getWidth() / 2, returnsTotalW));
      juce::Rectangle<int> lwLogoArea;
      for (auto *c : auxReturnComponents) {
        const int cw = c->getIsCollapsed() ? mutedCollapsedWidth : returnExpandedW;
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

    // 2. When Stage Compact is ON, collapse all unused empty strips into a single
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
      for (int i = lastUsedOrExpandedIdx + 1; i < rackSlotComponents.size(); ++i) {
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
        const int cw = isEmptyCollapsed ? emptyCollapsedWidth : mutedCollapsedWidth;
        totalStripsWidth += comp->getIsCollapsed() ? cw : expandedWidth;
      }
    }
    if (rackSlotComponents.size() > 0) {
      auto *comp = rackSlotComponents[0];
      comp->setVisible(true);
      totalStripsWidth += comp->getIsCollapsed() ? mutedCollapsedWidth : expandedWidth;
    }

    // 3. Dedicated high-contrast bottom navigation bar [ < ] [===] [ > ] when needed
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
      const int cw = isEmptyCollapsed ? emptyCollapsedWidth : mutedCollapsedWidth;
      const int w = comp->getIsCollapsed() ? cw : expandedWidth;
      comp->setBounds(juce::Rectangle<int>(xPos, 0, w, stripAreaH).reduced(1, 0));
      xPos += w;
    }

    // Place Monitor In (index 0) after the main channels, adjacent to Aux Returns
    if (rackSlotComponents.size() > 0) {
      auto *comp = rackSlotComponents[0];
      const int w = comp->getIsCollapsed() ? mutedCollapsedWidth : expandedWidth;
      comp->setBounds(juce::Rectangle<int>(xPos, 0, w, stripAreaH).reduced(1, 0));
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
  if (mp3Overlay)
    mp3Overlay->centreWithSize(780, 580);
  if (setupBuilderOverlay)
    setupBuilderOverlay->centreWithSize(400, 480);
  if (resourceInspectorOverlay)
    resourceInspectorOverlay->centreWithSize(740, 520);
  if (engineTuningModal)
    engineTuningModal->centreWithSize(juce::jmin(getWidth() - 32, 640), juce::jmin(getHeight() - 32, 560));
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
      if (slot->getPluginInstance(c) != nullptr || slot->isChainSlotMidiOut(c)) {
        hasPlugins = true;
        break;
      }
    }
    bool hasInput = (slot->getInputChannelIndex() >= 0);
    bool isBypassed = slot->isBypassed();

    // Active if not muted/bypassed AND has plugins, hardware input, or is the dedicated monitor channel (slot 0)
    bool isActive = !isBypassed && (hasPlugins || hasInput || i == 0);

    comp->setCollapsed(!isActive);
  }

  resized();
}

void MainComponent::showResourceInspectorModal() {
  if (!resourceInspectorOverlay) {
    resourceInspectorOverlay = std::make_unique<ResourceInspectorModal>(engine, [this] {
      resourceInspectorOverlay.reset();
      resized();
    });
    addAndMakeVisible(resourceInspectorOverlay.get());
  }
  resourceInspectorOverlay->centreWithSize(740, 520);
  resourceInspectorOverlay->toFront(true);
}

void MainComponent::showGigNotepadWindow() {
  if (gigNotepadWindow != nullptr) {
    gigNotepadWindow->toFront(true);
    return;
  }
  gigNotepadWindow = std::make_unique<GigNotepadWindow>(engine, [this] {
    gigNotepadWindow.reset();
  });
}

void MainComponent::hideLoadingOverlay() { loadingOverlay.reset(); }

void MainComponent::loadRigAsync(const juce::File &file, int buttonIndexForHighlight, int targetSetlistIndex) {
  if (missingPluginsToast)
    missingPluginsToast.reset();
  if (!transitioner || transitioner->isBusy()) {
    juce::AlertWindow::showMessageBoxAsync(
        juce::MessageBoxIconType::InfoIcon, "Transition Busy",
        "A song transition is currently in progress. Please wait.");
    return;
  }

  closeAllPluginWindows();
  SaveHistory::snapshotBeforeLoadAsync(file);
  showLoadingOverlay("SWITCHING SONG",
                     "Loading " + file.getFileNameWithoutExtension() + "...");

  int highlight = buttonIndexForHighlight;

  // Defer the transition slightly to guarantee that the message thread paints
  // the LoadingOverlay before heavy VST builds block the event loop.
  juce::Timer::callAfterDelay(50, [this, file, highlight, targetSetlistIndex]() {
    if (!transitioner)
      return;

    transitioner->transitionToFile(file,
        Fanfare::RigTransitioner::Callbacks{
            [this](juce::String progress) { setLoadingMessage(progress); },
            [this, highlight, file, targetSetlistIndex, fileName = file.getFileNameWithoutExtension()](bool ok, juce::String message, int builtCount, const std::vector<Fanfare::RigBuilder::FailedEntry>& failed) {
                hideLoadingOverlay();
                closeOrphanedPluginWindows();
                if (ok) {
                    suppressGlitchDetectionForMs(500);
                    audioUnderrunCount.store(0);
                    engineStallCount.store(0);
                    currentRigFile = file;
                    Fanfare::RecentSongs::record(file);
                    setupNameLabel.setText(fileName, juce::dontSendNotification);
                    freshBuildToggle.setToggleState(engine.isCurrentRigFreshBuild(), juce::dontSendNotification);
                    for (auto *comp : rackSlotComponents) {
                        comp->updateOutputSelector();
                        comp->repaint();
                    }
                    repaint();
                    refreshSceneButtons();
                    if (autoCollapseToggle.getToggleState()) {
                        updateStripCollapseStates();
                    } else {
                        for (auto *comp : rackSlotComponents)
                            comp->setCollapsed(false);
                        resized();
                    }
                    // Active-queue highlighting is driven by updatePreloadStatus()
                    // via the SetlistManager change broadcaster (setActiveIndex).
midiMonitorLabel.setText("SONG: " + message,
                                             juce::dontSendNotification);

                    if (!failed.empty()) {
                        showMissingPluginsWarning(failed);
                    }

                    // Update active index in SetlistManager if this file belongs to the setlist
                    auto& sm = Fanfare::SetlistManager::getInstance();
                    if (targetSetlistIndex >= 0) {
                        sm.setActiveIndex(targetSetlistIndex);
                    } else if (sm.getActiveFile() != file) {
                        const auto& setups = sm.getSetups();
                        for (int i = 0; i < setups.size(); ++i) {
                            if (setups[i] == file) {
                                sm.setActiveIndex(i);
                                break;
                            }
                        }
                    }
                    sm.triggerPreloadOfNext();
                } else {
                    // Rollback-by-construction: current rig is untouched.
                    juce::AlertWindow::showMessageBoxAsync(
                        juce::MessageBoxIconType::WarningIcon, "Song Load Failed",
                        message + "\n\nThe current song is unchanged.");
                    LOG_ERROR("Failed to load rig: " + message + " file: " +
                              fileName);
                }
            }
        });
  });
}

void MainComponent::assignJsonToButton(int buttonIndex) {
  auto setupsDir = Fanfare::RigLibrary::getSongsDirectory();
  setupsDir.createDirectory();

  auto chooser = std::make_shared<juce::FileChooser>("Select JSON Song File",
                                                     setupsDir, "*.json");

  chooser->launchAsync(
      juce::FileBrowserComponent::openMode |
          juce::FileBrowserComponent::canSelectFiles,
      [this, buttonIndex, chooser](const juce::FileChooser &fc) {
        auto file = fc.getResult();
        if (file.existsAsFile()) {
          setupFilePaths[buttonIndex] = file.getFullPathName();
          saveButtonMappings();
          auto& sm = Fanfare::SetlistManager::getInstance();
          sm.setSlotSetup(buttonIndex, file);
          updatePreloadStatus();
        }
      });
}

void MainComponent::saveButtonMappings() {
  auto mappingsFile = Fanfare::RigLibrary::buttonMappingsFile();
  mappingsFile.getParentDirectory().createDirectory();

  auto *root = new juce::DynamicObject();
  juce::Array<juce::var> paths;
  for (int i = 0; i < numSetupButtons; ++i) {
    paths.add(setupFilePaths[i]);
  }
  root->setProperty("buttons", paths);

  juce::Array<juce::var> scenePaths;
  for (int i = 0; i < sceneSetupFilePaths.size(); ++i) {
    scenePaths.add(sceneSetupFilePaths[i]);
  }
  root->setProperty("sceneButtons", scenePaths);

  mappingsFile.replaceWithText(juce::JSON::toString(juce::var(root)));
}

void MainComponent::loadButtonMappings() {
  auto mappingsFile = Fanfare::RigLibrary::buttonMappingsFile();

  if (!mappingsFile.existsAsFile())
    return;

  auto json = juce::JSON::parse(mappingsFile.loadFileAsString());
  if (auto *arr = json.getProperty("buttons", juce::var()).getArray()) {
    auto& sm = Fanfare::SetlistManager::getInstance();
    for (int i = 0; i < std::min((int)arr->size(), numSetupButtons); ++i) {
      setupFilePaths[i] = arr->getReference(i).toString();
      if (!setupFilePaths[i].isEmpty()) {
        sm.setSlotSetup(i, juce::File(setupFilePaths[i]));
      }
    }
  }

  if (auto *sceneArr = json.getProperty("sceneButtons", juce::var()).getArray()) {
    sceneSetupFilePaths.clear();
    for (int i = 0; i < sceneArr->size(); ++i) {
      sceneSetupFilePaths.add(sceneArr->getReference(i).toString());
    }
  }

  updatePreloadStatus();
}

void MainComponent::timerCallback() {
  double cpu = deviceManager.getCpuUsage() * 100.0;
  thermalMonitor.setAudioCpuLoadPercent(cpu);
  cpuLabel.setText("CPU: " + juce::String(cpu, 1) + "%",
                   juce::dontSendNotification);

  static int memUpdateTicks = 19;
  if (++memUpdateTicks >= 20) {
    memUpdateTicks = 0;
#ifdef _WIN32
    if constexpr (FanfareConstants::kPriorityElevationEnabled) {
      const DWORD curPri = GetPriorityClass(GetCurrentProcess());
      if (curPri != 0 && curPri != ABOVE_NORMAL_PRIORITY_CLASS) {
        SetPriorityClass(GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS);
        LOG_INFO("Driver/plugin altered PriorityClass (0x" + juce::String::toHexString((int)curPri) + ") - restored ABOVE_NORMAL_PRIORITY_CLASS");
      }
    }
#endif
    FanfareLog::flushLog();
    checkAndRecoverAudioDevice();
    auto memStats = FanfareLog::getMemoryStats();
    juce::String appRamStr = ResourceInspectorModal::formatBytes(memStats.workingSetBytes);
    ramLabel.setText("RAM: " + appRamStr + " (" + juce::String(memStats.systemRamLoadPercent) + "%)", juce::dontSendNotification);

    const auto thermSnap = thermalMonitor.getSnapshot();
    if (thermSnap.tempCelsius > 0) {
      cpuLabel.setTooltip("CPU Temp: " + juce::String(thermSnap.tempCelsius) +
                          " C (Peak " + juce::String(thermSnap.peakTempCelsius) +
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
  if (auto* dev = deviceManager.getCurrentAudioDevice()) {
    double sr = dev->getCurrentSampleRate();
    int inL = dev->getInputLatencyInSamples();
    int outL = dev->getOutputLatencyInSamples();
    double ms = 0.0;
    if (sr > 0.0) {
      if (inL + outL > 0)
        ms = (inL + outL) / sr * 1000.0;
      else
        ms = dev->getCurrentBufferSizeSamples() * 2.0 / sr * 1000.0;
    }
    latencyLabel.setText(inAudioRecoveryMode ? "LAT: RECONNECT..." : ("LAT: " + juce::String(ms, 1) + "ms"),
                         juce::dontSendNotification);
  } else {
    latencyLabel.setText(inAudioRecoveryMode ? "LAT: RECONNECT..." : "LAT: --",
                         juce::dontSendNotification);
  }

  // Surface any audio underruns: callback-gap count + engine-side stalls,
  // tallied separately so the meter matches what the ears hear while the
  // two pathways stay distinguishable.
  int xruns = audioUnderrunCount.load();
  int stalls = engineStallCount.load();
  if (xruns + stalls > 0) {
    juce::String txt = "XRUNS: " + juce::String(xruns + stalls);
    if (stalls > 0)
      txt += " (" + juce::String(xruns) + " gap / " + juce::String(stalls) + " stall)";
    txt += " [" + juce::String(lastGlitchStallMs.load(), 1) + "ms]";
    xrunsLabel.setText(txt, juce::dontSendNotification);
    xrunsLabel.setColour(juce::Label::textColourId, ThemeManager::get(Theme::Role::warn));
  } else {
    xrunsLabel.setText("XRUNS: 0", juce::dontSendNotification);
    xrunsLabel.setColour(juce::Label::textColourId, ThemeManager::get(Theme::Role::ok));
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

void MainComponent::showMasterPluginMenu(bool isFoh, int chainIndex) {
  juce::PopupMenu menu;
  menu.addSectionHeader("Select Master " + juce::String(isFoh ? "FOH" : "IEM") + " Plugin " + juce::String(chainIndex + 1));
  for (int p = 0; p < engine.getNumAvailablePlugins(); ++p) {
    menu.addItem(p + 1, engine.getAvailablePluginName(p));
  }
  menu.addSeparator();
  menu.addItem(-1, "(No Plugin)");

  menu.showMenuAsync(juce::PopupMenu::Options(), [this, isFoh, chainIndex](int result) {
    if (result > 0 || result == -1) {
      if (auto *oldInstance = engine.getMasterPluginInstance(isFoh, chainIndex)) {
        closePluginWindowForInstance(oldInstance);
      }
    }
    if (result > 0) {
      loadingOverlay.reset(new LoadingOverlay());
      addAndMakeVisible(loadingOverlay.get());

      engine.loadPluginIntoMasterBus(isFoh, chainIndex, result - 1,
          [this, isFoh, chainIndex](bool success, const juce::String &err) {
            loadingOverlay.reset();
            if (!success) {
              juce::AlertWindow::showMessageBoxAsync(
                  juce::MessageBoxIconType::WarningIcon,
                  "Plugin Load Failed", err);
            } else {
              auto name = engine.getMasterPluginName(isFoh, chainIndex);
              auto &btn = isFoh ? fohFxBtns[chainIndex] : iemFxBtns[chainIndex];
              bool hasPlugin = !name.isEmpty();
              btn.getProperties().set("isPluginSlot", true);
              btn.getProperties().set("hasPlugin", hasPlugin);
              btn.setButtonText(hasPlugin ? name : ((isFoh ? "FOH " : "IEM ") + juce::String(chainIndex + 1)));
            }
          });
    } else if (result == -1) {
      engine.loadPluginIntoMasterBus(isFoh, chainIndex, -1, [this, isFoh, chainIndex](bool, const juce::String &) {
        auto &btn = isFoh ? fohFxBtns[chainIndex] : iemFxBtns[chainIndex];
        btn.getProperties().set("isPluginSlot", true);
        btn.getProperties().set("hasPlugin", false);
        btn.setButtonText((isFoh ? "FOH " : "IEM ") + juce::String(chainIndex + 1));
      });
    }
  });
}

void MainComponent::openMasterPluginEditor(bool isFoh, int chainIndex) {
  auto *plugin = engine.getMasterPluginInstance(isFoh, chainIndex);
  if (plugin && plugin->hasEditor()) {
    juce::String name = (isFoh ? "FOH Master " : "IEM Master ") + juce::String(chainIndex + 1) + ": " + plugin->getName();
    for (auto *w : activePluginWindows) {
      if (auto *pw = dynamic_cast<PluginWindow*>(w)) {
        if (pw->pluginInstance == plugin) {
          pw->toFront(true);
          return;
        }
      }
    }

    juce::AudioProcessorEditor* editor = nullptr;
    bool ok = FanfareLog::safeExecutePluginCall([&]() {
      editor = plugin->createEditor();
    }, "createEditor (Master " + plugin->getName() + ")");
    if (ok && editor != nullptr) {
      auto *w = new PluginWindow(name, editor, plugin);
      w->onWindowClosed = [this, w] {
        if (activePluginWindows.contains(w)) {
          activePluginWindows.removeObject(w, false);
          w->destroyEditorNow();
          juce::MessageManager::callAsync([w]() { delete w; });
        }
      };
      activePluginWindows.add(w);
    }
  }
}

void MainComponent::loadRigFile(int index) {
  if (index >= 0 && index < numSetupButtons) {
    currentSetupIndex = index;
  }
  if (setupFilePaths[index].isEmpty()) {
    assignJsonToButton(index);
    return;
  }

  juce::File file(setupFilePaths[index]);
  loadRigAsync(file, index);
}

void MainComponent::loadRigFromFile(const juce::File &file, int targetSetlistIndex) {
  if (!file.existsAsFile())
    return;
  loadRigAsync(file, -1, targetSetlistIndex);
  setupNameLabel.setText(file.getFileNameWithoutExtension(),
                         juce::dontSendNotification);
}

void MainComponent::showScanResultsDialog(const FanfareEngine::ScanResults &results) {
  // Display plugin scanner results
  juce::String msg = "VST3 Scan Complete!\n\n";
  msg += "Total Available VST3s: " + juce::String(engine.getNumAvailablePlugins()) + "\n";
  msg += "Newly Added: " + juce::String(results.newPlugins.size()) + "\n";
  if (!results.missingPlugins.empty()) {
    msg += "Removed (no longer on disk): " + juce::String(results.missingPlugins.size()) + "\n";
  }
  msg += "\nNext step: Click any Strip Slot (1, 2, or 3) to load a VST3 instrument!";
  
  juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::InfoIcon,
                                         "Plugin Scan Results", msg);
}


void MainComponent::saveSetToFile() {
  fileChooser = std::make_unique<juce::FileChooser>(
      "Save Set...",
      Fanfare::RigLibrary::getSetsDirectory(),
      "*.orset");
  fileChooser->launchAsync(
      juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles,
      [this](const juce::FileChooser &fc) {
          auto file = fc.getResult();
          if (file == juce::File{})
              return;
          if (file.getFileExtension() != ".orset")
              file = file.withFileExtension(".orset");
          
          juce::DynamicObject::Ptr obj = new juce::DynamicObject();
          for (int i = 0; i < numSetupButtons; ++i) {
              if (!setupFilePaths[i].isEmpty())
                  obj->setProperty("setup_" + juce::String(i), setupFilePaths[i]);
          }
          
          SaveHistory::recordSave(file);
          file.getParentDirectory().createDirectory();
          bool wroteOk = false;
          {
              juce::FileOutputStream fos(file);
              if (fos.openedOk()) {
                  fos.setPosition(0);
                  fos.truncate();
                  juce::JSON::writeToStream(fos, juce::var(obj.get()));
                  fos.flush();
                  wroteOk = true;
              }
          }
          if (wroteOk)
              SaveHistory::recordSave(file);
      });
}

void MainComponent::loadSetFromFile() {
  fileChooser = std::make_unique<juce::FileChooser>(
      "Load Set...",
      Fanfare::RigLibrary::getSetsDirectory(),
      "*.orset");
  fileChooser->launchAsync(
      juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
      [this](const juce::FileChooser &fc) {
          applySetlistFromFile(fc.getResult());
      });
}

// Apply a .orset setlist to the setup buttons (assigns song paths, colors
// them green/grey, persists the button mappings). Shared between the file-
// chooser path and the library sidebar.
void MainComponent::applySetlistFromFile(const juce::File &file) {
  if (!file.existsAsFile())
    return;
  SaveHistory::snapshotBeforeLoadAsync(file);
  auto json = juce::JSON::parse(file);
  auto *obj = json.getDynamicObject();
  if (!obj)
    return;
  auto& sm = Fanfare::SetlistManager::getInstance();
  for (int i = 0; i < numSetupButtons; ++i) {
    juce::String key = "setup_" + juce::String(i);
    if (obj->hasProperty(key)) {
      setupFilePaths[i] = obj->getProperty(key).toString();
      sm.setSlotSetup(i, juce::File(setupFilePaths[i]));
    } else {
      setupFilePaths[i] = "";
      sm.setSlotSetup(i, juce::File{});
    }
  }
  saveButtonMappings();
  updatePreloadStatus();
}

void MainComponent::loadSetFile(const juce::File &file) {
  applySetlistFromFile(file);
}

void MainComponent::updateSetupButtonLabels() {
  // Empty
}


// Loading overlay helpers (recovered after MainComponent.cpp truncation)
void MainComponent::showLoadingOverlay(const juce::String &title, const juce::String &message) {
  if (!loadingOverlay)
    loadingOverlay = std::make_unique<LoadingOverlay>();
  loadingOverlay->setTitle(title);
  loadingOverlay->setMessage(message);
  loadingOverlay->setBounds(getLocalBounds());
  addAndMakeVisible(loadingOverlay.get());
  loadingOverlay->toFront(true);
}

void MainComponent::setLoadingMessage(const juce::String &message) {
  if (loadingOverlay)
    loadingOverlay->setMessage(message);
}

void MainComponent::showMissingPluginsWarning(const std::vector<Fanfare::RigBuilder::FailedEntry>& failed) {
  std::vector<MissingPluginInfo> missing;
  for (const auto& fe : failed) {
    MissingPluginInfo info;
    info.name = fe.pluginVar.getProperty("name", "").toString();
    info.path = fe.pluginVar.getProperty("path", "").toString();
    if (info.name.isEmpty() && info.path.isNotEmpty()) {
      info.name = juce::File(info.path).getFileNameWithoutExtension();
    }
    if (info.name.isEmpty())
      info.name = "Unknown Plugin";

    if (fe.key.startsWith("S:")) {
      int slotIdx = fe.key.fromFirstOccurrenceOf("S:", false, false)
                          .upToFirstOccurrenceOf(":", false, false)
                          .getIntValue();
      juce::String slotName;
      if (slotIdx >= 0 && slotIdx < (int)engine.getSlots().size()) {
        if (auto* s = engine.getSlot(slotIdx))
          slotName = s->getName();
      }
      if (slotName.isNotEmpty() && !slotName.startsWithIgnoreCase("Slot "))
        info.location = slotName + " (Strip " + juce::String(slotIdx + 1) + ")";
      else
        info.location = "Strip " + juce::String(slotIdx + 1);
    } else if (fe.key.startsWith("A:")) {
      int auxIdx = fe.key.fromFirstOccurrenceOf("A:", false, false)
                         .upToFirstOccurrenceOf(":", false, false)
                         .getIntValue();
      info.location = (auxIdx == 0 ? "Aux 1 Return" : "Aux 2 Return");
    } else if (fe.key.startsWith("F:")) {
      info.location = "Master FOH";
    } else if (fe.key.startsWith("I:")) {
      info.location = "Master IEM";
    } else {
      info.location = fe.key;
    }
    missing.push_back(info);
  }

  if (!missing.empty()) {
    missingPluginsToast = std::make_unique<MissingPluginsToast>(missing, [this]() {
      missingPluginsToast.reset();
    });
    addAndMakeVisible(missingPluginsToast.get());
    updateMissingPluginsToastBounds();
    missingPluginsToast->toFront(true);

    juce::String logMsg = "Setup loaded with " + juce::String((int)missing.size()) + " uninstalled plugin(s): ";
    for (size_t i = 0; i < missing.size(); ++i) {
      if (i > 0) logMsg += ", ";
      logMsg += missing[i].name + " [" + missing[i].location + "]";
    }
    LOG_WARN(logMsg);
  }
}

void MainComponent::updateMissingPluginsToastBounds() {
  if (missingPluginsToast) {
    int w = juce::jmin(520, getWidth() - 40);
    int h = missingPluginsToast->getDesiredHeight();
    int x = (getWidth() - w) / 2;
    int y = 76;
    missingPluginsToast->setBounds(x, y, w, h);
  }
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
    auto appData = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory).getChildFile("Fanfare");
    auto logFile = appData.getChildFile("audio_underrun_log.txt");
    if (!logFile.existsAsFile()) logFile.replaceWithText("No audio underruns detected yet.\n");
    logFile.startAsProcess();
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
      msg << "CPU Temperature: " << snap.tempCelsius << " C  (Session Peak: "
          << snap.peakTempCelsius << " C)\n";
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
        << "- Keep bottom intake vents clear (never sit flat on padded/nylon case lids or cloth).\n"
        << "- Prop the back edge of the laptop up 0.5 - 1 inch for under-chassis airflow.\n"
        << "- Plug AC power directly into the laptop (avoid unpowered/low-wattage USB-C hubs).";

    juce::AlertWindow::showMessageBoxAsync(
        juce::MessageBoxIconType::WarningIcon,
        "CPU Thermal / Firmware Throttle Monitor", msg);
    return;
  }


}

bool MainComponent::keyPressed (const juce::KeyPress& key, juce::Component* originatingComponent) {
  if (dynamic_cast<juce::TextEditor*>(originatingComponent) != nullptr)
    return false;

  if (key == juce::KeyPress::spaceKey) {
    auto& sm = Fanfare::SetlistManager::getInstance();
    if (sm.hasNext()) {
      int idx = sm.getActiveIndex() + 1;
      loadRigFromFile(sm.getSetups()[idx], idx);
    }
    return true;
  }
  else if (key.getModifiers().isCommandDown() && (key.getKeyCode() == 'N' || key.getKeyCode() == 'n')) {
    showGigNotepadWindow();
    return true;
  }
  else if (key.getModifiers().isCommandDown() && (key.getKeyCode() == 'S' || key.getKeyCode() == 's')) {
    if (saveBtn.onClick) saveBtn.onClick();
    return true;
  }
  else if (key.getModifiers().isCommandDown() && (key.getKeyCode() == 'O' || key.getKeyCode() == 'o')) {
    if (loadBtn.onClick) loadBtn.onClick();
    return true;
  }
  else if (key.getKeyCode() == juce::KeyPress::F1Key) {
    Fanfare::openHelpManual("#main-mixer");
    return true;
  }
  else if (!key.getModifiers().isCommandDown() && !key.getModifiers().isAltDown() && (key.isKeyCode ('P') || key.isKeyCode ('p'))) {
    engine.triggerPanic();
    return true;
  }
  return false;
}

void MainComponent::closePluginWindowForInstance(juce::AudioPluginInstance* instance) {
  if (!instance)
    return;
  for (int i = activePluginWindows.size() - 1; i >= 0; --i) {
    if (auto *pw = dynamic_cast<PluginWindow*>(activePluginWindows[i])) {
      if (pw->pluginInstance == instance) {
        pw->destroyEditorNow();
        auto* w = activePluginWindows.removeAndReturn(i);
        juce::MessageManager::callAsync([w]() { delete w; });
      }
    }
  }
}

void MainComponent::closePluginWindowsForSlot(RackSlot* slot) {
  if (!slot)
    return;
  for (int c = 0; c < 3; ++c) {
    if (auto* pi = slot->getPluginInstance(c))
      closePluginWindowForInstance(pi);
  }
}

void MainComponent::closeOrphanedPluginWindows() {
  std::vector<juce::AudioPluginInstance*> activeInstances;
  for (int slotIdx = 0; slotIdx < engine.getNumSlots(); ++slotIdx) {
    if (auto *slot = engine.getSlot(slotIdx)) {
      for (int chainIdx = 0; chainIdx < 3; ++chainIdx) {
        if (auto *pi = slot->getPluginInstance(chainIdx))
          activeInstances.push_back(pi);
      }
    }
  }
  for (int r = 0; r < engine.getNumAuxReturns(); ++r) {
    if (auto *aux = engine.getAuxReturn(r)) {
      for (int chainIdx = 0; chainIdx < 3; ++chainIdx) {
        if (auto *pi = aux->getPluginInstance(chainIdx))
          activeInstances.push_back(pi);
      }
    }
  }
  for (int i = 0; i < 3; ++i) {
    if (auto *pi = engine.getMasterPluginInstance(true, i))
      activeInstances.push_back(pi);
    if (auto *pi = engine.getMasterPluginInstance(false, i))
      activeInstances.push_back(pi);
  }

  for (int i = activePluginWindows.size() - 1; i >= 0; --i) {
    if (auto *pw = dynamic_cast<PluginWindow*>(activePluginWindows[i])) {
      bool found = (std::find(activeInstances.begin(), activeInstances.end(), pw->pluginInstance) != activeInstances.end());
      if (!found) {
        pw->destroyEditorNow();
        auto* w = activePluginWindows.removeAndReturn(i);
        juce::MessageManager::callAsync([w]() { delete w; });
      }
    }
  }
}

void MainComponent::closeAllPluginWindows() {
  auto* tempArray = new juce::OwnedArray<juce::DocumentWindow>();
  for (int i = activePluginWindows.size() - 1; i >= 0; --i) {
    if (auto *pw = dynamic_cast<PluginWindow*>(activePluginWindows[i]))
      pw->destroyEditorNow();
    tempArray->add(activePluginWindows.removeAndReturn(i));
  }
  juce::MessageManager::callAsync([tempArray]() {
    delete tempArray;
  });
}

void MainComponent::showWebRemoteDialog() {
  if (stageRemoteOverlay != nullptr) {
    stageRemoteOverlay.reset();
    return;
  }
  if (hotspotController == nullptr) {
    hotspotController = std::make_unique<OpenRig::StageHotspotController>();
  }
  stageRemoteOverlay = std::make_unique<OpenRig::StageRemoteModalOverlay>(*hotspotController, 8080);
  stageRemoteOverlay->onClose = [this]() {
    stageRemoteOverlay.reset();
    repaint();
  };
  addAndMakeVisible(*stageRemoteOverlay);
  stageRemoteOverlay->setBounds(getLocalBounds());
  stageRemoteOverlay->toFront(true);
}

//==============================================================================
// Studio shell (Nov 2026): menu bar, tool rail + context sidebar, scene menu.
// Header controls that moved into menus / the rail stay alive (their onClick
// handlers are reused below), they are just no longer laid out.
//==============================================================================
namespace {
enum MenuIds {
  miNewSong = 1, miLoadSong, miSaveSong, miBuildSong, miLoadMap, miSaveMap, miQuit,
  miViewContextBase = 100, // + ToolRail context index
  miCompact = 120, miCleanSlate,
  miWidthBase = 130,       // + preset index
  miThemeBase = 150,       // + theme index
  miAudio = 200, miBus, miEngine, miControllerProfile, miScan, miResetAudio, miSettings,
  miNotepad = 300, miInspector, miRemote,
  miManual = 400, miWelcome, miQuickStart, miUpdate, miAbout
};
constexpr int kStripWidthPresets[] = {96, 124, 160, 220};
const char *const kStripWidthNames[] = {"Narrow", "Normal", "Wide", "Extra wide"};

juce::PopupMenu::Item menuItem(int id, const juce::String &text,
                               const juce::String &shortcut = {},
                               bool ticked = false, bool enabled = true) {
  juce::PopupMenu::Item it(text);
  it.setID(id);
  it.setTicked(ticked);
  it.setEnabled(enabled);
  it.shortcutKeyDescription = shortcut;
  return it;
}
} // namespace

void MainComponent::setupStudioShell() {
  for (juce::Component *c : std::initializer_list<juce::Component *>{
           &saveBtn, &loadBtn, &exitBtn, &gigNotepadBtn, &webRemoteBtn,
           &quickStartBtn, &mainHelpBtn, &midiMonitorToggle, &settingsGearBtn,
           &toggleLibraryBtn, &setupBuilderBtn, &saveSceneBtn, &renameSceneBtn,
           &deleteSceneBtn, &freshBuildToggle, &autoCollapseToggle,
           &stripWidthLabel, &stripWidthSlider})
    c->setVisible(false);

  menuBar = std::make_unique<juce::MenuBarComponent>(this);
  addAndMakeVisible(menuBar.get());

  addAndMakeVisible(toolRail);
  toolRail.onTool = [this](int t) { onToolRail(t); };

  // The queue row moves into the Queue sidebar context.
  juce::Array<Fanfare::QueueButtonComponent *> tiles;
  for (auto *q : queueButtons)
    tiles.add(q);
  saveSetBtn.setButtonText("Save map");
  loadSetBtn.setButtonText("Load map");
  saveSetBtn.setTooltip("Save the queue tile assignments as a queue map (.orset)");
  loadSetBtn.setTooltip("Load a saved queue map");
  Fanfare::setButtonRole(saveSetBtn, Fanfare::ButtonRole::Subtle);
  Fanfare::setButtonRole(loadSetBtn, Fanfare::ButtonRole::Subtle);
  queueTilesPanel.setContent(tiles, {&loadSetBtn, &saveSetBtn});
  libraryPanel->setQueueContent(&queueTilesPanel);

  addSceneBtn.setButtonText("+ Scene");
  addSceneBtn.setTooltip("Add a scene (snapshot of strip mutes, levels and sends)");
  Fanfare::setButtonRole(addSceneBtn, Fanfare::ButtonRole::Subtle);
  addAndMakeVisible(sceneMenuBtn);
  Fanfare::setButtonRole(sceneMenuBtn, Fanfare::ButtonRole::Subtle);
  sceneMenuBtn.setTooltip("Scene actions: save, rename, delete (right-click a scene for more)");
  sceneMenuBtn.onClick = [this] { showSceneMenu(); };

  panicBtn.setButtonText("Panic");
  panicBtn.setTooltip("Panic (P): silences all stuck notes and flushes audio buffers.");
  prevSetlistBtn.setButtonText("<");
  nextSetlistBtn.setButtonText(">");
  prevSetlistBtn.setTooltip("Previous song in the setlist");
  nextSetlistBtn.setTooltip("Next song in the setlist (Space)");
  Fanfare::setButtonRole(prevSetlistBtn, Fanfare::ButtonRole::Subtle);
  Fanfare::setButtonRole(nextSetlistBtn, Fanfare::ButtonRole::Subtle);
  setupNameLabel.setFont(BoutiqueLookAndFeel::uiFont(17.0f, true));
  setupNameLabel.setJustificationType(juce::Justification::centred);
  midiMonitorLabel.setFont(BoutiqueLookAndFeel::monoFont(11.0f));
  for (auto *l : {&cpuLabel, &ramLabel, &latencyLabel, &xrunsLabel, &clockLabel})
    l->setFont(BoutiqueLookAndFeel::monoFont(11.0f));
  preloadStatusLabel.setFont(BoutiqueLookAndFeel::uiFont(12.0f));

  setSidebarContext(Fanfare::ToolRail::Songs);
}

void MainComponent::setSidebarContext(int ctx) {
  sidebarContext = ctx;
  libraryVisible = ctx >= 0;
  if (ctx == Fanfare::ToolRail::Midi && !midiMonitorPanel) {
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
    case R::Notepad:   showGigNotepadWindow(); break;
    case R::Inspector: showResourceInspectorModal(); break;
    case R::Remote:    showWebRemoteDialog(); break;
    case R::Help:      Fanfare::openHelpManual("#main-mixer"); break;
    case R::Settings:  showConfigOverlay(); break;
    default: break;
  }
}

void MainComponent::showSceneMenu() {
  const int idx = engine.getCurrentSceneIndex();
  const bool valid = idx >= 0 && idx < engine.getNumScenes();
  const juce::String name = valid ? engine.getSceneName(idx) : juce::String();
  juce::PopupMenu m;
  m.addSectionHeader(valid ? "Scene: " + name : "Scenes");
  m.addItem(1, "Save current state to this scene", valid);
  m.addItem(2, "Rename scene...", valid);
  m.addSeparator();
  m.addItem(3, "Delete scene...", valid && engine.getNumScenes() > 1);
  m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&sceneMenuBtn),
                  [this, name](int r) {
    if (r == 1 && saveSceneBtn.onClick) {
      saveSceneBtn.onClick();
    } else if (r == 2 && renameSceneBtn.onClick) {
      renameSceneBtn.onClick();
    } else if (r == 3) {
      juce::Component::SafePointer<MainComponent> safeThis(this);
      juce::AlertWindow::showAsync(
          juce::MessageBoxOptions()
              .withIconType(juce::MessageBoxIconType::WarningIcon)
              .withTitle("Delete scene")
              .withMessage("Delete \"" + name + "\"? This can't be undone.")
              .withButton("Delete")
              .withButton("Cancel"),
          [safeThis](int result) {
            if (result == 1 && safeThis != nullptr && safeThis->deleteSceneBtn.onClick)
              safeThis->deleteSceneBtn.onClick();
          });
    }
  });
}

void MainComponent::confirmAndQuit() {
  juce::AlertWindow::showAsync(
      juce::MessageBoxOptions()
          .withIconType(juce::MessageBoxIconType::QuestionIcon)
          .withTitle("Quit Fanfare")
          .withMessage("Quit Fanfare? All plugins will be unloaded.")
          .withButton("Quit")
          .withButton("Cancel"),
      [](int result) {
        if (result == 1)
          juce::JUCEApplication::quit();
      });
}

juce::StringArray MainComponent::getMenuBarNames() {
  return {"File", "View", "Rig", "Tools", "Help"};
}

juce::PopupMenu MainComponent::getMenuForIndex(int topLevelMenuIndex, const juce::String &) {
  juce::PopupMenu m;
  switch (topLevelMenuIndex) {
    case 0: // File
      m.addItem(menuItem(miNewSong, "New song"));
      m.addItem(menuItem(miLoadSong, "Load song...", "Ctrl+O"));
      m.addItem(menuItem(miSaveSong, "Save song as...", "Ctrl+S"));
      m.addItem(menuItem(miBuildSong, "Build song from strips..."));
      m.addSeparator();
      m.addItem(menuItem(miLoadMap, "Load queue map..."));
      m.addItem(menuItem(miSaveMap, "Save queue map..."));
      m.addSeparator();
      m.addItem(menuItem(miQuit, "Quit Fanfare..."));
      break;
    case 1: { // View
      for (int i = 0; i < Fanfare::ToolRail::kNumContexts; ++i)
        m.addItem(menuItem(miViewContextBase + i,
                           LibraryPanel::contextTitle((LibraryPanel::Context)i), {},
                           sidebarContext == i));
      m.addSeparator();
      m.addItem(menuItem(miCompact, "Stage compact (collapse muted strips)", {},
                         autoCollapseToggle.getToggleState()));
      m.addItem(menuItem(miCleanSlate, "Clean slate for this song", {},
                         freshBuildToggle.getToggleState()));
      juce::PopupMenu widths;
      for (int i = 0; i < 4; ++i)
        widths.addItem(menuItem(miWidthBase + i, kStripWidthNames[i], {},
                                std::abs(preferredExpandedWidth - kStripWidthPresets[i]) < 8));
      m.addSubMenu("Strip width", widths);
      juce::PopupMenu themes;
      auto names = ThemeManager::getInstance().getThemeNames();
      for (int i = 0; i < names.size(); ++i)
        themes.addItem(menuItem(miThemeBase + i, names[i], {},
                                ThemeManager::getInstance().getCurrentIndex() == i));
      m.addSubMenu("Theme", themes);
      break;
    }
    case 2: // Rig
      m.addItem(menuItem(miAudio, "Audio & MIDI settings..."));
      m.addItem(menuItem(miBus, "Bus routing..."));
      m.addItem(menuItem(miControllerProfile, "Controller profile..."));
      m.addItem(menuItem(miEngine, "Engine & CPU tuning..."));
      m.addSeparator();
      m.addItem(menuItem(miScan, "Scan for plugins"));
      m.addItem(menuItem(miResetAudio, "Reset audio driver"));
      m.addSeparator();
      m.addItem(menuItem(miSettings, "All settings..."));
      break;
    case 3: // Tools
      m.addItem(menuItem(miNotepad, "Gig notepad", "Ctrl+N"));
      m.addItem(menuItem(miInspector, "Resource inspector"));
      m.addItem(menuItem(miRemote, "Tablet remote..."));
      m.addItem(menuItem(miViewContextBase + Fanfare::ToolRail::Midi, "MIDI monitor"));
      m.addSeparator();
      m.addItem(menuItem(miSettings, "Settings..."));
      break;
    case 4: // Help
      m.addItem(menuItem(miManual, "User manual", "F1"));
      m.addItem(menuItem(miWelcome, "Welcome screen..."));
      m.addItem(menuItem(miQuickStart, "Quick start..."));
      if (latestReleaseUrl.isNotEmpty())
        m.addItem(menuItem(miUpdate, "Get the new release..."));
      m.addSeparator();
      m.addItem(menuItem(miAbout, "About Fanfare"));
      break;
    default:
      break;
  }
  return m;
}

void MainComponent::menuItemSelected(int id, int) {
  auto click = [](juce::Button &b) {
    if (b.onClick)
      b.onClick();
  };
  if (id >= miViewContextBase && id < miViewContextBase + Fanfare::ToolRail::kNumContexts) {
    setSidebarContext(id - miViewContextBase);
    return;
  }
  if (id >= miWidthBase && id < miWidthBase + 4) {
    stripWidthSlider.setValue(kStripWidthPresets[id - miWidthBase], juce::sendNotificationSync);
    return;
  }
  if (id >= miThemeBase && id < miThemeBase + 32) {
    ThemeManager::getInstance().setCurrentByIndex(id - miThemeBase);
    return;
  }
  switch (id) {
    case miNewSong:    resetToNewSong(); break;
    case miLoadSong:   click(loadBtn); break;
    case miSaveSong:   click(saveBtn); break;
    case miBuildSong:  click(setupBuilderBtn); break;
    case miLoadMap:    click(loadSetBtn); break;
    case miSaveMap:    click(saveSetBtn); break;
    case miQuit:       confirmAndQuit(); break;
    case miCompact:
      autoCollapseToggle.setToggleState(!autoCollapseToggle.getToggleState(), juce::dontSendNotification);
      click(autoCollapseToggle);
      break;
    case miCleanSlate:
      freshBuildToggle.setToggleState(!freshBuildToggle.getToggleState(), juce::dontSendNotification);
      click(freshBuildToggle);
      break;
    case miAudio:      click(audioSettingsBtn); break;
    case miBus:        click(busRoutingBtn); break;
    case miControllerProfile: Fanfare::showControllerProfileEditor(); break;
    case miEngine:     showEngineTuningModal(); break;
    case miScan:       click(scanButton); break;
    case miResetAudio: click(resetAudioBtn); break;
    case miSettings:   showConfigOverlay(); break;
    case miNotepad:    showGigNotepadWindow(); break;
    case miInspector:  showResourceInspectorModal(); break;
    case miRemote:     showWebRemoteDialog(); break;
    case miManual:     Fanfare::openHelpManual("#main-mixer"); break;
    case miWelcome:    showWelcomeModal(); break;
    case miQuickStart: showQuickStartDialog(false); break;
    case miUpdate:     click(updateBadgeBtn); break;
    case miAbout:      showAboutDialog(); break;
    default: break;
  }
}

void MainComponent::showWelcomeModal() {
  Fanfare::StartupWelcomeModal::Actions a;
  a.onNewSong = [this] { resetToNewSong(); };
  a.onOpenSong = [this] {
    if (loadBtn.onClick)
      loadBtn.onClick();
  };
  a.onBuildSong = [this] {
    if (setupBuilderBtn.onClick)
      setupBuilderBtn.onClick();
  };
  a.onScanPlugins = [this] {
    if (scanButton.onClick)
      scanButton.onClick();
  };
  a.onLoadRecentSong = [this](const juce::File &file) {
    loadRigFromFile(file);
  };
  a.onOpenSettings = [this] { showConfigOverlay(); };
  a.onClose = [this] {
    welcomeOverlay.reset();
    resized();
    repaint();
  };

  welcomeOverlay = std::make_unique<Fanfare::StartupWelcomeModal>(
      std::move(a), engine.getNumAvailablePlugins());
  addAndMakeVisible(welcomeOverlay.get());
  welcomeOverlay->setBounds(getLocalBounds());
  welcomeOverlay->toFront(true);
}

void MainComponent::resetToNewSong() {
  bool anyLoaded = false;
  for (int i = 0; i < engine.getNumSlots() && !anyLoaded; ++i)
    if (auto *s = engine.getSlot(i))
      for (int c = 0; c < 3; ++c)
        if (s->getPluginInstance(c) != nullptr || s->isChainSlotMidiOut(c))
          anyLoaded = true;
  for (int i = 0; i < 3 && !anyLoaded; ++i)
    if (!engine.getMasterPluginName(true, i).isEmpty() ||
        !engine.getMasterPluginName(false, i).isEmpty())
      anyLoaded = true;

  auto performReset = [this] {
    closeAllPluginWindows();
    engine.clearAllRacks();
    engine.resetScenesToDefault();
    currentRigFile = juce::File{};
    currentSetupIndex = -1;
    setupNameLabel.setText("Untitled song", juce::dontSendNotification);
    for (int i = 0; i < engine.getNumSlots(); ++i) {
      if (auto *s = engine.getSlot(i)) {
        if (i == 0)
          s->setName("Monitor In");
        else if (i == 1)
          s->setName("Keyboard");
        else
          s->setName("Slot " + juce::String(i + 1));
      }
    }
    for (auto *c : rackSlotComponents) {
      c->updateOutputSelector();
      c->repaint();
    }
    refreshSceneButtons();
    updatePreloadStatus();
    resized();
    repaint();
  };

  if (!anyLoaded) {
    performReset();
    return;
  }

  juce::Component::SafePointer<MainComponent> safeThis(this);
  juce::AlertWindow::showAsync(
      juce::MessageBoxOptions()
          .withIconType(juce::MessageBoxIconType::QuestionIcon)
          .withTitle("New song")
          .withMessage("Start a new empty song? Currently loaded plugins will be unloaded.")
          .withButton("New song")
          .withButton("Cancel"),
      [safeThis, performReset](int result) {
        if (result == 1 && safeThis != nullptr)
          performReset();
      });
}
