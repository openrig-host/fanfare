#pragma once

#include <JuceHeader.h>
#include <cmath>
#include <functional>
#include "../FmSynthProcessor.h"
#include "FmUiMetrics.h"

namespace FanfareFm::Ui {

class FmPatchLcd : public juce::Component, public juce::SettableTooltipClient {
public:
  std::function<void()> onOpenPresetMenu;

  FmPatchLcd() { setMouseCursor(juce::MouseCursor::PointingHandCursor); }

  void setPatchDisplay(int indexOneBased, const juce::String &modeTag,
                       const juce::String &name, bool isDirty,
                       const juce::String &sourceTag) {
    patchNum = indexOneBased > 0
                   ? juce::String(indexOneBased).paddedLeft('0', 3)
                   : "---";
    modeBadge = modeTag;
    patchName = name.isNotEmpty() ? name : juce::String("INIT VOICE");
    dirty = isDirty;
    bankBadge = sourceTag;
    setTooltip("Patch: " + patchName +
               (bankBadge.isNotEmpty() ? " [" + bankBadge + "]" : "") +
               (dirty ? " (modified)" : "") +
               " - Click to browse categorized DX7, DX100, TX81Z, and User presets");
    repaint();
  }

  void setTelemetryText(const juce::String &text, bool fineGear,
                        bool isError = false, bool isWarn = false) {
    if (telemetry == text && fineMode == fineGear && errorState == isError &&
        warnState == isWarn) {
      return;
    }
    telemetry = text;
    fineMode = fineGear;
    errorState = isError;
    warnState = isWarn;
    repaint();
  }

