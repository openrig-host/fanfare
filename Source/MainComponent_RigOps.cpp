#// MainComponent_RigOps.cpp - part of MainComponent (mechanical split, see nov8mistral.md)
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

    btn->onClicked = [this, sceneIdx](int) { recallScene(sceneIdx); };

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
      juce::Component::SafePointer<MainComponent> safeThis(this);
      menu.showMenuAsync(juce::PopupMenu::Options{}.withTargetComponent(b),
        [safeThis, sceneIdx](int result) {
          if (safeThis == nullptr)
            return;
          if (result == 1) {
            auto fileChooser = std::make_shared<juce::FileChooser>(
              "Select Song File...", Fanfare::RigLibrary::getSongsDirectory(), "*.json");
            fileChooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
              [safeThis, sceneIdx, fileChooser](const juce::FileChooser& fc) {
                if (safeThis == nullptr)
                  return;
                auto file = fc.getResult();
                if (file.existsAsFile()) {
                  while (safeThis->sceneSetupFilePaths.size() <= sceneIdx)
                    safeThis->sceneSetupFilePaths.add("");
                  safeThis->sceneSetupFilePaths.set(sceneIdx, file.getFullPathName());
                  safeThis->saveButtonMappings();
                  safeThis->refreshSceneButtons();
                }
              });
          } else if (result == 2) {
            if (sceneIdx < safeThis->sceneSetupFilePaths.size())
              safeThis->sceneSetupFilePaths.set(sceneIdx, "");
            safeThis->saveButtonMappings();
            safeThis->refreshSceneButtons();
          } else if (result == 3) {
            if (safeThis->sceneLearnAlert != nullptr) {
              safeThis->sceneLearnAlert->exitModalState(0);
              safeThis->sceneLearnAlert = nullptr;
            }
            auto* alert = new juce::AlertWindow(
              "MIDI Learn",
              "Send a Program Change from your controller now...",
              juce::AlertWindow::InfoIcon);
            alert->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));

            safeThis->sceneMidiLearnArmed.store(sceneIdx, std::memory_order_release);
            alert->enterModalState(true,
              juce::ModalCallbackFunction::create([safeThis](int) {
                if (safeThis != nullptr)
                  safeThis->sceneMidiLearnArmed.store(-1, std::memory_order_release);
              }),
              true);
            safeThis->sceneLearnAlert = alert;
          } else if (result == 4) {
            safeThis->engine.clearSceneMidiTrigger(sceneIdx);
            safeThis->saveButtonMappings();
            safeThis->refreshSceneButtons();
          }
        });
    };

    addAndMakeVisible(btn);
    sceneButtons.add(btn);
  }
  updateStripCollapseStates();
  resized();
}

