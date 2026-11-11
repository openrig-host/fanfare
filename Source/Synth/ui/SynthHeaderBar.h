#pragma once

#include <JuceHeader.h>
#include <functional>
#include "../../BoutiqueLookAndFeel.h"
#include "../../MidiLearnBus.h"
#include "../../ThemeManager.h"
#include "../FanfareSynthProcessorImpl.h"
#include "../SynthConstants.h"
#include "../SynthParams.h"
#include "SynthSkin.h"

namespace FanfareSynth::Ui {

class SynthKeyButton : public juce::TextButton {
public:
  SynthKeyButton() : juce::TextButton("", {}) {}

  juce::Colour onTint;
  juce::Colour textTint;
  bool showLed = false;

  void paintButton(juce::Graphics &g, bool isOver, bool isDown) override {
    const bool on = getToggleState() && getClickingTogglesState();
    const bool enabled = isEnabled();
    const auto r = getLocalBounds().toFloat().reduced(0.5f, 0.5f);
    const juce::Colour tint =
        onTint.getARGB() != 0 ? onTint : ThemeManager::get(Theme::Role::accent);

    Skin::drawKeycap(g, r, isDown, on && enabled, tint, 3.0f);

    if (isOver && enabled) {
      g.setColour(juce::Colours::white.withAlpha(Skin::isDark() ? 0.06f
                                                                : 0.30f));
      g.fillRoundedRectangle(r.reduced(1.0f), 2.5f);
    }

    auto textArea = getLocalBounds().reduced(4, 0);
    if (showLed) {
      const auto led = textArea.removeFromLeft(9)
                           .withSizeKeepingCentre(5, 5)
                           .toFloat();
      textArea.removeFromLeft(2);
      Skin::drawLed(g, led, tint, on && enabled);
    }

    const juce::Colour textCol =
        !enabled  ? ThemeManager::get(Theme::Role::textFaint)
        : on      ? tint
        : isOver  ? ThemeManager::get(Theme::Role::text)
        : textTint.getARGB() != 0 ? textTint
                                  : ThemeManager::get(Theme::Role::textDim);
    g.setFont(BoutiqueLookAndFeel::condensedFont(
        juce::jmin(12.0f, (float)getHeight() * 0.42f), 0.06f));
    g.drawFittedText(getButtonText(), textArea, juce::Justification::centred,
                     1, 0.9f);
  }
};

class SynthPatchLcd : public juce::Component,
                      public juce::SettableTooltipClient {
public:
  std::function<void()> onOpenPresetMenu;

  SynthPatchLcd() { setMouseCursor(juce::MouseCursor::PointingHandCursor); }

  void setPatchDisplay(int patchIndexOneBased, const juce::String &name,
                       bool isDirty, bool isUserPreset) {
    patchNumberText =
        patchIndexOneBased > 0
            ? juce::String(patchIndexOneBased).paddedLeft('0', 3)
            : "---";
    patchNameText = name;
    dirty = isDirty;
    userTag = isUserPreset;
    setTooltip((isUserPreset ? "User Preset: " : "Preset: ") + name +
               (isDirty ? " (modified)" : "") + " - Click to browse patches");
    repaint();
  }

  void setTelemetryText(const juce::String &text, bool fineGear,
                        bool isErrorState, bool isWarningState) {
    if (telemetryText == text && fineMode == fineGear &&
        errorFlag == isErrorState && warnFlag == isWarningState)
      return;
    telemetryText = text;
    fineMode = fineGear;
    errorFlag = isErrorState;
    warnFlag = isWarningState;
    repaint();
  }

  void paint(juce::Graphics &g) override {
    const auto b = getLocalBounds().toFloat().reduced(0.5f);
    Skin::drawRecess(g, b, 3.5f);

    const auto backlight =
        ThemeManager::get(Theme::Role::accent).withAlpha(0.045f);
    g.setColour(backlight);
    g.fillRoundedRectangle(b.reduced(1.0f), 3.0f);

    scanlines.render(getWidth(), getHeight(), Skin::themeSignature(),
                     [](juce::Graphics &cg, juce::Rectangle<int> r) {
                       cg.drawImageAt(Skin::makeScanlines(r.getWidth(),
                                                          r.getHeight()),
                                      0, 0);
                     });
    g.saveState();
    g.reduceClipRegion(getLocalBounds());
    scanlines.draw(g, 0, 0);
    Skin::fillVertical(
        g, b.withHeight(b.getHeight() * 0.45f).reduced(1.0f, 1.0f),
        juce::Colours::white.withAlpha(Skin::isDark() ? 0.05f : 0.22f),
        juce::Colours::white.withAlpha(0.0f), 3.0f);
    g.restoreState();

    const juce::Colour borderCol =
        errorFlag ? ThemeManager::get(Theme::Role::danger)
                  : (hovered ? ThemeManager::get(Theme::Role::accent)
                             : ThemeManager::get(Theme::Role::borderStrong));
    g.setColour(borderCol);
    g.drawRoundedRectangle(b, 3.5f, 1.0f);

    auto inner = getLocalBounds().reduced(8, 3);
    const int leftWidth = juce::jmax(140, (int)(inner.getWidth() * 0.46f));
    auto patchSide = inner.removeFromLeft(leftWidth);
    const int dividerX = inner.getX() + 2;
    inner.removeFromLeft(8);

    g.setColour(ThemeManager::get(Theme::Role::border));
    g.drawVerticalLine(dividerX, (float)inner.getY() + 2.0f,
                       (float)inner.getBottom() - 2.0f);

    auto numArea = patchSide.removeFromLeft(30);
    Skin::drawGlowText(g, patchNumberText, numArea,
                       BoutiqueLookAndFeel::monoFont(11.5f, true),
                       ThemeManager::get(Theme::Role::accent),
                       juce::Justification::centredLeft);

    if (userTag) {
      auto usrBadge = patchSide.removeFromRight(32).reduced(0, 4);
      g.setColour(ThemeManager::get(Theme::Role::panelAlt));
      g.fillRoundedRectangle(usrBadge.toFloat(), 2.0f);
      g.setColour(ThemeManager::get(Theme::Role::textDim));
      g.setFont(BoutiqueLookAndFeel::condensedFont(9.5f, 0.04f));
      g.drawText("USER", usrBadge, juce::Justification::centred, false);
      patchSide.removeFromRight(4);
    }

    // Dropdown chevron indicator
    auto arrowArea = patchSide.removeFromRight(12);
    juce::Path chevron;
    const float cx = (float)arrowArea.getCentreX();
    const float cy = (float)arrowArea.getCentreY();
    chevron.addTriangle(cx - 3.5f, cy - 1.5f, cx + 3.5f, cy - 1.5f, cx,
                        cy + 2.5f);
    g.setColour(ThemeManager::get(Theme::Role::textDim));
    g.fillPath(chevron);

    // Patch name with ellipsis overflow handling
    const juce::String fullPatchLabel =
        patchNameText + (dirty ? " *" : "");
    g.setColour(ThemeManager::get(Theme::Role::text));
    g.setFont(BoutiqueLookAndFeel::uiFont(13.0f, true));
    g.drawText(fullPatchLabel, patchSide, juce::Justification::centredLeft,
               true);

    // Right side: Live parameter telemetry / status banner
    if (fineMode) {
      auto fineBadge = inner.removeFromRight(56).reduced(0, 4);
      g.setColour(ThemeManager::get(Theme::Role::accent).withAlpha(0.22f));
      g.fillRoundedRectangle(fineBadge.toFloat(), 2.5f);
      g.setColour(ThemeManager::get(Theme::Role::accent));
      g.drawRoundedRectangle(fineBadge.toFloat(), 2.5f, 1.0f);
      g.setFont(BoutiqueLookAndFeel::monoFont(9.5f, true));
      g.drawText("FINE 10x", fineBadge, juce::Justification::centred, false);
      inner.removeFromRight(6);
    }

    const juce::Colour teleCol =
        errorFlag ? ThemeManager::get(Theme::Role::danger)
                  : (warnFlag ? ThemeManager::get(Theme::Role::warn)
                              : ThemeManager::get(Theme::Role::textDim));
    Skin::drawGlowText(g, telemetryText, inner,
                       BoutiqueLookAndFeel::monoFont(11.0f, false), teleCol,
                       juce::Justification::centredRight, true);
  }

  void mouseEnter(const juce::MouseEvent &) override {
    hovered = true;
    repaint();
  }
  void mouseExit(const juce::MouseEvent &) override {
    hovered = false;
    repaint();
  }
  void mouseDown(const juce::MouseEvent &) override {
    if (onOpenPresetMenu)
      onOpenPresetMenu();
  }

private:
  juce::String patchNumberText{"001"};
  juce::String patchNameText{"Init Patch"};
  juce::String telemetryText;
  bool dirty = false;
  bool userTag = false;
  bool fineMode = false;
  bool errorFlag = false;
  bool warnFlag = false;
  bool hovered = false;
  Skin::ChromeCache scanlines;
};

class SynthHeaderBar : public juce::Component {
public:
  SynthHeaderBar(FanfareSynthProcessor &processor) : proc(processor) {
    enableBtn.setClickingTogglesState(true);
    enableBtn.showLed = true;
    enableBtn.onTint = ThemeManager::get(Theme::Role::ok);
    enableBtn.setTooltip("Enable or bypass the native synth on this rack slot");
    enableBtn.onClick = [this] {
      const bool on = enableBtn.getToggleState();
      proc.enabled.store(on);
      syncEnableButton();
      if (onPowerToggled)
        onPowerToggled(on);
    };
    addAndMakeVisible(enableBtn);

    addAndMakeVisible(lcd);

    prevBtn.setButtonText("<");
    prevBtn.setTooltip("Previous patch");
    addAndMakeVisible(prevBtn);

    nextBtn.setButtonText(">");
    nextBtn.setTooltip("Next patch");
    addAndMakeVisible(nextBtn);

    compareBtn.setButtonText("COMPARE");
    compareBtn.setClickingTogglesState(true);
    compareBtn.onTint = ThemeManager::get(Theme::Role::warn);
    compareBtn.setTooltip(
        "Temporarily audition the unedited preset before your tweaks");
    addAndMakeVisible(compareBtn);

    initBtn.setButtonText("INIT");
    initBtn.setTooltip("Reset all parameters to a clean Init Patch");
    addAndMakeVisible(initBtn);

    saveBtn.setButtonText("SAVE");
    saveBtn.setTooltip("Save current patch as a user preset");
    addAndMakeVisible(saveBtn);

    delBtn.setButtonText("DEL");
    delBtn.textTint = ThemeManager::get(Theme::Role::danger);
    delBtn.setTooltip("Delete the selected user preset");
    addAndMakeVisible(delBtn);

    keysBtn.setButtonText("KEYS");
    keysBtn.setClickingTogglesState(true);
    keysBtn.onTint = ThemeManager::get(Theme::Role::accent);
    keysBtn.setTooltip(
        "Toggle onscreen 2-octave keyboard + Pitch Bend / Mod strips");
    addAndMakeVisible(keysBtn);

    midiBtn.setButtonText("MIDI");
    midiBtn.textTint = ThemeManager::get(Theme::Role::accent);
    midiBtn.setTooltip(
        "MIDI Routing & Splits: Note Range, CC Assignments, and MIDI Transformer");
    midiBtn.onClick = [this] {
      if (onOpenMidiDialog)
        onOpenMidiDialog();
    };
    addAndMakeVisible(midiBtn);

    closeBtn.setButtonText("X");
    closeBtn.setTooltip("Close synth editor");
    addAndMakeVisible(closeBtn);

    syncEnableButton();
  }

