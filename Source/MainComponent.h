#pragma once

#include "AssetLoader.h"
#include "BoutiqueLookAndFeel.h"
#include "ChannelStripComponent.h"
#include "ConfigModalOverlay.h"
#include "EngineTuningModal.h"
#include "MidiEffectsComponent.h"
#include "SamplerComponent.h"
#include "Mp3PlayerComponent.h"
#include "ResourceInspectorModal.h"
#include "GigNotepadWindow.h"
#include "SetupMidiTriggers.h"
#include "LoadingOverlay.h"
#include "MissingPluginsToast.h"
#include "SetupBuilderOverlay.h"
#include "Logger.h"
#include "LibraryPanel.h"
#include "MidiLearnBus.h"
#include "MidiMonitorComponent.h"
#include "NoteRangeComponent.h"
#include "IMidiNoteLearner.h"
#include "FanfareConstants.h"
#include "FanfareEngine.h"
#include "RackSlotComponent.h"
#include "RigLibrary.h"
#include "QueueButtonComponent.h"
#include "SceneButtonComponent.h"
#include "RigTransitioner.h"
#include "AnalogVuMeter.h"
#include "ThermalMonitor.h"
#include "UpdateChecker.h"
#include "ToolRail.h"
#include "QueueTilesPanel.h"
#include "StartupWelcomeModal.h"
#include "Web/OpenRigWebServer.h"
#include "Web/StageHotspotController.h"
#include "Web/StageRemoteModalOverlay.h"
#include <JuceHeader.h>
#include <functional>

