#pragma once

#include <JuceHeader.h>
#include "RackSlot.h"
#include "BoutiqueLookAndFeel.h"
#include "ButtonRoles.h"
#include "ThemeManager.h"

/**
 * SynthMidiSetupDialog: Quick-access MIDI routing and setup hub opened from
 * the BLEEP-216 analog synth or CoplanDX FM synth header bars.
 *
 * Provides one-click access and live status summaries for:
 *  1. 3-Socket Note Range & Keyboard Splits
 *  2. MIDI CC Controller Assignments & Learn
 *  3. MIDI Transformer, Arpeggiator & Harmonizer
 */
class SynthMidiSetupDialog : public juce::Component {
public:
  SynthMidiSetupDialog(RackSlot &s, int chainIdx, bool isFmSynth,
                       std::function<void()> openNoteRangeCb,
                       std::function<void()> openCCCb,
                       std::function<void()> openMidiFxCb,
                       std::function<void()> closeCb = {})
      : slot(s), chainIndex(chainIdx), isFm(isFmSynth),
        onOpenNoteRange(std::move(openNoteRangeCb)),
        onOpenCCDialog(std::move(openCCCb)),
        onOpenMidiEffects(std::move(openMidiFxCb)),
        onClose(std::move(closeCb)) {

    // --- Card 1: Note Range & Splits ---
    openNoteRangeBtn.setButtonText("OPEN NOTE RANGE DIALOG...");
    Fanfare::setButtonRole(openNoteRangeBtn, Fanfare::ButtonRole::Primary);
    openNoteRangeBtn.onClick = [this] {
      auto cb = onOpenNoteRange;
      dismissParentCallout();
      if (cb)
        cb();
    };
    addAndMakeVisible(openNoteRangeBtn);

    // --- Card 2: CC Assignments ---
    openCCBtn.setButtonText("OPEN CC ASSIGNMENTS...");
    Fanfare::setButtonRole(openCCBtn, Fanfare::ButtonRole::Standard);
    openCCBtn.onClick = [this] {
      auto cb = onOpenCCDialog;
      dismissParentCallout();
      if (cb)
        cb();
    };
    addAndMakeVisible(openCCBtn);

    // --- Card 3: MIDI Transformer / Arpeggiator ---
    openMidiFxBtn.setButtonText("OPEN MIDI TRANSFORMER...");
    Fanfare::setButtonRole(openMidiFxBtn, Fanfare::ButtonRole::Standard);
    openMidiFxBtn.onClick = [this] {
      auto cb = onOpenMidiEffects;
      dismissParentCallout();
      if (cb)
        cb();
    };
    addAndMakeVisible(openMidiFxBtn);

    // --- Close Button ---
    closeBtn.setButtonText("CLOSE");
    Fanfare::setButtonRole(closeBtn, Fanfare::ButtonRole::Standard);
    closeBtn.onClick = [this] {
      dismissParentCallout();
      if (onClose)
        onClose();
    };
    addAndMakeVisible(closeBtn);

    setSize(520, 440);
  }

  void resized() override {
    auto r = getLocalBounds().reduced(12, 10);
    r.removeFromTop(34); // Header title & subtitle
    r.removeFromTop(8);

    auto bottom = r.removeFromBottom(28);
    closeBtn.setBounds(bottom.removeFromRight(80));
    r.removeFromBottom(8);

    int cardH = r.getHeight() / 3;
    card1Area = r.removeFromTop(cardH);
    r.removeFromTop(6);
    card2Area = r.removeFromTop(cardH);
    r.removeFromTop(6);
    card3Area = r;

    // Position action buttons inside each card
    openNoteRangeBtn.setBounds(card1Area.removeFromRight(190).reduced(8, 20));
    openCCBtn.setBounds(card2Area.removeFromRight(190).reduced(8, 20));
    openMidiFxBtn.setBounds(card3Area.removeFromRight(190).reduced(8, 20));
  }

