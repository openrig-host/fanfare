#// MainComponent_Windows.cpp - part of MainComponent (mechanical split, see nov8mistral.md)
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

void MainComponent::showMasterPluginMenu(bool isFoh, int chainIndex) {
  juce::PopupMenu menu;
  menu.addSectionHeader("Select Master " + juce::String(isFoh ? "FOH" : "IEM") + " Plugin " + juce::String(chainIndex + 1));
  for (int p = 0; p < engine.getNumAvailablePlugins(); ++p) {
    menu.addItem(p + 1, engine.getAvailablePluginName(p));
  }
  menu.addSeparator();
  menu.addItem(-1, "(No Plugin)");

  juce::Component::SafePointer<MainComponent> safeThis(this);
  menu.showMenuAsync(juce::PopupMenu::Options(), [safeThis, isFoh, chainIndex](int result) {
    if (safeThis == nullptr)
      return;
    if (result > 0 || result == -1) {
      if (auto *oldInstance = safeThis->engine.getMasterPluginInstance(isFoh, chainIndex)) {
        safeThis->closePluginWindowForInstance(oldInstance);
      }
    }
    if (result > 0) {
      safeThis->loadingOverlay.reset(new LoadingOverlay());
      safeThis->addAndMakeVisible(safeThis->loadingOverlay.get());

      safeThis->engine.loadPluginIntoMasterBus(isFoh, chainIndex, result - 1,
          [safeThis, isFoh, chainIndex](bool success, const juce::String &err) {
            if (safeThis == nullptr)
              return;
            safeThis->loadingOverlay.reset();
            if (!success) {
              juce::AlertWindow::showMessageBoxAsync(
                  juce::MessageBoxIconType::WarningIcon,
                  "Plugin Load Failed", err);
            } else {
              auto name = safeThis->engine.getMasterPluginName(isFoh, chainIndex);
              auto &btn = isFoh ? safeThis->fohFxBtns[chainIndex] : safeThis->iemFxBtns[chainIndex];
              bool hasPlugin = !name.isEmpty();
              btn.getProperties().set("isPluginSlot", true);
              btn.getProperties().set("hasPlugin", hasPlugin);
              btn.setButtonText(hasPlugin ? name : ((isFoh ? "FOH " : "IEM ") + juce::String(chainIndex + 1)));
            }
          });
    } else if (result == -1) {
      safeThis->engine.loadPluginIntoMasterBus(isFoh, chainIndex, -1, [safeThis, isFoh, chainIndex](bool, const juce::String &) {
        if (safeThis == nullptr)
          return;
        auto &btn = isFoh ? safeThis->fohFxBtns[chainIndex] : safeThis->iemFxBtns[chainIndex];
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