  std::function<void(bool)> onPowerToggled;
  std::function<void()> onOpenMidiDialog;

  void setChainSlotIndex(int idx) {
    chainSlotIndex = juce::jlimit(0, 2, idx);
    repaint();
  }

  void syncEnableButton() {
    const bool on = proc.enabled.load();
    enableBtn.setToggleState(on, juce::dontSendNotification);
    enableBtn.setButtonText(on ? "SYNTH: ON" : "SYNTH: OFF");
    repaint();
  }

  void updateMeterTelemetry(float leftPeak, float rightPeak, int activeVoices) {
    const float nextL = juce::jlimit(0.0f, 1.0f, leftPeak);
    const float nextR = juce::jlimit(0.0f, 1.0f, rightPeak);
    peakL = juce::jmax(nextL, peakL - 0.014f);
    peakR = juce::jmax(nextR, peakR - 0.014f);
    const bool peakFalling = peakL > 0.001f || peakR > 0.001f;
    if (std::abs(nextL - meterL) > 0.01f || std::abs(nextR - meterR) > 0.01f ||
        activeVoices != voicesActive || peakFalling) {
      meterL = nextL;
      meterR = nextR;
      voicesActive = activeVoices;
      repaint(telemetryBrandArea);
    }
  }

  void paint(juce::Graphics &g) override {
    auto b = telemetryBrandArea;
    if (b.isEmpty())
      return;

    auto stripeBlock = b.removeFromLeft(15).reduced(0, 4);
    const juce::Colour bars[5] = {
        ThemeManager::get(Theme::Role::catAccordion),
        ThemeManager::get(Theme::Role::catMonitor),
        ThemeManager::get(Theme::Role::ok),
        ThemeManager::get(Theme::Role::catKeyboard),
        ThemeManager::get(Theme::Role::accent)};
    const float bw = (float)stripeBlock.getWidth() / 5.0f;
    for (int i = 0; i < 5; ++i)
      g.fillRect(juce::Rectangle<float>(
          (float)stripeBlock.getX() + (float)i * bw,
          (float)stripeBlock.getY(), bw - 1.0f,
          (float)stripeBlock.getHeight()));
    b.removeFromLeft(6);

    auto titleBox = b.removeFromLeft(128);
    auto titleLine = titleBox.removeFromTop(19);
    const juce::String title =
        "BLEEP-216  [S" + juce::String(chainSlotIndex + 1) + "]";
    g.setFont(BoutiqueLookAndFeel::condensedFont(16.0f, 0.05f));
    g.setColour(ThemeManager::get(Theme::Role::text).withAlpha(0.30f));
    g.drawFittedText(title, titleLine.expanded(1, 1),
                     juce::Justification::centredLeft, 1, 0.85f);
    g.setColour(ThemeManager::get(Theme::Role::text));
    g.drawFittedText(title, titleLine, juce::Justification::centredLeft, 1,
                     0.85f);

    auto countArea = titleBox.removeFromRight(28);
    const float dotW = 4.0f;
    const float dotGap = 1.6f;
    for (int i = 0; i < 16; ++i) {
      const bool lit = i < voicesActive;
      const auto dot = juce::Rectangle<float>(
          (float)titleBox.getX() + (float)i * (dotW + dotGap),
          (float)titleBox.getCentreY() - 2.0f, dotW, 4.0f);
      Skin::drawLed(g, dot, ThemeManager::get(Theme::Role::accent), lit, lit);
    }
    g.setColour(ThemeManager::get(Theme::Role::textFaint));
    g.setFont(BoutiqueLookAndFeel::monoFont(9.0f, false));
    g.drawText(juce::String(voicesActive) + "/16", countArea,
               juce::Justification::centredRight, false);

    if (b.getWidth() >= 16) {
      auto meterRect = b.removeFromLeft(16).reduced(1, 4).toFloat();
      const float barW = (meterRect.getWidth() - 2.0f) * 0.5f;
      auto lBar = meterRect.withWidth(barW);
      auto rBar = meterRect.withTrimmedLeft(barW + 2.0f);
      const auto low = ThemeManager::get(Theme::Role::meterLow);
      const auto mid = ThemeManager::get(Theme::Role::meterMid);
      const auto pk = ThemeManager::get(Theme::Role::meterPeak);
      BoutiqueLookAndFeel::drawSegmentedMeterBar(g, lBar, meterL, 8, low, mid,
                                                 pk);
      BoutiqueLookAndFeel::drawSegmentedMeterBar(g, rBar, meterR, 8, low, mid,
                                                 pk);
      g.setColour(pk);
      g.fillRect(lBar.withHeight(1.5f).withY(lBar.getBottom() -
                                             peakL * lBar.getHeight() - 1.5f));
      g.fillRect(rBar.withHeight(1.5f).withY(rBar.getBottom() -
                                             peakR * rBar.getHeight() - 1.5f));
    }
  }