  void paint(juce::Graphics &g) override {
    auto bounds = getLocalBounds().toFloat();

    // Dialog background
    g.setColour(ThemeManager::get(Theme::Role::panel));
    g.fillRoundedRectangle(bounds, 4.0f);

    // Header bar
    auto headerRect = bounds.removeFromTop(34.0f);
    g.setColour(ThemeManager::get(Theme::Role::sunken));
    g.fillRect(headerRect);
    g.setColour(ThemeManager::get(Theme::Role::hairline));
    g.drawHorizontalLine(34, 0.0f, bounds.getWidth());

    // Title
    g.setColour(ThemeManager::get(Theme::Role::accent));
    g.setFont(BoutiqueLookAndFeel::condensedFont(15.0f, 0.05f));
    g.drawText("MIDI ROUTING & CONTROLS", 14, 2, 240, 18, juce::Justification::centredLeft);

    // Subtitle
    const juce::String synthName = isFm ? "CoplanDX" : "BLEEP-216";
    const juce::String sub = "Strip: " + slot.getName() + "  |  " + synthName +
                             " [Socket " + juce::String(chainIndex + 1) + "]";
    g.setColour(ThemeManager::get(Theme::Role::textDim));
    g.setFont(BoutiqueLookAndFeel::monoFont(10.5f, false));
    g.drawText(sub, 14, 18, getWidth() - 28, 14, juce::Justification::centredLeft);

    // Frame border
    g.setColour(ThemeManager::get(Theme::Role::borderStrong));
    g.drawRoundedRectangle(getLocalBounds().toFloat().reduced(0.5f), 4.0f, 1.0f);

    // Draw the 3 cards
    paintCard1(g, card1Area.toFloat());
    paintCard2(g, card2Area.toFloat());
    paintCard3(g, card3Area.toFloat());
  }

private:
  RackSlot &slot;
  int chainIndex;
  bool isFm;

  std::function<void()> onOpenNoteRange;
  std::function<void()> onOpenCCDialog;
  std::function<void()> onOpenMidiEffects;
  std::function<void()> onClose;

  juce::TextButton openNoteRangeBtn;
  juce::TextButton openCCBtn;
  juce::TextButton openMidiFxBtn;
  juce::TextButton closeBtn;

  juce::Rectangle<int> card1Area;
  juce::Rectangle<int> card2Area;
  juce::Rectangle<int> card3Area;

  void dismissParentCallout() {
    if (auto *co = findParentComponentOfClass<juce::CallOutBox>())
      co->dismiss();
  }

  static juce::String noteName(int note) {
    static const char *names[] = {"C",  "C#", "D",  "D#", "E",  "F",
                                  "F#", "G",  "G#", "A",  "A#", "B"};
    return juce::String(names[note % 12]) + juce::String(note / 12 - 1);
  }

  static juce::String rangeStr(int low, int high) {
    if (low <= 0 && high >= 127)
      return "Full (C-1 - G9)";
    return noteName(low) + " - " + noteName(high);
  }

  void paintCard1(juce::Graphics &g, juce::Rectangle<float> area) {
    g.setColour(ThemeManager::get(Theme::Role::panelAlt));
    g.fillRoundedRectangle(area, 3.0f);
    g.setColour(ThemeManager::get(Theme::Role::hairline));
    g.drawRoundedRectangle(area.reduced(0.5f), 3.0f, 1.0f);

    // Left color accent bar
    g.setColour(ThemeManager::get(Theme::Role::accent));
    g.fillRoundedRectangle(area.getX() + 3.0f, area.getY() + 4.0f, 3.0f, area.getHeight() - 8.0f, 1.5f);

    auto textR = area.reduced(14.0f, 8.0f);
    textR.setWidth(textR.getWidth() - 190.0f);

    // Title
    g.setColour(ThemeManager::get(Theme::Role::text));
    g.setFont(BoutiqueLookAndFeel::condensedFont(13.5f, 0.05f));
    g.drawText("KEYBOARD NOTE RANGES & SPLITS", textR.removeFromTop(18.0f), juce::Justification::centredLeft);

    // Description
    g.setColour(ThemeManager::get(Theme::Role::textDim));
    g.setFont(BoutiqueLookAndFeel::uiFont(11.0f, false));
    g.drawText("Independent key ranges & splits for all 3 rack sockets.", textR.removeFromTop(16.0f), juce::Justification::centredLeft);

    // Live Readout per socket
    textR.removeFromTop(2.0f);
    g.setFont(BoutiqueLookAndFeel::monoFont(10.0f, true));
    for (int i = 0; i < 3; ++i) {
      const auto &cs = slot.getChainSlotSettings(i);
      juce::String name = slot.getPluginName(i);
      if (name.isEmpty()) {
        if (slot.isChainSlotMidiOut(i)) {
          auto dev = slot.getChainSlotMidiOutName(i);
          name = "MIDI: " + (dev.isNotEmpty() ? dev : ("Port " + juce::String(i + 1)));
        } else {
          name = "Socket " + juce::String(i + 1);
        }
      }
      if (name.length() > 18)
        name = name.substring(0, 16) + "..";

      const juce::String line = "S" + juce::String(i + 1) + " [" + name + "]: " +
                                rangeStr(cs.lowNote.load(), cs.highNote.load());
      g.setColour(i == chainIndex ? ThemeManager::get(Theme::Role::accent)
                                  : ThemeManager::get(Theme::Role::textFaint));
      g.drawText(line, textR.removeFromTop(13.0f), juce::Justification::centredLeft);
    }
  }