// Shared scene recall path: assigned song file loads asynchronously, otherwise
// the engine scene snapshot is restored. Used by scene buttons, the overflow
// menu and the 1-9 hotkeys (nov8mistral.md 1.2).
void MainComponent::recallScene(int sceneIdx) {
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
    nextUpLabel.setVisible(true);
    juce::String nextName = preloadTarget.getFileNameWithoutExtension();
    juce::String capsuleText;
    juce::Colour stateCol;
    if (sm.isPreloading()) {
      preloadStatusLabel.setText("Preloading next: " + nextName + "...", juce::dontSendNotification);
      capsuleText = "Next: " + nextName + " (loading...)";
      stateCol = ThemeManager::get(Theme::Role::warn);
    } else if (sm.isPreloaded()) {
      preloadStatusLabel.setText("Next Ready: " + nextName, juce::dontSendNotification);
      capsuleText = "Next ready: " + nextName;
      stateCol = ThemeManager::get(Theme::Role::ok);
    } else if (sm.isPreloadFailed()) {
      preloadStatusLabel.setText("Preload Failed: " + nextName, juce::dontSendNotification);
      capsuleText = "Next failed: " + nextName;
      stateCol = ThemeManager::get(Theme::Role::danger);
    } else {
      preloadStatusLabel.setText("Next: " + nextName + " (Not Preloaded)", juce::dontSendNotification);
      capsuleText = "Next: " + nextName;
      stateCol = ThemeManager::get(Theme::Role::textDim);
    }
    preloadStatusLabel.setColour(juce::Label::textColourId, stateCol);
    nextUpLabel.setText(capsuleText, juce::dontSendNotification);
    nextUpLabel.setColour(juce::Label::textColourId, stateCol);
  } else {
    preloadStatusLabel.setText("", juce::dontSendNotification);
    preloadStatusLabel.setVisible(false);
    nextUpLabel.setText("", juce::dontSendNotification);
    nextUpLabel.setVisible(false);
  }
}

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
  Fanfare::SetlistManager::getInstance().prepareForRigLoad(file);
  SaveHistory::snapshotBeforeLoadAsync(file);
  showLoadingOverlay("SWITCHING SONG",
                     "Loading " + file.getFileNameWithoutExtension() + "...");
  if (loadingOverlay) {
    juce::Component::SafePointer<MainComponent> cancelSafeThis(this);
    loadingOverlay->setOnCancel([cancelSafeThis]() {
      if (cancelSafeThis != nullptr && cancelSafeThis->transitioner)
        cancelSafeThis->transitioner->requestCancel();
    });
  }

  int highlight = buttonIndexForHighlight;

  // Defer the transition slightly to guarantee that the message thread paints
  // the LoadingOverlay before heavy VST builds block the event loop.
  juce::Component::SafePointer<MainComponent> safeThis(this);
  juce::Timer::callAfterDelay(50, [safeThis, file, highlight, targetSetlistIndex]() {
    if (safeThis == nullptr || !safeThis->transitioner)
      return;

    safeThis->transitioner->transitionToFile(file,
        Fanfare::RigTransitioner::Callbacks{
            [safeThis](juce::String progress) {
                if (safeThis != nullptr)
                    safeThis->setLoadingMessage(progress);
            },
            [safeThis, highlight, file, targetSetlistIndex, fileName = file.getFileNameWithoutExtension()](bool ok, juce::String message, int builtCount, const std::vector<Fanfare::RigBuilder::FailedEntry>& failed) {
                juce::ignoreUnused(highlight, builtCount);
                if (safeThis == nullptr)
                    return;
                safeThis->hideLoadingOverlay();
                safeThis->closeOrphanedPluginWindows();
                if (ok) {
                    safeThis->suppressGlitchDetectionForMs(500);
                    safeThis->audioUnderrunCount.store(0);
                    safeThis->engineStallCount.store(0);
                    safeThis->currentRigFile = file;
                    Fanfare::RecentSongs::record(file);
                    safeThis->setupNameLabel.setText(fileName, juce::dontSendNotification);
                    safeThis->freshBuildToggle.setToggleState(safeThis->engine.isCurrentRigFreshBuild(), juce::dontSendNotification);
                    for (auto *comp : safeThis->rackSlotComponents) {
                        comp->updateOutputSelector();
                        comp->repaint();
                    }
                    safeThis->repaint();
                    safeThis->refreshSceneButtons();
                    if (safeThis->autoCollapseToggle.getToggleState()) {
                        safeThis->updateStripCollapseStates();
                    } else {
                        for (auto *comp : safeThis->rackSlotComponents)
                            comp->setCollapsed(false);
                        safeThis->resized();
                    }
                    // Active-queue highlighting is driven by updatePreloadStatus()
                    // via the SetlistManager change broadcaster (setActiveIndex).
                    safeThis->midiMonitorLabel.setText("SONG: " + message,
                                                       juce::dontSendNotification);

                    if (!failed.empty()) {
                        safeThis->showMissingPluginsWarning(failed);
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
                    if (message == "Cancelled") {
                        safeThis->midiMonitorLabel.setText(
                            "SONG: Switch cancelled (current song unchanged)",
                            juce::dontSendNotification);
                        LOG_INFO("Rig transition cancelled by user for: " + fileName);
                    } else {
                        juce::AlertWindow::showAsync(
                            juce::MessageBoxOptions()
                                .withIconType(juce::MessageBoxIconType::WarningIcon)
                                .withTitle("Song Load Failed")
                                .withMessage(message + "\n\nThe current song is unchanged. Open Rig Doctor to diagnose and repair this setup?")
                                .withButton("Open Rig Doctor")
                                .withButton("Close"),
                            [safeThis, file](int result) {
                                if (result == 1 && safeThis != nullptr)
                                    safeThis->showRigDoctorModal(file);
                            });
                        LOG_ERROR("Failed to load rig: " + message + " file: " +
                                  fileName);
                    }
                }
            }
        });
  });
}

