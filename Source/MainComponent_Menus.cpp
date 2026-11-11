#// MainComponent_Menus.cpp - part of MainComponent (mechanical split, see nov8mistral.md)
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

#include "MainComponentInternal.h"

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

      footerLabel.setText("No virtual patch cords. No node graphs. Just pure stage-tested horsepower.\n"
                          "CoplanDX engine: msfa (Apache 2.0, (c) 2012 Google Inc.) as used in Dexed (GPL v3). See NOTICE.md.",
                          juce::dontSendNotification);
      footerLabel.setFont(juce::FontOptions(12.0f, juce::Font::italic));
      footerLabel.setColour(juce::Label::textColourId, ThemeManager::get(Theme::Role::textDim));
      footerLabel.setJustificationType(juce::Justification::topLeft);
      addAndMakeVisible(footerLabel);

      setSize(470, 166);
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
      footerLabel.setBounds(bounds.removeFromTop(34));
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

void MainComponent::showRigDoctorModal(const juce::File &file) {
  juce::File target = file.existsAsFile() ? file : currentRigFile;
  if (!target.existsAsFile() && libraryPanel != nullptr) {
    auto sel = libraryPanel->getSelectedPresetFile();
    if (sel.existsAsFile() && sel.hasFileExtension(".json"))
      target = sel;
  }

  Fanfare::RigDoctorModal::Actions a;
  a.onClose = [this] {
    rigDoctorOverlay.reset();
    if (libraryPanel != nullptr)
      libraryPanel->refreshAll();
    resized();
  };
  a.onLoadSong = [this](const juce::File &repairedFile) {
    if (libraryPanel != nullptr)
      libraryPanel->refreshAll();
    loadRigFromFile(repairedFile);
  };

  rigDoctorOverlay = std::make_unique<Fanfare::RigDoctorModal>(engine, target, std::move(a));
  addAndMakeVisible(rigDoctorOverlay.get());
  rigDoctorOverlay->centreWithSize(juce::jmin(getWidth() - 28, 920),
                                   juce::jmin(getHeight() - 28, 620));
  rigDoctorOverlay->toFront(true);
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


void MainComponent::showLoadingOverlay(const juce::String &title, const juce::String &message) {
  if (!loadingOverlay)
    loadingOverlay = std::make_unique<LoadingOverlay>();
  loadingOverlay->resetProgress();
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
    info.reason = fe.reason;
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
    juce::File diagFile = currentRigFile;
    missingPluginsToast = std::make_unique<MissingPluginsToast>(
        missing,
        [this]() {
          missingPluginsToast.reset();
        },
        [this, diagFile]() {
          showRigDoctorModal(diagFile);
        });
    addAndMakeVisible(missingPluginsToast.get());
    updateMissingPluginsToastBounds();
    missingPluginsToast->toFront(true);

    juce::String logMsg = "Setup loaded with " + juce::String((int)missing.size()) + " plugin issue(s): ";
    for (size_t i = 0; i < missing.size(); ++i) {
      if (i > 0) logMsg += ", ";
      logMsg += missing[i].name + " [" + missing[i].location + "]";
      if (missing[i].reason.isNotEmpty())
        logMsg += " (" + missing[i].reason + ")";
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
  else if (!key.getModifiers().isCommandDown() && !key.getModifiers().isAltDown()) {
    // Keys 1-9 recall scenes (nov8mistral.md 1.2). The TextEditor guard at the
    // top of this handler keeps digits typed into search/rename fields safe.
    const int digit = key.getKeyCode() - '0';
    if (digit >= 1 && digit <= 9 && digit <= engine.getNumScenes()) {
      recallScene(digit - 1);
      return true;
    }
  }
  return false;
}

void MainComponent::showWebRemoteDialog() {
  if (stageRemoteOverlay != nullptr) {
    stageRemoteOverlay.reset();
    return;
  }
  if (hotspotController == nullptr) {
    hotspotController = std::make_unique<OpenRig::StageHotspotController>();
  }
  const int port = webServer ? webServer->getPort() : 8080;
  const juce::String token = webServer ? webServer->getSessionToken() : juce::String();
  stageRemoteOverlay = std::make_unique<OpenRig::StageRemoteModalOverlay>(*hotspotController, port, token);
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
  miNotepad = 300, miInspector, miRigDoctor, miRemote,
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
  juce::Component::SafePointer<MainComponent> safeThis(this);
  m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&sceneMenuBtn),
                  [safeThis, name](int r) {
    if (safeThis == nullptr)
      return;
    if (r == 1 && safeThis->saveSceneBtn.onClick) {
      safeThis->saveSceneBtn.onClick();
    } else if (r == 2 && safeThis->renameSceneBtn.onClick) {
      safeThis->renameSceneBtn.onClick();
    } else if (r == 3) {
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
      m.addItem(menuItem(miRigDoctor, "Diagnose & repair song (Rig Doctor)..."));
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
      m.addItem(menuItem(miRigDoctor, "Rig doctor (diagnose & repair)..."));
      m.addItem(menuItem(miRemote, "Tablet remote..."));
      m.addItem(menuItem(miViewContextBase + Fanfare::ToolRail::Midi,
                         "MIDI monitor", {},
                         sidebarContext == Fanfare::ToolRail::Midi));
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
    const int targetCtx = id - miViewContextBase;
    setSidebarContext(sidebarContext == targetCtx ? -1 : targetCtx);
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
    case miRigDoctor:  showRigDoctorModal(); break;
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