  void paintCard2(juce::Graphics &g, juce::Rectangle<float> area) {
    g.setColour(ThemeManager::get(Theme::Role::panelAlt));
    g.fillRoundedRectangle(area, 3.0f);
    g.setColour(ThemeManager::get(Theme::Role::hairline));
    g.drawRoundedRectangle(area.reduced(0.5f), 3.0f, 1.0f);

    // Left color accent bar
    g.setColour(ThemeManager::get(Theme::Role::ok));
    g.fillRoundedRectangle(area.getX() + 3.0f, area.getY() + 4.0f, 3.0f, area.getHeight() - 8.0f, 1.5f);

    auto textR = area.reduced(14.0f, 8.0f);
    textR.setWidth(textR.getWidth() - 190.0f);

    // Title
    g.setColour(ThemeManager::get(Theme::Role::text));
    g.setFont(BoutiqueLookAndFeel::condensedFont(13.5f, 0.05f));
    g.drawText("MIDI CC ASSIGNMENTS & LEARN", textR.removeFromTop(18.0f), juce::Justification::centredLeft);

    // Description
    g.setColour(ThemeManager::get(Theme::Role::textDim));
    g.setFont(BoutiqueLookAndFeel::uiFont(11.0f, false));
    g.drawText("Map hardware faders & knobs with Learn & filtering.", textR.removeFromTop(16.0f), juce::Justification::centredLeft);

    // Readout
    textR.removeFromTop(4.0f);
    g.setFont(BoutiqueLookAndFeel::monoFont(10.5f, true));
    g.setColour(ThemeManager::get(Theme::Role::textFaint));

    const int foh = slot.getFohCC();
    const int iem = slot.getIemCC();
    const int mute = slot.getMuteCC();
    const auto &mappings = slot.getCCMappings();

    juce::String fohStr = foh >= 0 ? ("CC " + juce::String(foh)) : "Unmapped";
    juce::String iemStr = iem >= 0 ? ("CC " + juce::String(iem)) : "Unmapped";
    juce::String muteStr = mute >= 0 ? ("CC " + juce::String(mute)) : "Unmapped";

    g.drawText("FOH Vol: " + fohStr + "  |  IEM Vol: " + iemStr, textR.removeFromTop(14.0f), juce::Justification::centredLeft);
    g.drawText("Mute: " + muteStr + "  |  Params: " + juce::String((int)mappings.size()) + " mapped",
               textR.removeFromTop(14.0f), juce::Justification::centredLeft);
  }

  void paintCard3(juce::Graphics &g, juce::Rectangle<float> area) {
    g.setColour(ThemeManager::get(Theme::Role::panelAlt));
    g.fillRoundedRectangle(area, 3.0f);
    g.setColour(ThemeManager::get(Theme::Role::hairline));
    g.drawRoundedRectangle(area.reduced(0.5f), 3.0f, 1.0f);

    // Left color accent bar
    g.setColour(ThemeManager::get(Theme::Role::warn));
    g.fillRoundedRectangle(area.getX() + 3.0f, area.getY() + 4.0f, 3.0f, area.getHeight() - 8.0f, 1.5f);

    auto textR = area.reduced(14.0f, 8.0f);
    textR.setWidth(textR.getWidth() - 190.0f);

    // Title
    g.setColour(ThemeManager::get(Theme::Role::text));
    g.setFont(BoutiqueLookAndFeel::condensedFont(13.5f, 0.05f));
    g.drawText("MIDI TRANSFORMER & ARPEGGIATOR", textR.removeFromTop(18.0f), juce::Justification::centredLeft);

    // Description
    g.setColour(ThemeManager::get(Theme::Role::textDim));
    g.setFont(BoutiqueLookAndFeel::uiFont(11.0f, false));
    g.drawText("Pattern arpeggiator, chord harmonizer & transposition.", textR.removeFromTop(16.0f), juce::Justification::centredLeft);

    // Readout
    textR.removeFromTop(4.0f);
    g.setFont(BoutiqueLookAndFeel::monoFont(10.5f, true));
    g.setColour(ThemeManager::get(Theme::Role::textFaint));

    const bool arpOn = slot.getArpeggiator().enabled.load();
    const int transpose = slot.getTransposeSemis();
    const bool harmOn = slot.getHarmonizer().enabled.load();

    const juce::String arpStr = arpOn ? "RUNNING" : "Bypassed";
    const juce::String trspStr = transpose != 0 ? (juce::String(transpose) + " semis") : "0 (No Trsp)";
    const juce::String harmStr = harmOn ? "ACTIVE" : "Bypassed";

    g.drawText("Arp: " + arpStr + "  |  Harmonizer: " + harmStr, textR.removeFromTop(14.0f), juce::Justification::centredLeft);
    g.drawText("Transpose: " + trspStr, textR.removeFromTop(14.0f), juce::Justification::centredLeft);
  }

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SynthMidiSetupDialog)
};
