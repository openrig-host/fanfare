#pragma once

#include <JuceHeader.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>
#include "../../../RackSlot.h"
#include "../../../ThemeManager.h"
#include "../FmSynthProcessor.h"
#include "FmControls.h"
#include "FmHeaderBar.h"
#include "FmKeyboardStrip.h"
#include "FmModeBar.h"
#include "FmOperatorCard.h"
#include "FmRightDeck.h"
#include "FmUiMetrics.h"
#include "../../../SynthMidiSetupDialog.h"

class FmSynthComponent : public juce::Component,
                         public FanfareFm::Ui::FmTelemetrySink,
                         private juce::Timer {
public:
  static juce::Rectangle<int>
  computeOverlayBoundsForHost(juce::Rectangle<int> host) noexcept {
    using namespace FanfareFm::Ui::Metrics;
    const int availW = juce::jmax(320, host.getWidth() - kOverlayMarginFromHost);
    const int availH = juce::jmax(280, host.getHeight() - kOverlayMarginFromHost);
    const int targetW = juce::jmin(
        availW, juce::jlimit(kMinOverlayWidth, kMaxOverlayWidth,
                             static_cast<int>(host.getWidth() * 0.92f)));
    const int targetH = juce::jmin(
        availH, juce::jlimit(kMinOverlayHeight, kMaxOverlayHeight,
                             static_cast<int>(host.getHeight() * 0.86f)));
    return host.withSizeKeepingCentre(targetW, targetH);
  }

  void updateForHostBounds(juce::Rectangle<int> host) {
    if (userDraggedCorner && getWidth() > 0 && getHeight() > 0) {
      const int maxW = juce::jmax(320, host.getWidth() - 16);
      const int maxH = juce::jmax(280, host.getHeight() - 16);
      applyingHostBounds = true;
      setBounds(host.withSizeKeepingCentre(juce::jmin(getWidth(), maxW),
                                           juce::jmin(getHeight(), maxH)));
      applyingHostBounds = false;
      return;
    }
    applyingHostBounds = true;
    setBounds(computeOverlayBoundsForHost(host));
    applyingHostBounds = false;
  }

  FmSynthComponent(RackSlot &slot, std::function<void()> onClose,
                   int chainIdx = 0)
      : slotRef(slot), chainIndex(juce::jlimit(0, 2, chainIdx)),
        proc(slot.getFmSynth(chainIndex)), closeCallback(std::move(onClose)),
        header(proc), rightDeck(proc, *this, [this] { commitPatchFromUi(); }),
        keyboard(proc) {
    using namespace FanfareFm::Ui;

    setWantsKeyboardFocus(true);
    header.setChainSlotIndex(chainIndex);
    addAndMakeVisible(header);

    header.onPowerToggled = [this](bool) {
      updateBypassVisualState();
      refreshLcdDisplay();
    };
    header.lcd.onOpenPresetMenu = [this] { showPresetBrowserMenu(); };
    header.prevBtn.onClick = [this] { stepPreset(-1); };
    header.nextBtn.onClick = [this] { stepPreset(1); };
    header.syxBtn.onClick = [this] { showSysexMenu(); };
    header.compareBtn.onClick = [this] { toggleCompareMode(); };
    header.initBtn.onClick = [this] { loadInitVoice(); };
    header.saveBtn.onClick = [this] { promptSavePreset(); };
    header.delBtn.onClick = [this] { promptDeletePreset(); };
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

    addAndMakeVisible(modeBar);
    modeBar.onModeSelected = [this](int modeIdx) {
      if (compareActive)
        exitCompareKeepCurrent();
      proc.setMode(modeIdx);
      focusedTelemetry.clear();
      transientStatus.clear();
      transientError = false;
      syncAllControlsFromProcessor();
      refreshLcdDisplay();
    };

    for (int d = 0; d < 6; ++d) {
      operatorCards[(size_t)d] = std::make_unique<FmOperatorCard>(
          proc, d + 1, *this, [this] { commitPatchFromUi(); });
      addAndMakeVisible(*operatorCards[(size_t)d]);
    }

    addAndMakeVisible(rightDeck);
    addAndMakeVisible(keyboard);
    keyboard.setVisible(false);

    resizeLimits.setSizeLimits(Metrics::kMinOverlayWidth,
                               Metrics::kMinOverlayHeight,
                               Metrics::kMaxOverlayWidth,
                               Metrics::kMaxOverlayHeight);
    resizer =
        std::make_unique<juce::ResizableCornerComponent>(this, &resizeLimits);
    addAndMakeVisible(*resizer);

    proc.refreshPresetCatalog();
    captureCompareBaseline();
    syncAllControlsFromProcessor();
    updateBypassVisualState();
    refreshLcdDisplay();

    startTimerHz(Metrics::kTelemetryTimerHz);
  }

  std::function<void()> onOpenNoteRange;
  std::function<void()> onOpenCCDialog;
  std::function<void()> onOpenMidiEffects;

  juce::Rectangle<int> getMidiButtonScreenBounds() const {
    return header.midiBtn.getScreenBounds();
  }

  void showMidiRoutingDialog() {
    auto *dlg = new SynthMidiSetupDialog(
        slotRef, chainIndex, true /*isFm*/,
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

  ~FmSynthComponent() override {
    stopTimer();
    if (compareActive && preCompareState.isObject())
      proc.applyState(preCompareState, true);
    keyboard.releaseAllNotes();
  }

  void visibilityChanged() override {
    if (isVisible() && showKeyboard)
      keyboard.grabKeyboardFocus();
  }

  bool keyPressed(const juce::KeyPress &key) override {
    return showKeyboard ? keyboard.handleKeyPressed(key) : false;
  }

  bool keyStateChanged(bool) override {
    return showKeyboard ? keyboard.handleKeyStateChange() : false;
  }

  void paint(juce::Graphics &g) override {
    const auto bounds = getLocalBounds().toFloat();
    g.setColour(ThemeManager::get(Theme::Role::background));
    g.fillRoundedRectangle(bounds, 6.0f);

    g.setColour(ThemeManager::get(Theme::Role::borderStrong));
    g.drawRoundedRectangle(bounds.reduced(0.5f), 6.0f, 1.2f);
  }

  void resized() override {
    using namespace FanfareFm::Ui::Metrics;
    if (!applyingHostBounds && resizer != nullptr && resizer->isMouseButtonDown())
      userDraggedCorner = true;

    auto area = getLocalBounds().reduced(kOuterPadding);

    header.setBounds(area.removeFromTop(kHeaderHeight));
    area.removeFromTop(kSectionGap);

    modeBar.setBounds(area.removeFromTop(kModeBarHeight));
    area.removeFromTop(kSectionGap);

    if (showKeyboard) {
      keyboard.setBounds(area.removeFromBottom(kKeyboardStripHeight));
      area.removeFromBottom(kSectionGap);
    }

    const int rightW = juce::jlimit(
        kRightDeckMinWidth, kRightDeckMaxWidth,
        static_cast<int>(area.getWidth() * kRightDeckWidthRatio));
    rightDeck.setBounds(area.removeFromRight(rightW));
    area.removeFromRight(kSectionGap);

    const int opGap = 4;
    const int availOpW = juce::jmax(60, area.getWidth() - 5 * opGap);
    int curX = area.getX();
    for (int i = 0; i < 6; ++i) {
      const int w = (i == 5) ? (area.getRight() - curX) : (availOpW / 6);
      operatorCards[(size_t)i]->setBounds(curX, area.getY(), w, area.getHeight());
      curX += w + opGap;
    }

    if (resizer != nullptr)
      resizer->setBounds(getWidth() - 16, getHeight() - 16, 15, 15);
  }

private:
  void commitPatchFromUi() {
    if (compareActive)
      exitCompareKeepCurrent();
    proc.engine().setPatch(proc.engine().patch());
    proc.markDirty();
    rightDeck.syncFromProcessor();
    for (auto &op : operatorCards)
      if (op && !op->anyDragging())
        op->syncFromProcessor();
    lastSyncedHash = computeEngineSnapshotHash();
    refreshLcdDisplay();
  }

  void handleTelemetryHover(const juce::String &text) override {
    if (!isControlDragging) {
      focusedTelemetry = text;
      transientStatus.clear();
      transientError = false;
      refreshLcdDisplay();
    }
  }

  void handleTelemetryTouch(const juce::String &text, bool dragging,
                            bool fineGear) override {
    if (compareActive && dragging)
      exitCompareKeepCurrent();
    focusedTelemetry = text;
    isControlDragging = dragging;
    isFineGear = dragging && fineGear;
    transientStatus.clear();
    transientError = false;
    refreshLcdDisplay();
  }

  void handleTelemetryEdited(const juce::String &text) override {
    if (compareActive)
      exitCompareKeepCurrent();
    focusedTelemetry = text;
    transientStatus.clear();
    transientError = false;
    refreshLcdDisplay();
  }

  bool anyWidgetDragging() const noexcept {
    if (isControlDragging || rightDeck.anyDragging())
      return true;
    for (const auto &op : operatorCards)
      if (op && op->anyDragging())
        return true;
    return false;
  }

  uint64_t computeEngineSnapshotHash() const noexcept {
    uint64_t h = 14695981039346656037ULL;
    const auto &p = proc.engine().patch();
    for (int i = 0; i < FanfareFm::kPatchSize; ++i) {
      h ^= static_cast<uint64_t>(p.d[i]);
      h *= 1099511628211ULL;
    }
    for (int i = 0; i < FmSynthProcessor::kNumFmFx; ++i) {
      const uint64_t fxQ =
          static_cast<uint64_t>(std::lround(proc.getFx(i) * 1000.0f));
      h ^= fxQ + static_cast<uint64_t>(i * 131);
      h *= 1099511628211ULL;
    }
    const uint64_t volQ =
        static_cast<uint64_t>(std::lround(proc.getVolume() * 1000.0f));
    h ^= (volQ + static_cast<uint64_t>(proc.getMode() << 16));
    h *= 1099511628211ULL;
    return h;
  }

  void syncAllControlsFromProcessor() {
    modeBar.setSelectedMode(proc.getMode());
    for (auto &op : operatorCards)
      if (op)
        op->syncFromProcessor();
    rightDeck.syncFromProcessor();
    header.syncEnableButton();
    lastSyncedHash = computeEngineSnapshotHash();
  }

  void updateBypassVisualState() {
    const float bodyAlpha = proc.enabled.load() ? 1.0f : 0.78f;
    modeBar.setAlpha(bodyAlpha);
    rightDeck.setAlpha(bodyAlpha);
    for (auto &op : operatorCards)
      if (op)
        op->setAlpha(bodyAlpha);
  }

  juce::String formatActiveFxSummary() const {
    juce::StringArray active;
    if (proc.getFx(FmSynthProcessor::fxChoMix) >= 0.02f) {
      const int mode =
          static_cast<int>(std::lround(proc.getFx(FmSynthProcessor::fxChoMode)));
      active.add(mode <= 0 ? "CHO I" : (mode == 1 ? "CHO II" : "CHO I+II"));
    }
    if (proc.getFx(FmSynthProcessor::fxDlyMix) >= 0.02f)
      active.add("DLY");
    if (proc.getFx(FmSynthProcessor::fxRevMix) >= 0.02f) {
      const int revType =
          static_cast<int>(std::lround(proc.getFx(FmSynthProcessor::fxRevType)));
      active.add(revType <= 0 ? "ROOM" : (revType == 1 ? "PLATE" : "HALL"));
    }
    return active.isEmpty() ? juce::String("DRY") : active.joinIntoString(" + ");
  }

  void refreshLcdDisplay() {
    const auto &e = proc.engine();
    const juce::String curName = proc.getCurrentPresetName();
    const int curIdx = proc.getCurrentPresetIndex();
    const int displayNum = curIdx >= 0 ? (curIdx + 1) : 0;
    const juce::String srcTag = proc.getCurrentPresetProvenance();

    header.lcd.setPatchDisplay(displayNum, FanfareFm::fmModeName(e.mode()),
                               curName, proc.isPresetDirty(), srcTag);

    if (!proc.enabled.load()) {
      header.lcd.setTelemetryText("BYPASSED - click SYNTH: OFF to enable", false,
                                  false, true);
      return;
    }

    if (compareActive) {
      header.lcd.setTelemetryText("COMPARE: AUDITIONING UNEDITED PATCH", false,
                                  false, true);
      return;
    }

    if (transientStatus.isNotEmpty()) {
      header.lcd.setTelemetryText(transientStatus, false, transientError, false);
      return;
    }

    if (focusedTelemetry.isNotEmpty()) {
      header.lcd.setTelemetryText(focusedTelemetry, isFineGear, false, false);
      return;
    }

    const juce::String catName = proc.getCurrentPresetCategory();
    const juce::String algPart =
        "ALG " + juce::String(e.effectiveAlg() + 1).paddedLeft('0', 2);
    const juce::String summary =
        (catName.isNotEmpty() ? (catName + "  |  ") : juce::String()) +
        algPart + "  |  " + formatActiveFxSummary();
    header.lcd.setTelemetryText(summary, false, false, false);
  }

  void captureCompareBaseline() {
    compareBaselineState = proc.getState();
    compareActive = false;
    header.compareBtn.setToggleState(false, juce::dontSendNotification);
  }

  void exitCompareKeepCurrent() {
    compareActive = false;
    header.compareBtn.setToggleState(false, juce::dontSendNotification);
  }

  void toggleCompareMode() {
    if (!compareActive) {
      if (!compareBaselineState.isObject())
        return;
      preCompareState = proc.getState();
      proc.applyState(compareBaselineState, true);
      compareActive = true;
      header.compareBtn.setToggleState(true, juce::dontSendNotification);
    } else {
      if (preCompareState.isObject())
        proc.applyState(preCompareState, true);
      compareActive = false;
      header.compareBtn.setToggleState(false, juce::dontSendNotification);
    }
    syncAllControlsFromProcessor();
    refreshLcdDisplay();
  }

  void loadInitVoice() {
    if (compareActive)
      exitCompareKeepCurrent();
    FanfareFm::FmPatch p;
    FanfareFm::initPatch(p);
    proc.engine().loadPatchRaw(p);
    proc.setFx(FmSynthProcessor::fxChoMix, 0.0f);
    proc.setFx(FmSynthProcessor::fxDlyMix, 0.0f);
    proc.setFx(FmSynthProcessor::fxRevMix, 0.0f);
    proc.setPresetMetadata("INIT VOICE", false, "INIT", "Init", -1);
    captureCompareBaseline();
    transientStatus = "Loaded clean dry Init Voice";
    transientError = false;
    syncAllControlsFromProcessor();
    refreshLcdDisplay();
  }

  void stepPreset(int delta) {
    if (compareActive)
      exitCompareKeepCurrent();
    const auto &cat = proc.getPresetCatalog();
    if (cat.empty())
      return;
    const int total = static_cast<int>(cat.size());
    int idx = proc.getCurrentPresetIndex();
    if (idx < 0 || idx >= total)
      idx = (delta >= 0) ? 0 : (total - 1);
    else
      idx = (idx + delta + total) % total;
    proc.loadPresetAtIndex(idx);
    captureCompareBaseline();
    focusedTelemetry.clear();
    transientStatus.clear();
    syncAllControlsFromProcessor();
    refreshLcdDisplay();
  }

  void showPresetBrowserMenu() {
    proc.refreshPresetCatalog();
    const auto &cat = proc.getPresetCatalog();
    const int activeIndex = proc.getCurrentPresetIndex();
    juce::PopupMenu menu;

    std::vector<juce::String> categoryOrder;
    for (const auto &e : cat) {
      if (e.isUser) {
        categoryOrder.push_back("User Presets");
        break;
      }
    }

    int canonicalCount = 0;
    const char *const *canonicalCats =
        FanfareFm::fmPresetCategoryOrder(canonicalCount);
    for (int c = 0; c < canonicalCount; ++c)
      categoryOrder.push_back(juce::String::fromUTF8(canonicalCats[c]));

    for (const auto &e : cat) {
      if (std::find(categoryOrder.begin(), categoryOrder.end(), e.category) ==
          categoryOrder.end()) {
        categoryOrder.push_back(e.category);
      }
    }

    bool addedUserSep = false;
    for (const auto &catName : categoryOrder) {
      juce::PopupMenu sub;
      bool hasActive = false;
      int itemCount = 0;
      for (size_t i = 0; i < cat.size(); ++i) {
        if (cat[i].category != catName)
          continue;
        ++itemCount;
        const bool isCurrent = (static_cast<int>(i) == activeIndex);
        hasActive = hasActive || isCurrent;

        juce::String itemLabel = cat[i].name;
        if (cat[i].isImported) {
          itemLabel =
              juce::String(itemCount).paddedLeft('0', 2) + ". " + cat[i].name;
        } else if (!cat[i].isUser && cat[i].provenance.isNotEmpty()) {
          itemLabel += "   [" + cat[i].provenance + "]";
        }
        sub.addItem(static_cast<int>(i) + 1, itemLabel, true, isCurrent);
      }
      if (itemCount == 0)
        continue;

      juce::String menuLabel =
          (catName == "Imported Sysex" && activeSysexBankTitle.isNotEmpty())
              ? ("Imported SysEx: " + activeSysexBankTitle)
              : (catName + "  (" + juce::String(itemCount) + ")");
      menu.addSubMenu(menuLabel, sub, true, nullptr, hasActive);

      if (catName == "User Presets" && !addedUserSep) {
        menu.addSeparator();
        addedUserSep = true;
      }
    }

    juce::Component::SafePointer<FmSynthComponent> safeThis(this);
    menu.showMenuAsync(
        juce::PopupMenu::Options().withTargetComponent(&header.lcd),
        [safeThis](int res) {
          if (safeThis == nullptr || res <= 0)
            return;
          const auto &catalog = safeThis->proc.getPresetCatalog();
          const int picked = res - 1;
          if (picked >= 0 && picked < static_cast<int>(catalog.size())) {
            if (safeThis->compareActive)
              safeThis->exitCompareKeepCurrent();
            safeThis->proc.loadPresetAtIndex(picked);
            safeThis->captureCompareBaseline();
            safeThis->focusedTelemetry.clear();
            safeThis->transientStatus.clear();
            safeThis->syncAllControlsFromProcessor();
            safeThis->refreshLcdDisplay();
          }
        });
  }

  void promptSavePreset() {
    juce::Component::SafePointer<FmSynthComponent> safeThis(this);
    auto *w = new juce::AlertWindow("Save CoplanDX Preset", "Preset name:",
                                    juce::MessageBoxIconType::NoIcon, this);
    w->addTextEditor("name", proc.getCurrentPresetName());
    w->addButton("Save", 1, juce::KeyPress(juce::KeyPress::returnKey));
    w->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
    w->enterModalState(
        true, juce::ModalCallbackFunction::create([safeThis, w](int r) {
          const juce::String name = w->getTextEditorContents("name").trim();
          delete w;
          if (safeThis == nullptr || r != 1 || name.isEmpty())
            return;
          if (safeThis->proc.saveUserPreset(name)) {
            safeThis->transientStatus = "Saved user preset '" + name + "'";
            safeThis->transientError = false;
            safeThis->proc.refreshPresetCatalog();
            safeThis->captureCompareBaseline();
          } else {
            safeThis->transientStatus = "Could not write preset file";
            safeThis->transientError = true;
          }
          safeThis->refreshLcdDisplay();
        }));
  }

  void promptDeletePreset() {
    auto dir = FmSynthProcessor::userPresetDir();
    auto files = dir.findChildFiles(juce::File::findFiles, false, "*.json");
    const juce::String target =
        juce::File::createLegalFileName(proc.getCurrentPresetName());
    for (const auto &f : files) {
      if (f.getFileNameWithoutExtension().equalsIgnoreCase(target)) {
        f.deleteFile();
        transientStatus = "Deleted user preset '" + proc.getCurrentPresetName() + "'";
        transientError = false;
        proc.setPresetDirty(true);
        proc.refreshPresetCatalog();
        refreshLcdDisplay();
        return;
      }
    }
    transientStatus = "Active patch is a built-in library voice (cannot delete)";
    transientError = true;
    refreshLcdDisplay();
  }

  static bool isBuiltInFactoryBankFile(const juce::File &f) {
    const juce::String stem = f.getFileNameWithoutExtension();
    if (stem.startsWithIgnoreCase("ROM1") || stem.startsWithIgnoreCase("ROM2") ||
        stem.startsWithIgnoreCase("ROM3") || stem.startsWithIgnoreCase("ROM4"))
      return true;
    if (stem.startsWithIgnoreCase("dx100_dx7_") ||
        stem.startsWithIgnoreCase("tx81z_"))
      return true;
    return false;
  }

  static std::vector<juce::File> findExternalSysexBanks() {
    std::vector<juce::File> out;
    juce::Array<juce::File> roots;
    roots.add(juce::File::getSpecialLocation(juce::File::currentExecutableFile)
                  .getParentDirectory()
                  .getChildFile("fm-sysex"));
    roots.add(juce::File::getCurrentWorkingDirectory().getChildFile("fm-sysex"));
    roots.add(
        FmSynthProcessor::userPresetDir().getParentDirectory().getChildFile(
            "fm-sysex"));

    for (const auto &r : roots) {
      if (!r.isDirectory())
        continue;
      auto files = r.findChildFiles(juce::File::findFiles, true, "*.syx;*.SYX");
      for (const auto &f : files) {
        if (isBuiltInFactoryBankFile(f))
          continue;
        juce::FileInputStream fis(f);
        uint8_t h[6] = {0, 0, 0, 0, 0, 0};
        if (fis.read(h, 6) == 6 && h[0] == 0xF0 && h[1] == 0x43 &&
            (h[3] == 0x09 || h[3] == 0x00))
          out.push_back(f);
      }
      if (!out.empty())
        break;
    }

    std::sort(out.begin(), out.end(),
              [](const juce::File &a, const juce::File &b) {
                return a.getFullPathName().compareIgnoreCase(
                           b.getFullPathName()) < 0;
              });
    return out;
  }

  void loadSysexBankFile(const juce::File &f) {
    juce::String err;
    const int count = proc.importSysexFile(f, err);
    if (count > 0) {
      if (compareActive)
        exitCompareKeepCurrent();
      activeSysexBankTitle = f.getFileNameWithoutExtension();
      proc.refreshPresetCatalog();
      proc.loadImportedPatch(0);
      proc.setPresetDirty(false);
      captureCompareBaseline();
      transientStatus = "Imported " + juce::String(count) + " voice(s) from " +
                        f.getFileName();
      transientError = false;
      syncAllControlsFromProcessor();
    } else {
      transientStatus = "SysEx load failed: " + err;
      transientError = true;
    }
    refreshLcdDisplay();
  }

  void showSysexMenu() {
    const auto customBanks = findExternalSysexBanks();
    juce::PopupMenu menu;

    const auto &imported = proc.getImportedBank();
    if (!imported.empty()) {
      juce::PopupMenu activeSub;
      for (size_t i = 0; i < imported.size(); ++i) {
        activeSub.addItem(
            5000 + static_cast<int>(i),
            juce::String(static_cast<int>(i) + 1).paddedLeft('0', 2) + ". " +
                imported[i].name,
            true,
            proc.getCurrentPresetProvenance() == "SYX" &&
                imported[i].name == proc.getCurrentPresetName());
      }
      menu.addSubMenu(
          activeSysexBankTitle.isNotEmpty()
              ? ("Imported Bank: " + activeSysexBankTitle)
              : ("Imported SysEx Bank (" + juce::String(imported.size()) +
                 " Voices)"),
          activeSub, true, nullptr, true);
      menu.addSeparator();
    }

    if (!customBanks.empty()) {
      juce::PopupMenu customSub;
      for (size_t i = 0; i < customBanks.size(); ++i) {
        const juce::String lbl = customBanks[i].getFileName();
        customSub.addItem(1000 + static_cast<int>(i), lbl, true,
                          customBanks[i].getFileNameWithoutExtension() ==
                              activeSysexBankTitle);
      }
      menu.addSubMenu("Custom Banks in fm-sysex/", customSub);
      menu.addSeparator();
    }

    menu.addItem(1, "Import External .syx Bank or Patch...");
    menu.addItem(2, "Export Current Patch as .syx...");

    juce::Component::SafePointer<FmSynthComponent> safeThis(this);
    menu.showMenuAsync(
        juce::PopupMenu::Options().withTargetComponent(&header.syxBtn),
        [safeThis, customBanks](int r) {
          if (safeThis == nullptr || r == 0)
            return;
          if (r == 1) {
            safeThis->importSysexFromDisk();
          } else if (r == 2) {
            safeThis->exportCurrentPatchSysex();
          } else if (r >= 1000 &&
                     static_cast<size_t>(r - 1000) < customBanks.size()) {
            safeThis->loadSysexBankFile(
                customBanks[static_cast<size_t>(r - 1000)]);
          } else if (r >= 5000) {
            const int voiceIdx = r - 5000;
            if (voiceIdx >= 0 &&
                voiceIdx <
                    static_cast<int>(safeThis->proc.getImportedBank().size())) {
              if (safeThis->compareActive)
                safeThis->exitCompareKeepCurrent();
              safeThis->proc.loadImportedPatch(voiceIdx);
              safeThis->proc.setPresetDirty(false);
              safeThis->captureCompareBaseline();
              safeThis->syncAllControlsFromProcessor();
              safeThis->refreshLcdDisplay();
            }
          }
        });
  }

  void importSysexFromDisk() {
    juce::Component::SafePointer<FmSynthComponent> safeThis(this);
    auto chooser = std::make_shared<juce::FileChooser>(
        "Import DX7 SysEx Bank or Patch", juce::File(),
        "*.syx;*.SYX;*.sysex;*.bin");
    chooser->launchAsync(juce::FileBrowserComponent::openMode |
                             juce::FileBrowserComponent::canSelectFiles,
                         [safeThis, chooser](const juce::FileChooser &fc) {
                           if (safeThis == nullptr)
                             return;
                           const auto f = fc.getResult();
                           if (f != juce::File())
                             safeThis->loadSysexBankFile(f);
                         });
  }

  void exportCurrentPatchSysex() {
    juce::Component::SafePointer<FmSynthComponent> safeThis(this);
    const auto defaultName =
        juce::File::createLegalFileName(proc.getCurrentPresetName()) + ".syx";
    auto chooser = std::make_shared<juce::FileChooser>(
        "Export Single Voice DX7 SysEx",
        juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
            .getChildFile(defaultName),
        "*.syx");
    chooser->launchAsync(
        juce::FileBrowserComponent::saveMode |
            juce::FileBrowserComponent::canSelectFiles |
            juce::FileBrowserComponent::warnAboutOverwriting,
        [safeThis, chooser](const juce::FileChooser &fc) {
          if (safeThis == nullptr)
            return;
          const auto f = fc.getResult();
          if (f == juce::File())
            return;
          uint8_t raw[FanfareFm::kSingleSysexSize];
          FanfareFm::packSingleSysex(safeThis->proc.engine().patch(), raw);
          if (f.replaceWithData(raw, FanfareFm::kSingleSysexSize)) {
            safeThis->transientStatus = "Exported '" + f.getFileName() + "'";
            safeThis->transientError = false;
          } else {
            safeThis->transientStatus = "Could not write .syx file";
            safeThis->transientError = true;
          }
          safeThis->refreshLcdDisplay();
        });
  }

  void timerCallback() override {
    header.updateMeterTelemetry(proc.getOutputPeakL(), proc.getOutputPeakR(),
                                proc.getActiveVoices(),
                                proc.engine().polyphony());

    if (!anyWidgetDragging()) {
      const uint64_t nowHash = computeEngineSnapshotHash();
      if (nowHash != lastSyncedHash)
        syncAllControlsFromProcessor();
    }
  }

  RackSlot &slotRef;
  int chainIndex = 0;
  FmSynthProcessor &proc;
  std::function<void()> closeCallback;

  FanfareFm::Ui::FmHeaderBar header;
  FanfareFm::Ui::FmModeBar modeBar;
  std::array<std::unique_ptr<FanfareFm::Ui::FmOperatorCard>, 6> operatorCards;
  FanfareFm::Ui::FmRightDeck rightDeck;
  FanfareFm::Ui::FmKeyboardStrip keyboard;

  juce::ComponentBoundsConstrainer resizeLimits;
  std::unique_ptr<juce::ResizableCornerComponent> resizer;

  juce::var compareBaselineState;
  juce::var preCompareState;
  bool compareActive = false;
  bool showKeyboard = false;
  bool userDraggedCorner = false;
  bool applyingHostBounds = false;
  bool isControlDragging = false;
  bool isFineGear = false;
  bool transientError = false;

  uint64_t lastSyncedHash = 0;
  juce::String focusedTelemetry;
  juce::String transientStatus;
  juce::String activeSysexBankTitle;

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(FmSynthComponent)
};