  void paint(juce::Graphics &g) override {
    const auto b = getLocalBounds().toFloat().reduced(0.5f);
    g.setColour(ThemeManager::get(Theme::Role::sunken));
    g.fillRoundedRectangle(b, 3.5f);

    const juce::Colour borderCol =
        errorState ? ThemeManager::get(Theme::Role::danger)
                   : (hovered ? ThemeManager::get(Theme::Role::accent)
                              : ThemeManager::get(Theme::Role::borderStrong));
    g.setColour(borderCol);
    g.drawRoundedRectangle(b, 3.5f, 1.0f);

    auto inner = getLocalBounds().reduced(7, 3);
    const int leftW = juce::jmax(150, static_cast<int>(inner.getWidth() * 0.48f));
    auto patchSide = inner.removeFromLeft(leftW);
    const int divX = inner.getX() + 2;
    inner.removeFromLeft(8);

    g.setColour(ThemeManager::get(Theme::Role::border));
    g.drawVerticalLine(divX, static_cast<float>(inner.getY()) + 2.0f,
                       static_cast<float>(inner.getBottom()) - 2.0f);

    auto numArea = patchSide.removeFromLeft(28);
    g.setColour(ThemeManager::get(Theme::Role::accent));
    g.setFont(BoutiqueLookAndFeel::monoFont(11.0f, true));
    g.drawText(patchNum, numArea, juce::Justification::centredLeft, false);

    if (bankBadge.isNotEmpty() && patchSide.getWidth() > 110) {
      auto srcBox = patchSide.removeFromRight(42).reduced(0, 4);
      g.setColour(ThemeManager::get(Theme::Role::panelAlt));
      g.fillRoundedRectangle(srcBox.toFloat(), 2.0f);
      g.setColour(ThemeManager::get(Theme::Role::accent));
      g.setFont(BoutiqueLookAndFeel::condensedFont(9.5f, 0.04f));
      g.drawText(bankBadge, srcBox, juce::Justification::centred, false);
      patchSide.removeFromRight(5);
    }

    auto arrowArea = patchSide.removeFromRight(12);
    juce::Path chev;
    const float cx = static_cast<float>(arrowArea.getCentreX());
    const float cy = static_cast<float>(arrowArea.getCentreY());
    chev.addTriangle(cx - 3.5f, cy - 1.5f, cx + 3.5f, cy - 1.5f, cx, cy + 2.5f);
    g.setColour(ThemeManager::get(Theme::Role::textDim));
    g.fillPath(chev);

    g.setColour(ThemeManager::get(Theme::Role::text));
    g.setFont(BoutiqueLookAndFeel::uiFont(12.8f, true));
    g.drawText(patchName + (dirty ? " *" : ""), patchSide,
               juce::Justification::centredLeft, true);

    if (fineMode && inner.getWidth() > 70) {
      auto fineBox = inner.removeFromRight(54).reduced(0, 4);
      g.setColour(ThemeManager::get(Theme::Role::accent).withAlpha(0.22f));
      g.fillRoundedRectangle(fineBox.toFloat(), 2.5f);
      g.setColour(ThemeManager::get(Theme::Role::accent));
      g.drawRoundedRectangle(fineBox.toFloat(), 2.5f, 1.0f);
      g.setFont(BoutiqueLookAndFeel::monoFont(9.2f, true));
      g.drawText("FINE 10x", fineBox, juce::Justification::centred, false);
      inner.removeFromRight(5);
    }

    const juce::Colour teleCol =
        errorState ? ThemeManager::get(Theme::Role::danger)
                   : (warnState ? ThemeManager::get(Theme::Role::warn)
                                : ThemeManager::get(Theme::Role::textDim));
    g.setColour(teleCol);
    g.setFont(BoutiqueLookAndFeel::monoFont(10.8f, false));
    g.drawText(telemetry, inner, juce::Justification::centredRight, true);
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
  juce::String patchNum{"001"};
  juce::String modeBadge{"DX7"};
  juce::String patchName{"INIT VOICE"};
  juce::String bankBadge;
  juce::String telemetry;
  bool dirty = false;
  bool fineMode = false;
  bool errorState = false;
  bool warnState = false;
  bool hovered = false;
};

class FmHeaderBar : public juce::Component {
public:
  explicit FmHeaderBar(FmSynthProcessor &processor) : proc(processor) {
    enableBtn.setClickingTogglesState(true);
    enableBtn.setColour(juce::TextButton::buttonOnColourId,
                        ThemeManager::get(Theme::Role::ok));
    enableBtn.setTooltip("Enable or bypass CoplanDX on this rack slot");
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

    syxBtn.setButtonText("SYX");
    syxBtn.setColour(juce::TextButton::buttonColourId,
                     ThemeManager::get(Theme::Role::accentDim).darker(0.35f));
    syxBtn.setTooltip("Import external .syx banks/voices or export current patch as .syx");
    addAndMakeVisible(syxBtn);

    compareBtn.setButtonText("CMP");
    compareBtn.setClickingTogglesState(true);
    compareBtn.setColour(juce::TextButton::buttonOnColourId,
                         ThemeManager::get(Theme::Role::warn));
    compareBtn.setTooltip("Temporarily audition the unedited preset before your tweaks");
    addAndMakeVisible(compareBtn);

    initBtn.setButtonText("INIT");
    initBtn.setTooltip("Reset current voice to clean DX7 Init Voice");
    addAndMakeVisible(initBtn);

    saveBtn.setButtonText("SAVE");
    saveBtn.setTooltip("Save current patch as a user preset");
    addAndMakeVisible(saveBtn);

    delBtn.setButtonText("DEL");
    delBtn.setColour(juce::TextButton::buttonColourId,
                     ThemeManager::get(Theme::Role::danger).darker(0.35f));
    delBtn.setTooltip("Delete the active user preset");
    addAndMakeVisible(delBtn);

    keysBtn.setButtonText("KEYS");
    keysBtn.setClickingTogglesState(true);
    keysBtn.setColour(juce::TextButton::buttonOnColourId,
                      ThemeManager::get(Theme::Role::accentDim));
    keysBtn.setTooltip("Toggle 2-octave playable keyboard + Pitch Bend / Mod strips");
    addAndMakeVisible(keysBtn);

    midiBtn.setButtonText("MIDI");
    midiBtn.setColour(juce::TextButton::textColourOffId,
                      ThemeManager::get(Theme::Role::accent));
    midiBtn.setTooltip("MIDI Routing & Splits: Note Range, CC Assignments, and MIDI Transformer");
    midiBtn.onClick = [this] {
      if (onOpenMidiDialog)
        onOpenMidiDialog();
    };
    addAndMakeVisible(midiBtn);

    closeBtn.setButtonText("X");
    closeBtn.setTooltip("Close CoplanDX editor");
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

  void updateMeterTelemetry(float leftPeak, float rightPeak, int activeVoices,
                            int maxVoices) {
    const float nextL = juce::jlimit(0.0f, 1.0f, leftPeak);
    const float nextR = juce::jlimit(0.0f, 1.0f, rightPeak);
    if (std::abs(nextL - meterL) > 0.01f || std::abs(nextR - meterR) > 0.01f ||
        activeVoices != voicesActive || maxVoices != voicesMax) {
      meterL = nextL;
      meterR = nextR;
      voicesActive = activeVoices;
      voicesMax = maxVoices;
      repaint(brandArea);
    }
  }

  void paint(juce::Graphics &g) override {
    auto b = brandArea;
    if (b.isEmpty())
      return;

    auto stripe = b.removeFromLeft(4).reduced(0, 5);
    g.setColour(ThemeManager::get(Theme::Role::accent));
    g.fillRoundedRectangle(stripe.toFloat(), 1.5f);
    b.removeFromLeft(6);

    auto titleBox = b.removeFromLeft(118);
    g.setColour(ThemeManager::get(Theme::Role::text));
    g.setFont(BoutiqueLookAndFeel::condensedFont(15.5f, 0.05f));
    g.drawFittedText("COPLANDX  [S" + juce::String(chainSlotIndex + 1) + "]",
                     titleBox.removeFromTop(18),
                     juce::Justification::centredLeft, 1, 0.85f);

    g.setColour(ThemeManager::get(Theme::Role::textFaint));
    g.setFont(BoutiqueLookAndFeel::monoFont(9.5f, false));
    g.drawText("VOICES " + juce::String(voicesActive) + "/" +
                   juce::String(voicesMax),
               titleBox, juce::Justification::centredLeft, false);

    if (b.getWidth() >= 14) {
      const auto meterRect = b.removeFromLeft(14).reduced(1, 4).toFloat();
      BoutiqueLookAndFeel::drawStereoSegmentedMeter(
          g, meterRect, meterL, meterR, 8,
          ThemeManager::get(Theme::Role::meterLow),
          ThemeManager::get(Theme::Role::meterMid),
          ThemeManager::get(Theme::Role::meterPeak));
    }
  }

  void resized() override {
    auto b = getLocalBounds();
    brandArea = b.removeFromLeft(150);
    b.removeFromLeft(4);

    enableBtn.setBounds(b.removeFromLeft(86).reduced(1, 3));
    b.removeFromLeft(5);

    closeBtn.setBounds(b.removeFromRight(28).reduced(1, 3));
    b.removeFromRight(3);
    keysBtn.setBounds(b.removeFromRight(46).reduced(1, 3));
    b.removeFromRight(3);
    midiBtn.setBounds(b.removeFromRight(46).reduced(1, 3));
    b.removeFromRight(3);
    delBtn.setBounds(b.removeFromRight(36).reduced(1, 3));
    saveBtn.setBounds(b.removeFromRight(44).reduced(1, 3));
    initBtn.setBounds(b.removeFromRight(40).reduced(1, 3));
    compareBtn.setBounds(b.removeFromRight(42).reduced(1, 3));
    b.removeFromRight(3);
    syxBtn.setBounds(b.removeFromRight(42).reduced(1, 3));
    b.removeFromRight(3);
    nextBtn.setBounds(b.removeFromRight(25).reduced(1, 3));
    prevBtn.setBounds(b.removeFromRight(25).reduced(1, 3));
    b.removeFromRight(5);

    lcd.setBounds(b.reduced(0, 2));
  }

  juce::TextButton enableBtn;
  FmPatchLcd lcd;
  juce::TextButton prevBtn, nextBtn, syxBtn, compareBtn, initBtn, saveBtn,
      delBtn, midiBtn, keysBtn, closeBtn;

private:
  FmSynthProcessor &proc;
  juce::Rectangle<int> brandArea;
  float meterL = 0.0f;
  float meterR = 0.0f;
  int voicesActive = 0;
  int voicesMax = 16;
  int chainSlotIndex = 0;
};

} // namespace FanfareFm::Ui
