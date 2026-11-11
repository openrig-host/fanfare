#pragma once

#include <JuceHeader.h>
#include <array>
#include <cmath>
#include <functional>
#include <vector>
#include "../../FanfareConstants.h"
#include "../../MidiLearnBus.h"
#include "../../RackSlot.h"
#include "../../ThemeManager.h"
#include "../FanfareSynthProcessorImpl.h"
#include "../SynthConstants.h"
#include "../SynthParams.h"
#include "OnscreenKeyboard.h"
#include "SynthControlCallbacks.h"
#include "SynthHeaderBar.h"
#include "SynthLayoutSpec.h"
#include "SynthSectionComponent.h"
#include "SynthSkin.h"
#include "../../SynthMidiSetupDialog.h"

class FanfareSynthComponent : public juce::Component,
                              private FanfareSynth::Ui::SynthTelemetrySink,
                              private juce::Timer {
public:
  static juce::Rectangle<int>
  computeOverlayBoundsForHost(juce::Rectangle<int> hostBounds) noexcept {
    using namespace FanfareSynth::UiMetrics;
    const int availW = juce::jmax(320, hostBounds.getWidth() - kOverlayMarginFromHost);
    const int availH = juce::jmax(280, hostBounds.getHeight() - kOverlayMarginFromHost);
    const int targetW =
        juce::jmin(availW, juce::jlimit(kMinOverlayWidth, kMaxOverlayWidth,
                                        (int)(hostBounds.getWidth() * 0.90f)));
    const int targetH =
        juce::jmin(availH, juce::jlimit(kMinOverlayHeight, kMaxOverlayHeight,
                                        (int)(hostBounds.getHeight() * 0.84f)));
    return hostBounds.withSizeKeepingCentre(targetW, targetH);
  }

  void updateForHostBounds(juce::Rectangle<int> hostBounds) {
    using namespace FanfareSynth::UiMetrics;
    if (userDraggedCorner && getWidth() > 0 && getHeight() > 0) {
      const int maxW = juce::jmax(320, hostBounds.getWidth() - 16);
      const int maxH = juce::jmax(280, hostBounds.getHeight() - 16);
      const int clampedW = juce::jmin(getWidth(), maxW);
      const int clampedH = juce::jmin(getHeight(), maxH);
      applyingHostBounds = true;
      setBounds(hostBounds.withSizeKeepingCentre(clampedW, clampedH));
      applyingHostBounds = false;
      return;
    }
    applyingHostBounds = true;
    setBounds(computeOverlayBoundsForHost(hostBounds));
    applyingHostBounds = false;
  }

  FanfareSynthComponent(RackSlot &slot, std::function<void()> onClose,
                        int chainIdx = 0)
      : slotRef(slot), chainIndex(juce::jlimit(0, 2, chainIdx)),
        synth(slot.getSynth(chainIndex)), closeCallback(std::move(onClose)),
        header(synth), keyboard(synth) {
    using namespace FanfareSynth;

    header.setChainSlotIndex(chainIndex);
    setWantsKeyboardFocus(true);
    addAndMakeVisible(header);

    header.onPowerToggled = [this](bool) {
      updateBypassVisualState();
      refreshLcdDisplay();
    };
    header.lcd.onOpenPresetMenu = [this] { showPresetBrowserMenu(); };
    header.prevBtn.onClick = [this] { stepPreset(-1); };
    header.nextBtn.onClick = [this] { stepPreset(1); };
    header.compareBtn.onClick = [this] { toggleCompareMode(); };
    header.initBtn.onClick = [this] { loadInitPatch(); };
    header.saveBtn.onClick = [this] { promptSaveUserPreset(); };
    header.delBtn.onClick = [this] { promptDeleteUserPreset(); };
    header.keysBtn.onClick = [this] {
      showKeyboard = header.keysBtn.getToggleState();
      keyboard.setKeyboardEnabled(showKeyboard);
      keyboard.setVisible(showKeyboard);
      resized();
      repaint();
    };
    header.onOpenMidiDialog = [this] {
      showMidiRoutingDialog();
    };
    header.closeBtn.onClick = [this] {
      if (closeCallback)
        closeCallback();
    };

    buildSectionsFromSpec();
    addAndMakeVisible(keyboard);
    keyboard.setVisible(false);

    resizeLimits.setSizeLimits(UiMetrics::kMinOverlayWidth,
                               UiMetrics::kMinOverlayHeight,
                               UiMetrics::kMaxOverlayWidth,
                               UiMetrics::kMaxOverlayHeight);
    resizer = std::make_unique<juce::ResizableCornerComponent>(this, &resizeLimits);
    addAndMakeVisible(*resizer);

    refreshPresetCatalog();
    captureCompareBaselineFromCurrent();
    syncAllControlsFromProcessor();
    updateBypassVisualState();
    refreshLcdDisplay();

    startTimerHz(UiMetrics::kTelemetryTimerHz);
  }

  std::function<void()> onOpenNoteRange;
  std::function<void()> onOpenCCDialog;
  std::function<void()> onOpenMidiEffects;

  juce::Rectangle<int> getMidiButtonScreenBounds() const {
    return header.midiBtn.getScreenBounds();
  }

  void showMidiRoutingDialog() {
    auto *dlg = new SynthMidiSetupDialog(
        slotRef, chainIndex, false /*isFm*/,
        [this] { if (onOpenNoteRange) onOpenNoteRange(); },
        [this] { if (onOpenCCDialog) onOpenCCDialog(); },
        [this] { if (onOpenMidiEffects) onOpenMidiEffects(); });
    dlg->setSize(520, 440);
    auto &box = juce::CallOutBox::launchAsynchronously(
        std::unique_ptr<juce::Component>(dlg),
        header.midiBtn.getScreenBounds(),
        this);
    box.setDismissalMouseClicksAreAlwaysConsumed(true);
  }

  ~FanfareSynthComponent() override {
    stopTimer();
    if (compareActive && preCompareWorkingState.isObject())
      synth.applyState(preCompareWorkingState, true);
    keyboard.releaseAllNotes();
    Fanfare::MidiLearnBus::getInstance().disarmIfOwner(this);
  }

  void visibilityChanged() override {
    if (isVisible() && showKeyboard)
      keyboard.grabKeyboardFocus();
  }

  bool keyPressed(const juce::KeyPress &key) override {
    if (!showKeyboard)
      return false;
    return keyboard.handleKeyPressed(key);
  }

  bool keyStateChanged(bool) override {
    if (!showKeyboard)
      return false;
    return keyboard.handleKeyStateChange();
  }

  void paint(juce::Graphics &g) override {
    const auto bounds = getLocalBounds().toFloat();
    g.setColour(ThemeManager::get(Theme::Role::background));
    g.fillRoundedRectangle(bounds, 6.0f);

    backdrop.render(getWidth(), getHeight(),
                    FanfareSynth::Ui::Skin::themeSignature(),
                    [](juce::Graphics &cg, juce::Rectangle<int> r) {
                      cg.drawImageAt(
                          FanfareSynth::Ui::Skin::makeNoise(r.getWidth(),
                                                            r.getHeight()),
                          0, 0);
                      FanfareSynth::Ui::Skin::drawVignette(cg, r.toFloat());
                    });
    g.saveState();
    g.reduceClipRegion(bounds.toNearestInt());
    backdrop.draw(g, 0, 0);
    g.restoreState();

    for (const auto &p : flowMarks) {
      juce::Path ch;
      ch.startNewSubPath(p.x - 2.5f, p.y - 4.0f);
      ch.lineTo(p.x + 2.0f, p.y);
      ch.lineTo(p.x - 2.5f, p.y + 4.0f);
      g.setColour(
          ThemeManager::get(Theme::Role::accentDim).withAlpha(0.80f));
      g.strokePath(ch, juce::PathStrokeType(1.4f,
                                            juce::PathStrokeType::curved,
                                            juce::PathStrokeType::rounded));
    }

    g.setColour(ThemeManager::get(Theme::Role::borderStrong));
    g.drawRoundedRectangle(bounds.reduced(0.5f), 6.0f, 1.2f);
  }

  void resized() override {
    using namespace FanfareSynth::UiMetrics;
    if (!applyingHostBounds && resizer != nullptr && resizer->isMouseButtonDown())
      userDraggedCorner = true;
    auto area = getLocalBounds().reduced(kOuterPadding);

    header.setBounds(area.removeFromTop(kHeaderHeight));
    area.removeFromTop(kSectionGap);

    if (showKeyboard) {
      keyboard.setBounds(area.removeFromBottom(kKeyboardStripHeight));
      area.removeFromBottom(kSectionGap);
    }

    if (controlCheekSection != nullptr) {
      const int cheekW =
          juce::jlimit(148, 188, (int)(area.getWidth() * 0.142f));
      controlCheekSection->setBounds(area.removeFromLeft(cheekW));
      area.removeFromLeft(kSectionGap);
    }

    const int tierGap = kSectionGap;
    const int availH = juce::jmax(40, area.getHeight() - tierGap);
    const int tier1H = (int)std::lround((float)availH * 0.57f);
    auto tier1Rect = area.removeFromTop(tier1H);
    area.removeFromTop(tierGap);
    auto tier2Rect = area;

    layoutSectionRow(tier1Rect, tier1Sections);
    layoutSectionRow(tier2Rect, tier2Sections);

    flowMarks.clear();
    collectFlowMarks(tier1Sections);
    collectFlowMarks(tier2Sections);

    if (resizer != nullptr)
      resizer->setBounds(getWidth() - 16, getHeight() - 16, 15, 15);
  }

private:
  struct PresetEntry {
    bool isUser = false;
    int factoryIndex = -1;
    juce::File userFile;
    juce::String category;
    juce::String name;
  };

  void buildSectionsFromSpec() {
    for (const auto &spec : FanfareSynth::Ui::getJunoLayoutSpecs()) {
      auto *sec =
          new FanfareSynth::Ui::SynthSectionComponent(spec, synth, *this);
      addAndMakeVisible(sec);
      ownedSections.add(sec);
      if (spec.tier == 0)
        controlCheekSection = sec;
      else if (spec.tier == 1)
        tier1Sections.push_back(sec);
      else
        tier2Sections.push_back(sec);
    }
  }

  void collectFlowMarks(
      const std::vector<FanfareSynth::Ui::SynthSectionComponent *> &row) {
    for (size_t i = 0; i + 1 < row.size(); ++i)
      flowMarks.push_back(
          juce::Point<float>((float)(row[i]->getRight() + row[i + 1]->getX()) *
                                 0.5f,
                             (float)(row[i]->getY() + row[i]->getBottom()) *
                                 0.5f));
  }

  static void layoutSectionRow(
      juce::Rectangle<int> rowArea,
      const std::vector<FanfareSynth::Ui::SynthSectionComponent *> &row) {
    if (row.empty() || rowArea.isEmpty())
      return;

    float totalWeight = 0.0f;
    for (const auto *sec : row)
      totalWeight += juce::jmax(0.1f, sec->getWidthWeight());

    const int gap = FanfareSynth::UiMetrics::kSectionGap;
    const int availW =
        juce::jmax(1, rowArea.getWidth() - (int)(row.size() - 1) * gap);
    int curX = rowArea.getX();

    for (size_t i = 0; i < row.size(); ++i) {
      const int w =
          (i + 1 == row.size())
              ? (rowArea.getRight() - curX)
              : (int)std::lround((float)availW *
                                 (row[i]->getWidthWeight() / totalWeight));
      row[i]->setBounds(curX, rowArea.getY(), juce::jmax(20, w),
                        rowArea.getHeight());
      curX += w + gap;
    }
  }

  void syncAllControlsFromProcessor() {
    for (int i = 0; i < FanfareSynth::kNumParams; ++i)
      cachedUiParams[(size_t)i] = synth.getParam(i);
    for (auto *sec : ownedSections)
      sec->syncAllControls();
    header.syncEnableButton();
  }

  void updateBypassVisualState() {
    const float bodyAlpha = synth.enabled.load() ? 1.0f : 0.80f;
    for (auto *sec : ownedSections)
      sec->setAlpha(bodyAlpha);
  }

  // ===== Telemetry & Context Menu Callbacks ==================================

  void handleParamHovered(int paramIdx) override {
    if (!isDraggingControl) {
      focusedParamIdx = paramIdx;
      transientStatusError.clear();
      refreshLcdDisplay();
    }
  }

  void handleParamTouchState(int paramIdx, bool dragging,
                             bool fineMode) override {
    if (compareActive && dragging)
      exitCompareKeepCurrent();
    focusedParamIdx = paramIdx;
    isDraggingControl = dragging;
    isFineGearActive = dragging && fineMode;
    transientStatusError.clear();
    refreshLcdDisplay();
  }

  void handleParamEdited(int paramIdx) override {
    if (compareActive)
      exitCompareKeepCurrent();
    focusedParamIdx = paramIdx;
    cachedUiParams[(size_t)paramIdx] = synth.getParam(paramIdx);
    for (auto *sec : ownedSections)
      sec->refreshDimmingAndBadges();
    refreshLcdDisplay();
  }

  void showParamContextMenu(int paramIdx, juce::Component *anchor) override {
    using namespace FanfareSynth;
    const auto &d = paramDefs()[(size_t)paramIdx];
    const int currentCC = synth.getCCForParamIndex(paramIdx);
    const int defaultCC = d.defaultCC;
    const bool armedHere =
        Fanfare::MidiLearnBus::getInstance().isArmed() &&
        learningParamIdx == paramIdx;

    juce::PopupMenu menu;
    menu.addSectionHeader(juce::String(d.label) + "  (" +
                          formatParamValue(d, synth.getParam(paramIdx), true) +
                          ")");
    if (d.kind == KindContinuous)
      menu.addItem(1, "Enter Exact Value...");
    menu.addItem(2, "Reset to Default (" + formatParamValue(d, d.def, true) +
                        ")");
    menu.addSeparator();
    menu.addItem(3, armedHere ? "Cancel MIDI Learn" : "MIDI Learn CC...", true,
                 armedHere);
    if (currentCC >= 0)
      menu.addItem(4, "Clear CC Mapping (CC " + juce::String(currentCC) + ")");
    if (defaultCC >= 0 && currentCC != defaultCC)
      menu.addItem(5, "Restore Default CC (CC " + juce::String(defaultCC) + ")");

    juce::Component::SafePointer<FanfareSynthComponent> safeThis(this);
    menu.showMenuAsync(
        juce::PopupMenu::Options().withTargetComponent(anchor),
        [safeThis, paramIdx, defaultCC, armedHere](int result) {
          if (safeThis == nullptr || result == 0)
            return;
          if (result == 1) {
            for (auto *sec : safeThis->ownedSections) {
              if (auto *w = sec->findWidgetForParam(paramIdx)) {
                w->beginInlineValueEdit();
                break;
              }
            }
          } else if (result == 2) {
            const auto &def = FanfareSynth::paramDefs()[(size_t)paramIdx];
            safeThis->synth.setParam(paramIdx, def.def, true);
            safeThis->syncAllControlsFromProcessor();
            safeThis->refreshLcdDisplay();
          } else if (result == 3) {
            if (armedHere) {
              Fanfare::MidiLearnBus::getInstance().disarm();
              safeThis->learningParamIdx = -1;
              safeThis->refreshLcdDisplay();
              return;
            }
            const auto &def = FanfareSynth::paramDefs()[(size_t)paramIdx];
            Fanfare::MidiLearnBus::Target t;
            t.label = "Synth: " + juce::String(def.label);
            t.targetId = "synth." + juce::String(def.id);
            t.onBind = [safeThis, paramIdx](int cc) {
              if (safeThis == nullptr)
                return;
              safeThis->synth.setCCForParamIndex(paramIdx, cc);
              safeThis->learningParamIdx = -1;
              safeThis->syncAllControlsFromProcessor();
              safeThis->refreshLcdDisplay();
            };
            safeThis->learningParamIdx = paramIdx;
            Fanfare::MidiLearnBus::getInstance().arm(t, safeThis.getComponent());
            safeThis->refreshLcdDisplay();
          } else if (result == 4) {
            safeThis->synth.setCCForParamIndex(paramIdx, -1);
            safeThis->syncAllControlsFromProcessor();
            safeThis->refreshLcdDisplay();
          } else if (result == 5) {
            safeThis->synth.setCCForParamIndex(paramIdx, defaultCC);
            safeThis->syncAllControlsFromProcessor();
            safeThis->refreshLcdDisplay();
          }
        });
  }

  // ===== Live Audio->UI Telemetry Timer ======================================

  void timerCallback() override {
    header.updateMeterTelemetry(synth.getOutputPeakL(), synth.getOutputPeakR(),
                                synth.getActiveVoiceCount());

    bool externalChange = false;
    for (int i = 0; i < FanfareSynth::kNumParams; ++i) {
      if (isDraggingControl && i == focusedParamIdx)
        continue;
      const float live = synth.getParam(i);
      if (std::abs(live - cachedUiParams[(size_t)i]) > 1e-5f) {
        cachedUiParams[(size_t)i] = live;
        externalChange = true;
      }
    }

    if (externalChange) {
      for (auto *sec : ownedSections)
        sec->syncAllControls();
      refreshLcdDisplay();
    } else if (learningParamIdx >= 0 &&
               !Fanfare::MidiLearnBus::getInstance().isArmed()) {
      learningParamIdx = -1;
      refreshLcdDisplay();
    }
  }

  void refreshLcdDisplay() {
    using namespace FanfareSynth;
    const juce::String curName = synth.getCurrentPresetName();
    const int matchedIdx = findPresetIndexByName(curName);
    const bool isUser =
        (matchedIdx >= 0 && presetList[(size_t)matchedIdx].isUser);

    header.lcd.setPatchDisplay(matchedIdx >= 0 ? (matchedIdx + 1) : 0, curName,
                               synth.isPresetDirty(), isUser);
    header.delBtn.setEnabled(isUser);
    header.compareBtn.setEnabled(synth.isPresetDirty() || compareActive);

    if (transientStatusError.isNotEmpty()) {
      header.lcd.setTelemetryText(transientStatusError, false, true, false);
      return;
    }

    if (learningParamIdx >= 0 &&
        Fanfare::MidiLearnBus::getInstance().isArmed()) {
      const auto &d = paramDefs()[(size_t)learningParamIdx];
      header.lcd.setTelemetryText(
          "MIDI LEARN: Move CC for " + juce::String(d.label), false, false,
          true);
      return;
    }

    if (compareActive) {
      header.lcd.setTelemetryText("COMPARE: Auditioning saved patch", false,
                                  false, true);
      return;
    }

    if (focusedParamIdx >= 0 && focusedParamIdx < kNumParams &&
        (isDraggingControl || synth.enabled.load())) {
      const auto &d = paramDefs()[(size_t)focusedParamIdx];
      juce::String tele =
          juce::String(d.label) + ": " +
          formatParamValue(d, synth.getParam(focusedParamIdx), true);
      const int cc = synth.getCCForParamIndex(focusedParamIdx);
      if (cc >= 0)
        tele += "  [CC " + juce::String(cc) + "]";
      header.lcd.setTelemetryText(tele, isFineGearActive, false, false);
      return;
    }

    if (!synth.enabled.load()) {
      header.lcd.setTelemetryText("SYNTH OFF - Click [SYNTH: OFF] to enable",
                                  false, false, true);
      return;
    }

    const auto &cutoffDef = paramDefs()[ParamId::cutoff];
    header.lcd.setTelemetryText(
        "VCF Cutoff: " +
            formatParamValue(cutoffDef, synth.getParam(ParamId::cutoff), true),
        false, false, false);
  }

  // ===== Presets, Compare, Save, Delete ======================================

  void refreshPresetCatalog() {
    presetList.clear();
    const auto &factory = FanfareSynthProcessor::factoryPresets();
    for (int i = 0; i < factory.size(); ++i) {
      PresetEntry e;
      e.isUser = false;
      e.factoryIndex = i;
      e.category = factory[i].getProperty("cat", "Patches").toString();
      e.name = factory[i].getProperty("name", "Patch").toString();
      presetList.push_back(e);
    }

    userPresetDir = FanfareConstants::getAppDirectory().getChildFile(
        FanfareSynth::StateSchema::kUserPresetsSubdir);
    if (userPresetDir.isDirectory()) {
      auto files = userPresetDir.findChildFiles(juce::File::findFiles, false,
                                                "*.json");
      files.sort();
      for (const auto &f : files) {
        PresetEntry e;
        e.isUser = true;
        e.userFile = f;
        e.category = "User Presets";
        e.name = f.getFileNameWithoutExtension();
        presetList.push_back(e);
      }
    }
  }

  int findPresetIndexByName(const juce::String &name) const {
    for (size_t i = 0; i < presetList.size(); ++i)
      if (presetList[i].isUser && presetList[i].name.equalsIgnoreCase(name))
        return (int)i;
    for (size_t i = 0; i < presetList.size(); ++i)
      if (!presetList[i].isUser && presetList[i].name.equalsIgnoreCase(name))
        return (int)i;
    return -1;
  }

  void showPresetBrowserMenu() {
    refreshPresetCatalog();
    juce::PopupMenu mainMenu;
    const juce::String activeName = synth.getCurrentPresetName();

    juce::String lastCat;
    juce::PopupMenu subMenu;
    bool hasSubItems = false;
    bool anyUserPresets = false;

    auto flushCategory = [&]() {
      if (hasSubItems) {
        mainMenu.addSubMenu(lastCat, subMenu);
        subMenu.clear();
        hasSubItems = false;
      }
    };

    for (size_t i = 0; i < presetList.size(); ++i) {
      const auto &e = presetList[i];
      if (e.isUser)
        anyUserPresets = true;

      if (e.category != lastCat) {
        flushCategory();
        lastCat = e.category;
      }
      const bool ticked = e.name.equalsIgnoreCase(activeName);
      if (e.category == "Init") {
        mainMenu.addItem((int)i + 1, e.name, true, ticked);
        mainMenu.addSeparator();
        lastCat.clear();
      } else {
        subMenu.addItem((int)i + 1, e.name, true, ticked);
        hasSubItems = true;
      }
    }
    flushCategory();

    if (!anyUserPresets) {
      juce::PopupMenu emptyUserMenu;
      emptyUserMenu.addItem(99999, "No user presets saved yet (click SAVE)",
                            false, false);
      mainMenu.addSeparator();
      mainMenu.addSubMenu("User Presets", emptyUserMenu);
    }

    juce::Component::SafePointer<FanfareSynthComponent> safeThis(this);
    mainMenu.showMenuAsync(
        juce::PopupMenu::Options().withTargetComponent(&header.lcd),
        [safeThis](int result) {
          if (safeThis == nullptr || result <= 0 ||
              result > (int)safeThis->presetList.size())
            return;
          safeThis->loadPresetAtIndex(result - 1);
        });
  }

  void loadPresetAtIndex(int idx) {
    if (idx < 0 || idx >= (int)presetList.size())
      return;

    exitCompareDiscardSnapshot();
    transientStatusError.clear();

    const auto &entry = presetList[(size_t)idx];
    if (entry.isUser) {
      const auto parsed = juce::JSON::parse(entry.userFile);
      if (!parsed.isObject()) {
        transientStatusError =
            "ERROR: Preset '" + entry.name + "' is corrupt";
        refreshLcdDisplay();
        return;
      }
      const bool wasEnabled = synth.enabled.load();
      synth.applyState(parsed, true);
      synth.enabled.store(wasEnabled);
      synth.setPresetMetadata(entry.name, false);
    } else {
      synth.applyPreset(
          FanfareSynthProcessor::factoryPresets()[entry.factoryIndex]);
    }

    captureCompareBaselineFromCurrent();
    syncAllControlsFromProcessor();
    refreshLcdDisplay();
  }

  void stepPreset(int delta) {
    refreshPresetCatalog();
    if (presetList.empty())
      return;
    const int n = (int)presetList.size();
    int cur = findPresetIndexByName(synth.getCurrentPresetName());
    if (cur < 0)
      cur = 0;
    const int next = (cur + delta + n) % n;
    loadPresetAtIndex(next);
  }

  void loadInitPatch() {
    exitCompareDiscardSnapshot();
    transientStatusError.clear();
    synth.applyDefaults();
    synth.setPresetMetadata(
        FanfareSynth::StateSchema::kDefaultInitPresetName, false);
    captureCompareBaselineFromCurrent();
    syncAllControlsFromProcessor();
    refreshLcdDisplay();
  }

  void captureCompareBaselineFromCurrent() {
    const int idx = findPresetIndexByName(synth.getCurrentPresetName());
    if (idx >= 0 && !synth.isPresetDirty()) {
      compareBaselineState = synth.getState();
    } else if (idx >= 0) {
      FanfareSynthProcessor temp;
      const auto &entry = presetList[(size_t)idx];
      if (entry.isUser) {
        const auto parsed = juce::JSON::parse(entry.userFile);
        if (parsed.isObject())
          temp.applyState(parsed, true);
      } else {
        temp.applyPreset(
            FanfareSynthProcessor::factoryPresets()[entry.factoryIndex]);
      }
      compareBaselineState = temp.getState();
    } else {
      compareBaselineState = synth.getState();
    }
  }

  void toggleCompareMode() {
    const bool wantCompare = header.compareBtn.getToggleState();
    if (wantCompare && compareBaselineState.isObject()) {
      preCompareWorkingState = synth.getState();
      const bool wasEnabled = synth.enabled.load();
      synth.applyState(compareBaselineState, true);
      synth.enabled.store(wasEnabled);
      compareActive = true;
    } else {
      if (compareActive && preCompareWorkingState.isObject()) {
        const bool wasEnabled = synth.enabled.load();
        synth.applyState(preCompareWorkingState, true);
        synth.enabled.store(wasEnabled);
      }
      compareActive = false;
      header.compareBtn.setToggleState(false, juce::dontSendNotification);
    }
    syncAllControlsFromProcessor();
    refreshLcdDisplay();
  }

  void exitCompareKeepCurrent() {
    compareActive = false;
    preCompareWorkingState = juce::var();
    header.compareBtn.setToggleState(false, juce::dontSendNotification);
  }

  void exitCompareDiscardSnapshot() {
    compareActive = false;
    preCompareWorkingState = juce::var();
    header.compareBtn.setToggleState(false, juce::dontSendNotification);
  }

  void promptSaveUserPreset() {
    if (compareActive)
      toggleCompareMode();

    juce::Component::SafePointer<FanfareSynthComponent> safeThis(this);
    auto *w = new juce::AlertWindow(
        "Save Synth Preset", "Enter a name for this user preset:",
        juce::MessageBoxIconType::NoIcon, this);
    const juce::String suggested = synth.getCurrentPresetName();
    w->addTextEditor("name", suggested.isNotEmpty() ? suggested : "My Patch");
    w->addButton("Save", 1, juce::KeyPress(juce::KeyPress::returnKey));
    w->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
    w->enterModalState(
        true,
        juce::ModalCallbackFunction::create([safeThis, w](int result) {
          const juce::String rawName =
              w->getTextEditorContents("name").trim();
          delete w;
          if (result != 1 || rawName.isEmpty() || safeThis == nullptr)
            return;

          auto dir = FanfareConstants::getAppDirectory().getChildFile(
              FanfareSynth::StateSchema::kUserPresetsSubdir);
          if (!dir.exists() && !dir.createDirectory()) {
            safeThis->transientStatusError =
                "ERROR: Could not create preset folder";
            safeThis->refreshLcdDisplay();
            return;
          }

          safeThis->synth.setPresetMetadata(rawName, false);
          auto state = safeThis->synth.getState();
          auto file = dir.getChildFile(
              juce::File::createLegalFileName(rawName) + ".json");
          if (!file.replaceWithText(juce::JSON::toString(state))) {
            safeThis->transientStatusError = "ERROR: Failed writing preset file";
            safeThis->refreshLcdDisplay();
            return;
          }

          safeThis->transientStatusError.clear();
          safeThis->refreshPresetCatalog();
          safeThis->captureCompareBaselineFromCurrent();
          safeThis->refreshLcdDisplay();
        }));
  }

  void promptDeleteUserPreset() {
    const int idx = findPresetIndexByName(synth.getCurrentPresetName());
    if (idx < 0 || !presetList[(size_t)idx].isUser)
      return;

    const auto fileToDelete = presetList[(size_t)idx].userFile;
    const juce::String delName = presetList[(size_t)idx].name;

    juce::Component::SafePointer<FanfareSynthComponent> safeThis(this);
    auto *w = new juce::AlertWindow(
        "Delete User Preset",
        "Delete user preset '" + delName + "'?",
        juce::MessageBoxIconType::WarningIcon, this);
    w->addButton("Delete", 1, juce::KeyPress(juce::KeyPress::returnKey));
    w->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
    w->enterModalState(
        true,
        juce::ModalCallbackFunction::create([safeThis, w, fileToDelete](int res) {
          delete w;
          if (res != 1 || safeThis == nullptr)
            return;
          fileToDelete.deleteFile();
          safeThis->refreshPresetCatalog();
          safeThis->synth.setPresetDirty(true);
          safeThis->refreshLcdDisplay();
        }));
  }

  RackSlot &slotRef;
  int chainIndex = 0;
  FanfareSynthProcessor &synth;
  std::function<void()> closeCallback;

  FanfareSynth::Ui::SynthHeaderBar header;
  FanfareSynth::Ui::OnscreenKeyboard keyboard;
  juce::ComponentBoundsConstrainer resizeLimits;
  std::unique_ptr<juce::ResizableCornerComponent> resizer;

  juce::OwnedArray<FanfareSynth::Ui::SynthSectionComponent> ownedSections;
  FanfareSynth::Ui::SynthSectionComponent *controlCheekSection = nullptr;
  std::vector<FanfareSynth::Ui::SynthSectionComponent *> tier1Sections;
  std::vector<FanfareSynth::Ui::SynthSectionComponent *> tier2Sections;
  std::vector<juce::Point<float>> flowMarks;
  FanfareSynth::Ui::Skin::ChromeCache backdrop;

  std::array<float, FanfareSynth::kNumParams> cachedUiParams{};
  std::vector<PresetEntry> presetList;
  juce::File userPresetDir;

  juce::var compareBaselineState;
  juce::var preCompareWorkingState;
  juce::String transientStatusError;

  int focusedParamIdx = FanfareSynth::ParamId::cutoff;
  int learningParamIdx = -1;
  bool isDraggingControl = false;
  bool isFineGearActive = false;
  bool compareActive = false;
  bool showKeyboard = false;
  bool applyingHostBounds = false;
  bool userDraggedCorner = false;

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(FanfareSynthComponent)
};