void MainComponent::assignJsonToButton(int buttonIndex) {
  if (buttonIndex < 0 || buttonIndex >= numSetupButtons)
    return;
  auto setupsDir = Fanfare::RigLibrary::getSongsDirectory();
  setupsDir.createDirectory();

  auto chooser = std::make_shared<juce::FileChooser>("Select JSON Song File",
                                                     setupsDir, "*.json");

  juce::Component::SafePointer<MainComponent> safeThis(this);
  chooser->launchAsync(
      juce::FileBrowserComponent::openMode |
          juce::FileBrowserComponent::canSelectFiles,
      [safeThis, buttonIndex, chooser](const juce::FileChooser &fc) {
        if (safeThis == nullptr || buttonIndex < 0 || buttonIndex >= numSetupButtons)
          return;
        auto file = fc.getResult();
        if (file.existsAsFile()) {
          safeThis->setupFilePaths[buttonIndex] = file.getFullPathName();
          safeThis->saveButtonMappings();
          auto& sm = Fanfare::SetlistManager::getInstance();
          sm.setSlotSetup(buttonIndex, file);
          safeThis->updatePreloadStatus();
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

void MainComponent::loadRigFile(int index) {
  if (index < 0 || index >= numSetupButtons)
    return;
  currentSetupIndex = index;
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

void MainComponent::saveSetToFile() {
  fileChooser = std::make_unique<juce::FileChooser>(
      "Save Set...",
      Fanfare::RigLibrary::getSetsDirectory(),
      "*.orset");
  juce::Component::SafePointer<MainComponent> safeThis(this);
  fileChooser->launchAsync(
      juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles,
      [safeThis](const juce::FileChooser &fc) {
          if (safeThis == nullptr)
              return;
          auto file = fc.getResult();
          if (file == juce::File{})
              return;
          if (file.getFileExtension() != ".orset")
              file = file.withFileExtension(".orset");

          juce::DynamicObject::Ptr obj = new juce::DynamicObject();
          for (int i = 0; i < numSetupButtons; ++i) {
              if (!safeThis->setupFilePaths[i].isEmpty())
                  obj->setProperty("setup_" + juce::String(i), safeThis->setupFilePaths[i]);
          }

          SaveHistory::recordSave(file);
          const juce::String json = juce::JSON::toString(juce::var(obj.get()));
          if (Fanfare::RigSerializer::save(file, json)) {
              SaveHistory::recordSave(file);
          } else {
              juce::AlertWindow::showMessageBoxAsync(
                  juce::MessageBoxIconType::WarningIcon, "Save Failed",
                  "Could not save queue map to:\n" + file.getFullPathName());
          }
      });
}

void MainComponent::loadSetFromFile() {
  fileChooser = std::make_unique<juce::FileChooser>(
      "Load Set...",
      Fanfare::RigLibrary::getSetsDirectory(),
      "*.orset");
  juce::Component::SafePointer<MainComponent> safeThis(this);
  fileChooser->launchAsync(
      juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
      [safeThis](const juce::FileChooser &fc) {
          if (safeThis != nullptr)
              safeThis->applySetlistFromFile(fc.getResult());
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
void MainComponent::resetToNewSong() {
  bool anyLoaded = false;
  for (int i = 0; i < engine.getNumSlots() && !anyLoaded; ++i)
    if (auto *s = engine.getSlot(i))
      for (int c = 0; c < 3; ++c)
        if (s->getPluginInstance(c) != nullptr || s->isChainSlotMidiOut(c) ||
            s->isChainSlotSynth(c) || s->isChainSlotFm(c))
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