//==============================================================================
/*
    This component lives inside our window, and this is where you should put all
    your controls and content.
*/
class MainComponent : public juce::Component,
                      public juce::MidiInputCallback,
                      public juce::AudioIODeviceCallback,
                      public juce::ChangeListener,
                      public juce::Timer,
                      public juce::KeyListener,
                      public juce::MenuBarModel {
public:
  //==============================================================================
  MainComponent();
  ~MainComponent() override;

  void timerCallback() override;
  bool keyPressed (const juce::KeyPress& key, juce::Component* originatingComponent) override;

  // --- MenuBarModel (File / View / Rig / Tools / Help) ---
  juce::StringArray getMenuBarNames() override;
  juce::PopupMenu getMenuForIndex(int topLevelMenuIndex, const juce::String& menuName) override;
  void menuItemSelected(int menuItemID, int topLevelMenuIndex) override;

  // --- AudioIODeviceCallback ---
  void audioDeviceIOCallbackWithContext(
      const float *const *inputChannelData, int numInputChannels,
      float *const *outputChannelData, int numOutputChannels, int numSamples,
      const juce::AudioIODeviceCallbackContext &context) override;
  void audioDeviceAboutToStart(juce::AudioIODevice *device) override;
  void audioDeviceStopped() override;

  //==============================================================================
  void paint(juce::Graphics &) override;
  void resized() override;
  void mouseDown(const juce::MouseEvent &e) override;

  void updateSetupButtonLabels();

  void saveAudioSettings();
  void loadAudioSettings();
  void resetAudioDevice();
  void setAuxLogoImage(const juce::Image &loaded, bool animate);
  void chooseAuxLogoImage();
  void resetAuxLogoImage();
  void loadAuxLogoPreference();
  void saveAuxLogoPreference() const;
  void showAboutDialog();
  void showQuickStartDialog(bool isFirstRunPrompt = false);
  void showWelcomeModal();
  void onStartupSplashFinished();
  void resetToNewSong();
  void showConfigOverlay();
  void setActiveMidiNoteLearner(juce::Component* learner) {
    activeMidiNoteLearner = learner;
    hasMidiNoteLearner.store(learner != nullptr, std::memory_order_release);
  }
  double getCpuUsage() { return deviceManager.getCpuUsage() * 100.0; }
  int getAudioUnderruns() const { return audioUnderrunCount.load() + engineStallCount.load(); }
  bool isThermalWarningActive() const { return thermalMonitor.isWarningActive(); }
  int getCpuTemperatureCelsius() const { return thermalMonitor.getTemperatureCelsius(); }
  juce::String getThermalBadgeText() const { return thermalMonitor.getSnapshot().badgeText; }
  void loadRigFromFile(const juce::File &file, int targetSetlistIndex = -1);
  void updateStripCollapseStates();
  void refreshThemeDependentStyles();

private:
  //==============================================================================
  juce::AudioDeviceManager deviceManager;
  FanfareEngine engine;
  BoutiqueLookAndFeel boutiqueLookAndFeel;
  juce::TooltipWindow tooltipWindow{this};

  juce::ImageButton aboutBtn;
  juce::ImageButton settingsGearBtn;
  juce::TextButton scanButton{"SCAN FOR PLUGINS"};
  juce::TextButton audioSettingsBtn{"AUDIO SETTINGS"};
  juce::TextButton busRoutingBtn{"BUS ROUTING"};
  juce::TextButton saveBtn{"SAVE SONG"};
  juce::TextButton loadBtn{"LOAD SONG"};
  juce::TextButton gigNotepadBtn{"NOTEPAD"};
  juce::TextButton webRemoteBtn{"REMOTE"};
  juce::TextButton quickStartBtn{"?"};
  juce::TextButton mainHelpBtn{"HAAALP!"};
  juce::TextButton updateBadgeBtn{"UPDATE"};
  juce::String latestReleaseUrl;
  juce::TextButton prevSetlistBtn{"<<"};
  juce::TextButton nextSetlistBtn{">>"};
  juce::TextButton panicBtn{"PANIC"};
  juce::TextButton resetAudioBtn{"RESET AUDIO"};
  juce::TextButton exitBtn{"EXIT"};
  struct RackViewport : public juce::Viewport {
    std::function<void(const juce::Rectangle<int> &)> onVisibleAreaChanged;
    void visibleAreaChanged(const juce::Rectangle<int> &r) override {
      juce::Viewport::visibleAreaChanged(r);
      if (onVisibleAreaChanged)
        onVisibleAreaChanged(r);
    }
  };
  struct RackScrollListener : public juce::ScrollBar::Listener {
    MainComponent *owner = nullptr;
    void scrollBarMoved(juce::ScrollBar *, double newRangeStart) override {
      if (owner != nullptr)
        owner->setRackScrollX((int)std::round(newRangeStart));
    }
  } rackScrollListener;
  void setRackScrollX(int newX);
  void scrollRackBy(int deltaPx);
  void ensureStripVisibleInRack(RackSlotComponent *target);

  juce::Component rackStripContainer;
  RackViewport rackViewport;
  juce::ScrollBar rackHScrollBar{false};
  juce::TextButton rackScrollLeftBtn{"<"};
  juce::TextButton rackScrollRightBtn{">"};
  juce::OwnedArray<RackSlotComponent> rackSlotComponents;
  juce::OwnedArray<RackSlotComponent> auxReturnComponents;

  // Overlays & Windows
  std::unique_ptr<ChannelStripComponent> channelStripOverlay;
  std::unique_ptr<MidiEffectsComponent> midiEffectsOverlay;
  std::unique_ptr<SamplerComponent> samplerOverlay;
  std::unique_ptr<Mp3PlayerComponent> mp3Overlay;
  std::unique_ptr<LoadingOverlay> loadingOverlay;
  std::unique_ptr<MissingPluginsToast> missingPluginsToast;
  std::unique_ptr<ConfigModalOverlay> configOverlay;
  std::unique_ptr<EngineTuningModal> engineTuningModal;
  std::atomic<juce::int64> suppressGlitchUntilTicks{0};
  EngineTuningModal::Settings engineTuningSettings;

  void suppressGlitchDetectionForMs(int durationMs) noexcept;
  void showEngineTuningModal();
  void applyEngineTuningSettings();
  void loadEngineTuningSettings();
  void saveEngineTuningSettings();

  // USB Audio & MIDI Hotplug / Auto-Recovery state
  juce::String desiredAudioDeviceType;
  juce::AudioDeviceManager::AudioDeviceSetup desiredAudioSetup;
  bool hasDesiredAudioSetup = false;
  bool inAudioRecoveryMode = false;
  bool isSyncingMidiInputs = false;
  bool isRecoveringAudioDevice = false;
  juce::int64 lastAudioRecoveryAttemptMs = 0;
  int audioRecoveryAttemptCount = 0;
  juce::StringArray activeMidiInputIds;
  juce::StringArray lastEnumeratedMidiIds;
  std::map<juce::String, int> midiOpenFailCounts;
  std::map<juce::String, juce::int64> midiOpenRetryAfterMs;
  juce::int64 lastTopologyUnquarantineMs = 0;
  juce::int64 suppressHotplugChangeBroadcastUntilMs = 0;
  int lastWinMmMidiInCount = -1;
  std::atomic<bool> usbTopologyDirty{true};
#ifdef _WIN32
  void *usbNotifyHwnd = nullptr;
  void *usbNotifyHandle = nullptr;
  void startUsbDeviceWatcher();
  void stopUsbDeviceWatcher();
#endif
  juce::Component::SafePointer<juce::Component> activeAudioSelector;

  void stripLegacyDirectSoundDeviceType();
  void captureDesiredAudioSetup();
  void syncHotplugMidiInputs(bool isInitialSetup = false);
  void checkAndRecoverAudioDevice();
  std::unique_ptr<LibraryPanel> libraryPanel;
  std::unique_ptr<SetupBuilderOverlay> setupBuilderOverlay;
  std::unique_ptr<ResourceInspectorModal> resourceInspectorOverlay;
  std::unique_ptr<GigNotepadWindow> gigNotepadWindow;
  std::unique_ptr<OpenRig::WebServer> webServer;
  std::unique_ptr<OpenRig::StageHotspotController> hotspotController;
  std::unique_ptr<OpenRig::StageRemoteModalOverlay> stageRemoteOverlay;

  void showResourceInspectorModal();
  void showGigNotepadWindow();
  void showWebRemoteDialog();
  
  int currentSetupIndex = 0;

  // Async rig transitions (Pillar B); CC-learn uses the MidiLearnBus singleton
  std::unique_ptr<Fanfare::RigTransitioner> transitioner;

  // Stage Queue buttons - each links to a JSON rig file
  static constexpr int numSetupButtons = FanfareConstants::kNumSetupButtons;
  juce::OwnedArray<Fanfare::QueueButtonComponent> queueButtons;
  juce::String setupFilePaths[numSetupButtons];
  juce::TextButton saveSetBtn{"SAVE SET"};
  juce::TextButton loadSetBtn{"LOAD SET"};
  juce::TextButton setupBuilderBtn{"BUILD SONG"};

  // Scene (Preset) management buttons
  juce::TextButton addSceneBtn{"+ SCENE"};
  juce::TextButton saveSceneBtn{"SAVE SCENE"};
  juce::TextButton renameSceneBtn{"RENAME"};
  juce::TextButton deleteSceneBtn{"DELETE"};
  juce::OwnedArray<Fanfare::SceneButtonComponent> sceneButtons;
  juce::StringArray sceneSetupFilePaths;
  void refreshSceneButtons();
  void showSetupBuilderOverlay();

  // Collapsible library sidebar
  juce::TextButton toggleLibraryBtn{"<<"};

  void assignJsonToButton(int buttonIndex);
  void loadRigFile(int index);
  void loadSetFile(const juce::File &file);
  void saveButtonMappings();
  void loadButtonMappings();
  void saveSetToFile();
  void loadSetFromFile();
  void applySetlistFromFile(const juce::File &file);
  void showScanResultsDialog(const FanfareEngine::ScanResults &results);
  void showMasterPluginMenu(bool isFoh, int chainIndex);
  void openMasterPluginEditor(bool isFoh, int chainIndex);
  void updatePreloadStatus();
  void showSlotMidiOutDialog(int slotIdx, int chainIdx);

  // --- Setup Helpers (extracted from constructor) ---
  void setupSlotComponents();
  void setupHeaderButtons();
  void setupSetupButtons();
  void setupMasterSection();

  // --- Loading overlay helpers ---
  void showLoadingOverlay(const juce::String &title, const juce::String &message);
  void setLoadingMessage(const juce::String &message);
  void hideLoadingOverlay();
  void showMissingPluginsWarning(const std::vector<Fanfare::RigBuilder::FailedEntry>& failed);
  void updateMissingPluginsToastBounds();

  // Begin an async (non-freezing) rig load from a file.
  void loadRigAsync(const juce::File &file, int buttonIndexForHighlight = -1, int targetSetlistIndex = -1);

  // Plugin window management
  void closePluginWindowForInstance(juce::AudioPluginInstance* instance);
  void closePluginWindowsForSlot(RackSlot* slot);
  void closeOrphanedPluginWindows();
  void closeAllPluginWindows();

  // Windows managed by MainComponent to prevent shutdown crashes
  juce::OwnedArray<juce::DocumentWindow> activePluginWindows;

  // FileChooser must outlive async callbacks
  std::unique_ptr<juce::FileChooser> fileChooser;

  // Master bus sliders
  juce::Slider masterFohSlider;
  juce::Slider masterIemSlider;
  juce::Label fohLabel{"fohLabel", "FOH"};
  juce::Label iemLabel{"iemLabel", "IEM"};
  juce::TextButton fohFxBtns[3], fohEditGuiBtns[3];
  juce::TextButton iemFxBtns[3], iemEditGuiBtns[3];
  AnalogVuMeter masterVuMeterL{"FOH L"};
  AnalogVuMeter masterVuMeterR{"FOH R"};
  float masterFohL = 0.0f, masterFohR = 0.0f;
  float masterIemL = 0.0f, masterIemR = 0.0f;

  // CPU/RAM/latency/underrun/thermal monitors
  juce::Label cpuLabel{"cpuLabel", "CPU: 0%"};
  juce::Label ramLabel{"ramLabel", "RAM: 0%"};
  juce::Label latencyLabel{"latencyLabel", "LAT: --"};
  juce::Label xrunsLabel{"xrunsLabel", "XRUNS: 0"};
  juce::Label thermalLabel{"thermalLabel", "THERMAL"};
  ThermalMonitor thermalMonitor;
  UpdateChecker updateChecker;
  std::atomic<int> audioUnderrunCount{0};
  std::atomic<int> engineStallCount{0}; // worker-stall silence blocks (engine-side pathway)
  std::atomic<juce::int64> lastAudioCallbackTicks{0};
  std::atomic<double> lastGlitchStallMs{0.0};
  std::atomic<double> pendingGlitchActualMs{0.0};
  std::atomic<double> pendingGlitchExpectedMs{0.0};
  // Duration of the previous audio callback (entry -> processAudio returned).
  // On a glitch this splits "the driver called us late" (small duration) from
  // "our own callback blocked" (duration ~ the gap).
  std::atomic<double> lastCallbackDurationMs{0.0};
  std::atomic<double> pendingGlitchDurationMs{0.0};
  std::atomic<bool> hasPendingGlitchLog{false};
  void logAudioGlitch(double actualMs, double expectedMs, double cbDurationMs);
  void logAudioNanSpike(int channel, int sampleIndex, float badVal);
  // Glitch log batching: entries accumulate in memory and flush to disk at
  // most once per second — storm bursts no longer open the file per entry.
  juce::String pendingGlitchLogText;
  juce::CriticalSection glitchLogLock;
  void flushGlitchLog();

  juce::Label setupNameLabel{"setupNameLabel", "No song loaded"};
  juce::File currentRigFile;
  juce::ToggleButton freshBuildToggle{"CLEAN SLATE"};
  juce::ToggleButton autoCollapseToggle{"STAGE COMPACT"};
  juce::Label stripWidthLabel{"wLabel", "STRIP WIDTH"};
  juce::Slider stripWidthSlider;
  int preferredExpandedWidth = 120;
  int dragBaseExpandedWidth = 120;
  void savePreferredStripWidth() const;
  static constexpr int kMinExpandedWidth = 85;
  static constexpr int kMaxExpandedWidth = 260;
  juce::ImageComponent logoComponent;
  juce::Image logoImage;
  juce::ImageComponent lastWaltzLogoComponent;
  juce::Image lastWaltzLogoImage;
  juce::String auxLogoUserPath; // empty = embedded Last Waltz art
  juce::Label clockLabel{"clockLabel", ""};
  juce::Label preloadStatusLabel{"preloadStatusLabel", ""};
  juce::Rectangle<int> headerBounds;
  juce::Rectangle<int> stageHeaderBounds;
  juce::Rectangle<int> controlBarBounds;
  juce::Rectangle<int> queueRowBounds;
  juce::Rectangle<int> sceneRowBounds;
  juce::Rectangle<int> telemetryWellBounds;
  juce::Rectangle<int> midiWellBounds;
  juce::Rectangle<int> statusBarBounds;

  // ===== Studio shell (Nov 2026): menu bar + tool rail + context sidebar =====
  std::unique_ptr<juce::MenuBarComponent> menuBar;
  std::unique_ptr<Fanfare::StartupWelcomeModal> welcomeOverlay;
  Fanfare::ToolRail toolRail;
  Fanfare::QueueTilesPanel queueTilesPanel;
  juce::TextButton sceneMenuBtn{"..."};
  int sidebarContext = 0; // ToolRail context index, -1 = sidebar closed
  void setupStudioShell();
  void setSidebarContext(int ctx);
  void onToolRail(int tool);
  void showSceneMenu();
  void confirmAndQuit();
  juce::Rectangle<int> masterColumnBounds;
  juce::ComboBox schemeSelector;

  juce::MidiMessageCollector midiCollector;

  // Windows MMCSS audio thread priority: attempted once per audio thread
  // (device restarts create new threads — MMCSS may succeed on a retry even
  // if it failed for an earlier thread). Tracks the thread id, not a bool.
  DWORD mmcssThreadId = 0;
  // CPU-0 shelter mask (constructor excludes the interrupt-dump core). Some
  // ASIO drivers re-expand the process affinity during init, so every new
  // audio thread re-applies it — see the audio callback.
  std::atomic<DWORD_PTR> processShelterMask{0};
  std::atomic<DWORD_PTR> dspThreadAffinityMask{0};
#ifdef _WIN32
  HMODULE avrtModule = nullptr;
  typedef HANDLE(WINAPI * PAvSetMmThreadCharacteristicsA)(LPCSTR, LPDWORD);
  PAvSetMmThreadCharacteristicsA avSetMmThreadFn = nullptr;
  GUID previousPowerSchemeGuid{};
  bool previousPowerSchemeSaved = false;
  bool powerPlanForcedHigh = false;
  void configureWindowsPerformanceMode();
  void restoreWindowsPowerMode();
#endif
  juce::Label midiMonitorLabel{"midiMonitor", "MIDI: --"};
  juce::TextButton midiMonitorToggle{"MIDI MON"};
  std::unique_ptr<MidiMonitorComponent> midiMonitorPanel;
  bool midiMonitorVisible = false;

  void changeListenerCallback(juce::ChangeBroadcaster *source) override;

  void handleIncomingMidiMessage(juce::MidiInput *,
                                 const juce::MidiMessage &msg) override {
    // Skip SysEx messages - they can cause issues and we don't need them
    if (msg.isSysEx())
      return;

    // Feed detailed monitor panel before any early returns so learned/triggered
    // events (and [CAPTURED] CC learn states) are always visible in the monitor.
    if (midiMonitorPanel) {
      midiMonitorPanel->addMidiMessage(msg);
    }

    // Intercept for Library MIDI Trigger Learning
    if (libraryPanel != nullptr && libraryPanel->isLearning()) {
      if (msg.isProgramChange()) {
        SetupMidiTrigger t;
        t.isProgramChange = true;
        t.number = msg.getProgramChangeNumber();
        t.channel = msg.getChannel();
        juce::Component::SafePointer<MainComponent> safeThis(this);
        juce::MessageManager::callAsync([safeThis, t]() {
          if (safeThis != nullptr && safeThis->libraryPanel != nullptr)
            safeThis->libraryPanel->assignLearnedTrigger(t);
        });
        return;
      }
      if (msg.isController() && msg.getControllerValue() > 64) {
        SetupMidiTrigger t;
        t.isProgramChange = false;
        t.number = msg.getControllerNumber();
        t.channel = msg.getChannel();
        juce::Component::SafePointer<MainComponent> safeThis(this);
        juce::MessageManager::callAsync([safeThis, t]() {
          if (safeThis != nullptr && safeThis->libraryPanel != nullptr)
            safeThis->libraryPanel->assignLearnedTrigger(t);
        });
        return;
      }
    }

    // Scene MIDI trigger learn - intercept next PC if armed
    const int armedScene = sceneMidiLearnArmed.load(std::memory_order_acquire);
    if (msg.isProgramChange() && armedScene >= 0) {
      int learnedPC = msg.getProgramChangeNumber();
      int learnedCh = msg.getChannel();
      sceneMidiLearnArmed.store(-1, std::memory_order_release);
      juce::Component::SafePointer<MainComponent> safeThis(this);
      juce::MessageManager::callAsync([safeThis, armedScene, learnedPC, learnedCh]() {
        if (safeThis == nullptr) return;
        safeThis->engine.setSceneMidiTrigger(armedScene, learnedPC, learnedCh);
        safeThis->sceneMidiLearnArmed.store(-1, std::memory_order_release);
        if (safeThis->sceneLearnAlert != nullptr) {
          safeThis->sceneLearnAlert->exitModalState(0);
          safeThis->sceneLearnAlert = nullptr;
        }
        safeThis->refreshSceneButtons();
      });
      return;
    }

    // CC Learn takes precedence over live setup triggers so wiggling an already-bound
    // controller knob while armed learns the CC instead of switching songs.
    if (msg.isController()) {
      int ccNum = msg.getControllerNumber();
      int ccVal = msg.getControllerValue();
      int channel = msg.getChannel();

      if (Fanfare::MidiLearnBus::getInstance().handleMidiCC(ccNum, ccVal, channel)) {
        return; // Intercepted by learn mode
      }
    }

    // Direct MIDI Trigger matching for Setups
    if (msg.isProgramChange()) {
      int pgNum = msg.getProgramChangeNumber();
      int channel = msg.getChannel();
      juce::String setupFile = SetupMidiTriggers::getInstance().findSetupForTrigger(true, pgNum, channel);
      if (setupFile.isNotEmpty()) {
        juce::Component::SafePointer<MainComponent> safeThis(this);
        juce::MessageManager::callAsync([safeThis, setupFile]() {
          if (safeThis != nullptr)
            safeThis->loadRigFromFile(Fanfare::RigLibrary::getSongsDirectory().getChildFile(setupFile));
        });
        return;
      }

      // Check if any scene has this PC assigned as a trigger
      int sceneByTrigger = engine.findSceneForProgram(pgNum, channel);
      if (sceneByTrigger >= 0) {
        juce::Component::SafePointer<MainComponent> safeThis(this);
        juce::MessageManager::callAsync([safeThis, sceneByTrigger]() {
          if (safeThis == nullptr) return;
          safeThis->engine.saveCurrentStateToScene(safeThis->engine.getCurrentSceneIndex());
          safeThis->engine.loadScene(sceneByTrigger);
          safeThis->refreshSceneButtons();
        });
        return;
      }

      // If it doesn't trigger a setup switch, use the PC number to select the scene index inside the current setup
      juce::Component::SafePointer<MainComponent> safeThis(this);
      juce::MessageManager::callAsync([safeThis, pgNum]() {
        if (safeThis == nullptr) return;
        safeThis->engine.saveCurrentStateToScene(safeThis->engine.getCurrentSceneIndex());
        if (pgNum >= 0 && pgNum < safeThis->engine.getNumScenes()) {
          safeThis->engine.loadScene(pgNum);
          safeThis->refreshSceneButtons();
        }
      });
      return;
    } else if (msg.isController() && msg.getControllerValue() > 64) {
      int ccNum = msg.getControllerNumber();
      int channel = msg.getChannel();
      juce::String setupFile = SetupMidiTriggers::getInstance().findSetupForTrigger(false, ccNum, channel);
      if (setupFile.isNotEmpty()) {
        juce::Component::SafePointer<MainComponent> safeThis(this);
        juce::MessageManager::callAsync([safeThis, setupFile]() {
          if (safeThis != nullptr)
            safeThis->loadRigFromFile(Fanfare::RigLibrary::getSongsDirectory().getChildFile(setupFile));
        });
        return;
      }
    }

    if (msg.isNoteOn() && msg.getVelocity() > 0 && msg.getChannel() == 10) {
      int note = msg.getNoteNumber();
      if (note >= 36 && note < 36 + numSetupButtons) {
        int setupIdx = note - 36;
        juce::Component::SafePointer<MainComponent> safeThis(this);
        juce::MessageManager::callAsync([safeThis, setupIdx]() {
          if (safeThis != nullptr)
            safeThis->loadRigFile(setupIdx);
        });
      }
    }

    midiCollector.addMessageToQueue(msg);

    // Route note-on to active sampler overlay for learning
    if (msg.isNoteOn() && samplerOverlayOpen.load(std::memory_order_acquire)) {
      int noteNum = msg.getNoteNumber();
      juce::Component::SafePointer<MainComponent> safeThis(this);
      juce::MessageManager::callAsync([safeThis, noteNum]() {
        if (safeThis != nullptr && safeThis->samplerOverlay != nullptr)
          safeThis->samplerOverlay->handleMidiNote(noteNum);
      });
    }

    // Route note-on to any active learner (NoteRangeComponent or per-plugin config).
    // Guarded by atomic flag so normal playing never allocates an async message.
    if (msg.isNoteOn() && msg.getVelocity() > 0 && hasMidiNoteLearner.load(std::memory_order_acquire)) {
      int noteNum = msg.getNoteNumber();
      juce::Component::SafePointer<MainComponent> safeThis(this);
      juce::MessageManager::callAsync([safeThis, noteNum]() {
        if (safeThis == nullptr) return;
        if (safeThis->activeMidiNoteLearner != nullptr) {
          if (auto* l = dynamic_cast<IMidiNoteLearner*>(safeThis->activeMidiNoteLearner.getComponent()))
            l->handleMidiNote(noteNum);
        } else {
          safeThis->hasMidiNoteLearner.store(false, std::memory_order_release);
        }
      });
    }
  }

  // Active MIDI note learner. Can be a NoteRangeComponent, PluginStackConfigComp,
  // or any Component implementing IMidiNoteLearner. Held as a generic SafePointer
  // because the CallOutBox owns/deletes the component and may dismiss at any time.
  juce::Component::SafePointer<juce::Component> activeMidiNoteLearner;
  std::atomic<bool> hasMidiNoteLearner{false};
  std::atomic<bool> samplerOverlayOpen{false};

  // Scene MIDI trigger learn state
  std::atomic<int> sceneMidiLearnArmed{-1};  // scene index being learned, -1 = not armed
  juce::Component::SafePointer<juce::AlertWindow> sceneLearnAlert;

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MainComponent)
};
