#pragma once

#include <JuceHeader.h>
#include <functional>
#include "../../BoutiqueLookAndFeel.h"
#include "../../MidiLearnBus.h"
#include "../../ThemeManager.h"
#include "../FanfareSynthProcessorImpl.h"
#include "../SynthConstants.h"
#include "../SynthParams.h"

namespace FanfareSynth::Ui {

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
    g.setColour(ThemeManager::get(Theme::Role::sunken));
    g.fillRoundedRectangle(b, 3.5f);

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

    // Patch number badge
    auto numArea = patchSide.removeFromLeft(30);
    g.setColour(ThemeManager::get(Theme::Role::accent));
    g.setFont(BoutiqueLookAndFeel::monoFont(11.5f, true));
    g.drawText(patchNumberText, numArea, juce::Justification::centredLeft,
               false);

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
    g.setColour(teleCol);
    g.setFont(BoutiqueLookAndFeel::monoFont(11.0f, false));
    g.drawText(telemetryText, inner, juce::Justification::centredRight, true);
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
};

class SynthHeaderBar : public juce::Component {
public:
  SynthHeaderBar(FanfareSynthProcessor &processor) : proc(processor) {
    enableBtn.setClickingTogglesState(true);
    enableBtn.setColour(juce::TextButton::buttonOnColourId,
                        ThemeManager::get(Theme::Role::ok));
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
    compareBtn.setColour(juce::TextButton::buttonOnColourId,
                         ThemeManager::get(Theme::Role::warn));
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
    delBtn.setColour(juce::TextButton::buttonColourId,
                     ThemeManager::get(Theme::Role::danger).darker(0.35f));
    delBtn.setTooltip("Delete the selected user preset");
    addAndMakeVisible(delBtn);

    keysBtn.setButtonText("KEYS");
    keysBtn.setClickingTogglesState(true);
    keysBtn.setColour(juce::TextButton::buttonOnColourId,
                      ThemeManager::get(Theme::Role::accentDim));
    keysBtn.setTooltip(
        "Toggle onscreen 2-octave keyboard + Pitch Bend / Mod strips");
    addAndMakeVisible(keysBtn);

    closeBtn.setButtonText("X");
    closeBtn.setTooltip("Close synth editor");
    addAndMakeVisible(closeBtn);

    syncEnableButton();
  }

  std::function<void(bool)> onPowerToggled;

  void syncEnableButton() {
    const bool on = proc.enabled.load();
    enableBtn.setToggleState(on, juce::dontSendNotification);
    enableBtn.setButtonText(on ? "SYNTH: ON" : "SYNTH: OFF");
    repaint();
  }

  void updateMeterTelemetry(float leftPeak, float rightPeak, int activeVoices) {
    const float nextL = juce::jlimit(0.0f, 1.0f, leftPeak);
    const float nextR = juce::jlimit(0.0f, 1.0f, rightPeak);
    if (std::abs(nextL - meterL) > 0.01f || std::abs(nextR - meterR) > 0.01f ||
        activeVoices != voicesActive) {
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

    // Juno-106 inspired stripe + model title
    auto stripe = b.removeFromLeft(4).reduced(0, 5);
    g.setColour(ThemeManager::get(Theme::Role::accent));
    g.fillRoundedRectangle(stripe.toFloat(), 1.5f);
    b.removeFromLeft(6);

    auto titleBox = b.removeFromLeft(128);
    g.setColour(ThemeManager::get(Theme::Role::text));
    g.setFont(BoutiqueLookAndFeel::condensedFont(16.5f, 0.06f));
    g.drawFittedText("FANFARE SYN-106", titleBox.removeFromTop(19),
                     juce::Justification::centredLeft, 1, 0.85f);

    g.setColour(ThemeManager::get(Theme::Role::textFaint));
    g.setFont(BoutiqueLookAndFeel::monoFont(9.5f, false));
    g.drawText("VOICES " + juce::String(voicesActive) + "/16", titleBox,
               juce::Justification::centredLeft, false);

    if (b.getWidth() >= 14) {
      auto meterRect =
          b.removeFromLeft(14).reduced(1, 4).toFloat();
      BoutiqueLookAndFeel::drawStereoSegmentedMeter(
          g, meterRect, meterL, meterR, 8,
          ThemeManager::get(Theme::Role::meterLow),
          ThemeManager::get(Theme::Role::meterMid),
          ThemeManager::get(Theme::Role::meterPeak));
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

  juce::TextButton enableBtn;
  SynthPatchLcd lcd;
  juce::TextButton prevBtn, nextBtn, compareBtn, initBtn, saveBtn, delBtn,
      keysBtn, closeBtn;

private:
  FanfareSynthProcessor &proc;
  juce::Rectangle<int> telemetryBrandArea;
  float meterL = 0.0f;
  float meterR = 0.0f;
  int voicesActive = 0;
};

} // namespace FanfareSynth::Ui