  void resized() override {
    auto b = getLocalBounds();
    telemetryBrandArea = b.removeFromLeft(158);
    b.removeFromLeft(4);

    enableBtn.setBounds(b.removeFromLeft(90).reduced(1, 3));
    b.removeFromLeft(6);

    closeBtn.setBounds(b.removeFromRight(30).reduced(1, 3));
    b.removeFromRight(3);
    keysBtn.setBounds(b.removeFromRight(48).reduced(1, 3));
    b.removeFromRight(4);
    midiBtn.setBounds(b.removeFromRight(48).reduced(1, 3));
    b.removeFromRight(4);
    delBtn.setBounds(b.removeFromRight(38).reduced(1, 3));
    saveBtn.setBounds(b.removeFromRight(46).reduced(1, 3));
    initBtn.setBounds(b.removeFromRight(42).reduced(1, 3));
    compareBtn.setBounds(b.removeFromRight(66).reduced(1, 3));
    b.removeFromRight(4);
    nextBtn.setBounds(b.removeFromRight(26).reduced(1, 3));
    prevBtn.setBounds(b.removeFromRight(26).reduced(1, 3));
    b.removeFromRight(4);

    lcd.setBounds(b.reduced(0, 2));
  }

  SynthKeyButton enableBtn;
  SynthPatchLcd lcd;
  SynthKeyButton prevBtn, nextBtn, compareBtn, initBtn, saveBtn, delBtn,
      midiBtn, keysBtn, closeBtn;

private:
  FanfareSynthProcessor &proc;
  juce::Rectangle<int> telemetryBrandArea;
  float meterL = 0.0f;
  float meterR = 0.0f;
  float peakL = 0.0f;
  float peakR = 0.0f;
  int voicesActive = 0;
  int chainSlotIndex = 0;
};

} // namespace FanfareSynth::Ui
