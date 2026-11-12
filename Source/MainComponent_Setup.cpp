// MainComponent_Setup.cpp - part of MainComponent (mechanical split, see nov8mistral.md)
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

void MainComponent::setupSlotComponents() {
  // Build input selector data
  std::vector<std::pair<juce::String, int>> inputItems;
  inputItems.push_back({"No Input", -1});
  auto *device = deviceManager.getCurrentAudioDevice();
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
    comp->onShowPluginMenu = [this, i, slot, comp, isMonitorIn](int chainIndex) {
      juce::PopupMenu menu;
      menu.addSectionHeader("Select Plugin for Chain " +
                            juce::String(chainIndex + 1));
      if (!isMonitorIn) {
        menu.addItem(-4, "BLEEP-216 Polysynth (Built-in)", true,
                     slot != nullptr && slot->isChainSlotSynth(chainIndex));
        menu.addItem(-5, "CoplanDX (Built-in)", true,
                     slot != nullptr && slot->isChainSlotFm(chainIndex));
        menu.addSeparator();
      }
      if (engine.getNumAvailablePlugins() == 0) {
        menu.addItem(-3, "Scan for VST3 Plugins...");
      } else {
        for (int p = 0; p < engine.getNumAvailablePlugins(); ++p)
          menu.addItem(p + 1, engine.getAvailablePluginName(p));
      }
      menu.addSeparator();
      menu.addItem(-2, "Send MIDI OUT...", slot != nullptr && !slot->isChainSlotMidiOut(chainIndex));
      menu.addItem(-1, "(No Plugin)");

      juce::Component::SafePointer<MainComponent> safeThis(this);
      juce::Component::SafePointer<RackSlotComponent> safeComp(comp);
      menu.showMenuAsync(juce::PopupMenu::Options(), [safeThis, i, slot, safeComp,
                                                      chainIndex](int result) {
        if (safeThis == nullptr)
          return;
        if (result == -4) {
          if (slot != nullptr) {
            if (auto *oldInstance = slot->getPluginInstance(chainIndex))
              safeThis->closePluginWindowForInstance(oldInstance);
            safeThis->engine.clearSlotChainMidiOut(i, chainIndex);
            slot->setChainSlotSynth(chainIndex, true);
            if (safeComp != nullptr) {
              if (safeComp->getIsCollapsed()) {
                safeComp->setCollapsed(false);
                safeThis->resized();
              }
              safeComp->repaint();
            }
            safeThis->synthOverlay.reset(new FanfareSynthComponent(
                *slot,
                [safeThis] {
                  if (safeThis != nullptr) {
                    safeThis->synthOverlay.reset();
                    safeThis->resized();
                  }
                },
                chainIndex));
            safeThis->addAndMakeVisible(safeThis->synthOverlay.get());
            safeThis->synthOverlay->updateForHostBounds(safeThis->getLocalBounds());
          }
          return;
        }
        if (result == -5) {
          if (slot != nullptr) {
            if (auto *oldInstance = slot->getPluginInstance(chainIndex))
              safeThis->closePluginWindowForInstance(oldInstance);
            safeThis->engine.clearSlotChainMidiOut(i, chainIndex);
            slot->setChainSlotFm(chainIndex, true);
            if (safeComp != nullptr) {
              if (safeComp->getIsCollapsed()) {
                safeComp->setCollapsed(false);
                safeThis->resized();
              }
              safeComp->repaint();
            }
            safeThis->fmOverlay.reset(new FmSynthComponent(
                *slot,
                [safeThis] {
                  if (safeThis != nullptr) {
                    safeThis->fmOverlay.reset();
                    safeThis->resized();
                  }
                },
                chainIndex));
            safeThis->addAndMakeVisible(safeThis->fmOverlay.get());
            safeThis->fmOverlay->updateForHostBounds(safeThis->getLocalBounds());
          }
          return;
        }
        if (result == -3) {
          if (safeThis->scanButton.onClick)
            safeThis->scanButton.onClick();
          return;
        }
        if (result == -2) {
          safeThis->showSlotMidiOutDialog(i, chainIndex);
          return;
        }
        if (result > 0 || result == -1) {
          if (slot != nullptr) {
            if (auto *oldInstance = slot->getPluginInstance(chainIndex)) {
              safeThis->closePluginWindowForInstance(oldInstance);
            }
          }
        }
        if (result > 0) {
          safeThis->loadingOverlay.reset(new LoadingOverlay());
          safeThis->addAndMakeVisible(safeThis->loadingOverlay.get());

          safeThis->engine.loadPluginIntoSlot(
              i, chainIndex, result - 1,
              [safeThis, safeComp](bool success, const juce::String &err) {
                if (safeThis == nullptr)
                  return;
                safeThis->loadingOverlay.reset();
                if (!success)
                  juce::AlertWindow::showMessageBoxAsync(
                      juce::MessageBoxIconType::WarningIcon,
                      "Plugin Load Failed", err);
                else if (safeComp != nullptr) {
                  if (safeComp->getIsCollapsed()) {
                    safeComp->setCollapsed(false);
                    safeThis->resized();
                  }
                  safeComp->repaint();
                }
              });
        } else if (result == -1 && slot != nullptr) {
          safeThis->engine.clearSlotChainMidiOut(i, chainIndex);
          slot->clearChainSlotSynth(chainIndex);
          slot->clearChainSlotFm(chainIndex);
          juce::ScopedLock sl(safeThis->engine.getCallbackLock());
          slot->setPluginInChain(chainIndex, nullptr);
          if (safeComp != nullptr)
            safeComp->repaint();
        }
      });
    };

    // MIDI OUT destination editor (reuses the slot's chain config + dialog)
    comp->onEditMidiOut = [this, i](int chainIndex) {
      showSlotMidiOutDialog(i, chainIndex);
    };

    // Editor open callback
    comp->onOpenEditor = [this, slot](int chainIndex) {
      if (slot != nullptr && slot->isChainSlotSynth(chainIndex)) {
        synthOverlay.reset(new FanfareSynthComponent(
            *slot,
            [this] {
              synthOverlay.reset();
              resized();
            },
            chainIndex));
        addAndMakeVisible(synthOverlay.get());
        synthOverlay->updateForHostBounds(getLocalBounds());
        return;
      }
      if (slot != nullptr && slot->isChainSlotFm(chainIndex)) {
        fmOverlay.reset(new FmSynthComponent(
            *slot,
            [this] {
              fmOverlay.reset();
              resized();
            },
            chainIndex));
        addAndMakeVisible(fmOverlay.get());
        fmOverlay->updateForHostBounds(getLocalBounds());
        return;
      }
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
      juce::Component::SafePointer<MainComponent> safeThis(this);
      juce::MessageManager::callAsync([safeThis, slot, chainIndex]() {
        if (safeThis == nullptr)
          return;
        auto *plugin = slot->getPluginInstance(chainIndex);
        if (!plugin) {
          safeThis->hideLoadingOverlay();
          return;
        }
        for (auto *existing : safeThis->activePluginWindows) {
          if (auto *pw = dynamic_cast<PluginWindow*>(existing)) {
            if (pw->pluginInstance == plugin) {
              safeThis->hideLoadingOverlay();
              pw->toFront(true);
              return;
            }
          }
        }
        juce::AudioProcessorEditor* editor = nullptr;
        bool ok = FanfareLog::safeExecutePluginCall([&]() {
          editor = plugin->createEditor();
        }, "createEditor (" + plugin->getName() + ")");
        if (safeThis == nullptr)
          return;
        safeThis->hideLoadingOverlay();
        if (ok && editor != nullptr) {
          auto *w = new PluginWindow(slot->getPluginName(chainIndex), editor, plugin);
          w->onWindowClosed = [safeThis, w] {
            if (safeThis == nullptr)
              return;
            if (safeThis->activePluginWindows.contains(w)) {
              safeThis->activePluginWindows.removeObject(w, false);
              w->destroyEditorNow();
              juce::MessageManager::callAsync([w]() { delete w; });
            }
          };
          safeThis->activePluginWindows.add(w);
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

    // Fanfare Synth Dialog (native instrument — opens active synth slot or
    // binds the first free chain slot so SYN-106 shows up in SLOT 1..3)
    comp->onShowSynth = [this, slot, comp] {
      int targetChain = slot->getFirstSynthChainSlot();
      if (targetChain < 0) {
        for (int c = 0; c < 3; ++c) {
          if (slot->isChainSlotFm(c) && comp->onShowFm) {
            comp->onShowFm();
            return;
          }
        }
        targetChain = 0;
        for (int c = 0; c < 3; ++c) {
          if (!slot->getPluginInstance(c) && !slot->isChainSlotMidiOut(c) &&
              !slot->isChainSlotSynth(c) && !slot->isChainSlotFm(c)) {
            targetChain = c;
            break;
          }
        }
        if (auto *oldInstance = slot->getPluginInstance(targetChain))
          closePluginWindowForInstance(oldInstance);
        slot->setChainSlotSynth(targetChain, true);
        if (comp->getIsCollapsed()) {
          comp->setCollapsed(false);
          resized();
        }
        comp->repaint();
      } else if (!slot->isChainSlotSynth(targetChain) &&
                 !slot->getPluginInstance(targetChain) &&
                 !slot->isChainSlotMidiOut(targetChain)) {
        slot->setChainSlotSynth(targetChain, true);
        comp->repaint();
      }
      synthOverlay.reset(new FanfareSynthComponent(
          *slot,
          [this] {
            synthOverlay.reset();
            resized();
          },
          targetChain));

      synthOverlay->onOpenNoteRange = [this, slot, comp] {
        auto *nrComp = new NoteRangeComponent(*slot);
        nrComp->setSize(860, 358);
        setActiveMidiNoteLearner(nrComp);
        auto targetArea = synthOverlay != nullptr ? synthOverlay->getMidiButtonScreenBounds()
                                                  : comp->getScreenBounds();
        auto &box = juce::CallOutBox::launchAsynchronously(
            std::unique_ptr<juce::Component>(nrComp), targetArea, this);
        box.setDismissalMouseClicksAreAlwaysConsumed(true);
      };

      synthOverlay->onOpenCCDialog = [this, slot, comp] {
        auto *ccComp = new CCMappingComponent(*slot, 0);
        ccComp->setSize(660, 510);
        auto targetArea = synthOverlay != nullptr ? synthOverlay->getMidiButtonScreenBounds()
                                                  : comp->getScreenBounds();
        auto &box = juce::CallOutBox::launchAsynchronously(
            std::unique_ptr<juce::Component>(ccComp), targetArea, this);
        box.setDismissalMouseClicksAreAlwaysConsumed(true);
      };

      synthOverlay->onOpenMidiEffects = comp->onShowArpeggiator;

      addAndMakeVisible(synthOverlay.get());
      synthOverlay->updateForHostBounds(getLocalBounds());
    };

    // CoplanDX Dialog (native instrument — opens active FM slot or binds the
    // first free chain slot so CoplanDX shows up in SLOT 1..3)
    comp->onShowFm = [this, slot, comp] {
      int targetChain = -1;
      for (int c = 0; c < 3; ++c)
        if (slot->isChainSlotFm(c)) {
          targetChain = c;
          break;
        }
      if (targetChain < 0) {
        targetChain = 0;
        for (int c = 0; c < 3; ++c) {
          if (!slot->getPluginInstance(c) && !slot->isChainSlotMidiOut(c) &&
              !slot->isChainSlotSynth(c) && !slot->isChainSlotFm(c)) {
            targetChain = c;
            break;
          }
        }
        if (auto *oldInstance = slot->getPluginInstance(targetChain))
          closePluginWindowForInstance(oldInstance);
        slot->setChainSlotFm(targetChain, true);
        if (comp->getIsCollapsed()) {
          comp->setCollapsed(false);
          resized();
        }
        comp->repaint();
      }
      fmOverlay.reset(new FmSynthComponent(
          *slot,
          [this] {
            fmOverlay.reset();
            resized();
          },
          targetChain));

      fmOverlay->onOpenNoteRange = [this, slot, comp] {
        auto *nrComp = new NoteRangeComponent(*slot);
        nrComp->setSize(860, 358);
        setActiveMidiNoteLearner(nrComp);
        auto targetArea = fmOverlay != nullptr ? fmOverlay->getMidiButtonScreenBounds()
                                               : comp->getScreenBounds();
        auto &box = juce::CallOutBox::launchAsynchronously(
            std::unique_ptr<juce::Component>(nrComp), targetArea, this);
        box.setDismissalMouseClicksAreAlwaysConsumed(true);
      };

      fmOverlay->onOpenCCDialog = [this, slot, comp] {
        auto *ccComp = new CCMappingComponent(*slot, 0);
        ccComp->setSize(660, 510);
        auto targetArea = fmOverlay != nullptr ? fmOverlay->getMidiButtonScreenBounds()
                                               : comp->getScreenBounds();
        auto &box = juce::CallOutBox::launchAsynchronously(
            std::unique_ptr<juce::Component>(ccComp), targetArea, this);
        box.setDismissalMouseClicksAreAlwaysConsumed(true);
      };

      fmOverlay->onOpenMidiEffects = comp->onShowArpeggiator;

      addAndMakeVisible(fmOverlay.get());
      fmOverlay->updateForHostBounds(getLocalBounds());
    };

    // Note Range Dialog (CallOutBox)
    comp->onShowNoteRangeDialog = [this, slot, comp] {
      auto *nrComp = new NoteRangeComponent(*slot);
      nrComp->setSize(860, 358);
      setActiveMidiNoteLearner(nrComp);
      auto &box = juce::CallOutBox::launchAsynchronously(
          std::unique_ptr<juce::Component>(nrComp),
          getLocalArea(comp, comp->getNoteRangeButtonBounds()), this);
      box.setDismissalMouseClicksAreAlwaysConsumed(true);
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
              if (slot->getPluginInstance(chainIdx) != nullptr ||
                  slot->isChainSlotMidiOut(chainIdx) ||
                  slot->isChainSlotSynth(chainIdx) ||
                  slot->isChainSlotFm(chainIdx)) {
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
      juce::Component::SafePointer<MainComponent> safeThis(this);
      juce::MessageManager::callAsync([safeThis, slot, chainIndex]() {
        if (safeThis == nullptr)
          return;
        auto *plugin = slot->getPluginInstance(chainIndex);
        if (!plugin) {
          safeThis->hideLoadingOverlay();
          return;
        }
        for (auto *existing : safeThis->activePluginWindows) {
          if (auto *pw = dynamic_cast<PluginWindow*>(existing)) {
            if (pw->pluginInstance == plugin) {
              safeThis->hideLoadingOverlay();
              pw->toFront(true);
              return;
            }
          }
        }
        juce::AudioProcessorEditor* editor = nullptr;
        bool ok = FanfareLog::safeExecutePluginCall([&]() {
          editor = plugin->createEditor();
        }, "createEditor (" + plugin->getName() + ")");
        if (safeThis == nullptr)
          return;
        safeThis->hideLoadingOverlay();
        if (ok && editor != nullptr) {
          auto *w = new PluginWindow(slot->getPluginName(chainIndex), editor, plugin);
          w->onWindowClosed = [safeThis, w] {
            if (safeThis == nullptr)
              return;
            if (safeThis->activePluginWindows.contains(w)) {
              safeThis->activePluginWindows.removeObject(w, false);
              w->destroyEditorNow();
              juce::MessageManager::callAsync([w]() { delete w; });
            }
          };
          safeThis->activePluginWindows.add(w);
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
      nrComp->setSize(860, 358);
      setActiveMidiNoteLearner(nrComp);
      auto &box = juce::CallOutBox::launchAsynchronously(
          std::unique_ptr<juce::Component>(nrComp),
          getLocalArea(comp, comp->getNoteRangeButtonBounds()), this);
      box.setDismissalMouseClicksAreAlwaysConsumed(true);
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

  // Legacy midiMonitorToggle hidden by Studio shell; route via setSidebarContext
  midiMonitorToggle.setVisible(false);
  midiMonitorToggle.onClick = [this] {
    setSidebarContext(sidebarContext == Fanfare::ToolRail::Midi
                          ? -1
                          : Fanfare::ToolRail::Midi);
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

    juce::Component::SafePointer<MainComponent> safeThis(this);
    menu.showMenuAsync(juce::PopupMenu::Options(), [safeThis](int result) {
      if (safeThis == nullptr)
        return;
      if (result >= 1 && result <= 4) {
        safeThis->engine.setFohOutputOffset((result - 1) * 2);
      } else if (result >= 11 && result <= 14) {
        safeThis->engine.setIemOutputOffset((result - 11) * 2);
      } else if (result == 100) {
        safeThis->engine.setDefaultMidiChannel(0);
      } else if (result >= 101 && result <= 116) {
        safeThis->engine.setDefaultMidiChannel(result - 100);
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

    juce::Component::SafePointer<MainComponent> safeThis(this);
    chooser->launchAsync(
        juce::FileBrowserComponent::saveMode |
            juce::FileBrowserComponent::canSelectFiles,
        [safeThis, chooser](const juce::FileChooser &fc) {
          if (safeThis == nullptr)
            return;
          auto file = fc.getResult();
          if (file != juce::File()) {
            if (file.getFileExtension() != ".json")
              file = file.withFileExtension(".json");

            juce::String json = safeThis->engine.exportRigToJson();
            SaveHistory::recordSave(file);
            if (Fanfare::RigSerializer::save(file, json)) {
              SaveHistory::recordSave(file);
              safeThis->currentRigFile = file;
              safeThis->setupNameLabel.setText(file.getFileNameWithoutExtension(), juce::dontSendNotification);
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

    juce::Component::SafePointer<MainComponent> safeThis(this);
    chooser->launchAsync(juce::FileBrowserComponent::openMode |
                             juce::FileBrowserComponent::canSelectFiles,
                          [safeThis, chooser](const juce::FileChooser &fc) {
                            if (safeThis == nullptr)
                              return;
                            auto file = fc.getResult();
                            if (file.existsAsFile())
                              safeThis->loadRigAsync(file);
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
  preloadStatusLabel.setMouseCursor(juce::MouseCursor::PointingHandCursor);
  preloadStatusLabel.setTooltip("Next song in setlist/queue - click to load it now");
  preloadStatusLabel.addMouseListener(this, false);

  // Next-up capsule for the empty center of the top bar (nov8mistral.md 1.1)
  addAndMakeVisible(nextUpLabel);
  nextUpLabel.setFont(BoutiqueLookAndFeel::condensedFont(13.0f, 0.04f));
  nextUpLabel.setJustificationType(juce::Justification::centred);
  nextUpLabel.setTooltip("Next song in setlist/queue - click to load it now");
  nextUpLabel.setMouseCursor(juce::MouseCursor::PointingHandCursor);
  nextUpLabel.addMouseListener(this, false);

  // Audio device / sample rate / buffer readout in the status bar (nov8mistral.md 1.6)
  addAndMakeVisible(audioDeviceLabel);
  audioDeviceLabel.setFont(BoutiqueLookAndFeel::monoFont(11.0f, true));
  audioDeviceLabel.setJustificationType(juce::Justification::centredLeft);
  audioDeviceLabel.setTooltip("Audio device, sample rate and buffer size - click for Audio & MIDI settings");
  audioDeviceLabel.setMouseCursor(juce::MouseCursor::PointingHandCursor);
  audioDeviceLabel.addMouseListener(this, false);

  midiMonitorLabel.setMouseCursor(juce::MouseCursor::PointingHandCursor);
  midiMonitorLabel.setTooltip("Click to open the MIDI monitor in the sidebar");
  midiMonitorLabel.addMouseListener(this, false);

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

  // Overflow button: lists scenes that no longer fit in the scene row
  addAndMakeVisible(sceneOverflowBtn);
  Fanfare::setButtonRole(sceneOverflowBtn, Fanfare::ButtonRole::Subtle);
  sceneOverflowBtn.setTooltip("Scenes hidden by a narrow window - click to recall one");
  sceneOverflowBtn.onClick = [this] {
    juce::PopupMenu m;
    m.addSectionHeader("More scenes");
    for (int idx : hiddenSceneIndices) {
      juce::String name = (idx >= 0 && idx < engine.getNumScenes()) ? engine.getSceneName(idx) : juce::String();
      juce::File f(idx < sceneSetupFilePaths.size() ? sceneSetupFilePaths[idx] : juce::String());
      if (f.existsAsFile())
        name = f.getFileNameWithoutExtension();
      m.addItem(idx + 1, juce::String(idx + 1) + "  " + name);
    }
    juce::Component::SafePointer<MainComponent> safeThis(this);
    m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&sceneOverflowBtn),
                    [safeThis](int result) {
      if (safeThis != nullptr && result > 0)
        safeThis->recallScene(result - 1);
    });
  };

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

