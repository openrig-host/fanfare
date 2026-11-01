#pragma once

#include <JuceHeader.h>
#include "RackSlot.h"
#include "BoutiqueLookAndFeel.h"
#include "ButtonRoles.h"
#include "HelpLauncher.h"
#include "ThemeManager.h"
#include "IMidiNoteLearner.h"

/**
 * A component that displays a piano keyboard and allows selecting a MIDI note
 * range using two draggable handles. Includes a "Learn" mode to capture range
 * from incoming MIDI notes.
 */
class NoteRangeComponent : public juce::Component, public juce::Timer, public IMidiNoteLearner {
public:
  NoteRangeComponent(RackSlot &s) : slot(s) {
    lowNote = slot.getLowNote();
    highNote = slot.getHighNote();

    // Start in learn mode automatically on open
    learning = true;
    learnedLow = -1;
    learnedHigh = -1;
    startTimerHz(30);

    learnButton.setButtonText("DONE");
    Fanfare::setButtonRole(learnButton, Fanfare::ButtonRole::Primary);
    learnButton.onClick = [this] {
      learning = false;
      if (auto* co = findParentComponentOfClass<juce::CallOutBox>()) {
        co->dismiss();
      }
    };
    addAndMakeVisible(learnButton);

    resetButton.setButtonText("RESET");
    Fanfare::setButtonRole(resetButton, Fanfare::ButtonRole::Danger);
    resetButton.onClick = [this] {
      learnedLow = -1;
      learnedHigh = -1;
      lowNote = 0;
      highNote = 127;
      slot.setNoteRange(lowNote, highNote);
      repaint();
    };
    addAndMakeVisible(resetButton);
    Fanfare::styleHelpButton(helpButton, "#note-range", "HAAALP! Open the Note Range & Keyboard Split section of the User's Manual");
    addAndMakeVisible(helpButton);
  }

  ~NoteRangeComponent() override { stopTimer(); }

  void resized() override {
    auto r = getLocalBounds();
    auto buttonArea = r.removeFromBottom(32).reduced(10, 3);
    learnButton.setBounds(buttonArea.removeFromLeft(80));
    buttonArea.removeFromLeft(8);
    resetButton.setBounds(buttonArea.removeFromLeft(80));
    helpButton.setBounds(buttonArea.removeFromRight(76));
  }

  void paint(juce::Graphics &g) override {
    auto bounds = getLocalBounds().toFloat();

    g.setColour(ThemeManager::get(Theme::Role::panel));
    g.fillRoundedRectangle(bounds, 3.0f);

    // Header strip
    auto header = bounds.removeFromTop(28.0f);
    g.setColour(ThemeManager::get(Theme::Role::sunken));
    g.fillRect(header);
    g.setColour(ThemeManager::get(Theme::Role::hairline));
    g.drawHorizontalLine((int)header.getBottom(), bounds.getX(), bounds.getRight());

    g.setColour(ThemeManager::get(Theme::Role::borderStrong));
    g.drawRoundedRectangle(getLocalBounds().toFloat().reduced(0.5f), 3.0f, 1.0f);

    // Keyboard area (above the buttons)
    auto keyboardArea = getLocalBounds().toFloat().reduced(10.0f, 5.0f);
    keyboardArea.removeFromBottom(34.0f);
    keyboardArea.removeFromTop(28.0f);

    Fanfare::ModalFrame::paintRecessedWell(g, keyboardArea.expanded(1.0f), 2.0f);
    drawKeyboard(g, keyboardArea);
    drawRangeOverlay(g, keyboardArea);

    // Engraved range readouts
    g.setColour(ThemeManager::get(Theme::Role::engrave));
    g.setFont(juce::FontOptions(juce::Font::getDefaultMonospacedFontName(), 12.0f, juce::Font::bold));
    g.drawText("LOW: " + midiNoteToName(lowNote), 12, 4, 110, 20, juce::Justification::centredLeft);
    g.drawText("HIGH: " + midiNoteToName(highNote), getWidth() - 122, 4, 110, 20, juce::Justification::centredRight);

    g.setColour(ThemeManager::get(Theme::Role::foh));
    g.setFont(juce::FontOptions("Barlow Condensed", 14.5f, juce::Font::bold));
    g.drawText("PLAY NOTES OR DRAG KEYBOARD", 0, 4, getWidth(), 20, juce::Justification::centred);
  }

  void mouseDown(const juce::MouseEvent &e) override {
    learnedLow = -1;
    learnedHigh = -1;
    updateNoteFromMouse(e.position);
  }

  void mouseDrag(const juce::MouseEvent &e) override {
    learnedLow = -1;
    learnedHigh = -1;
    updateNoteFromMouse(e.position);
  }

  void handleMidiNote(int noteNumber) override {
    if (!learning)
      return;

    if (learnedLow < 0) {
      learnedLow = noteNumber;
      learnedHigh = noteNumber;
    } else {
      learnedLow = std::min(learnedLow, noteNumber);
      learnedHigh = std::max(learnedHigh, noteNumber);
    }

    lowNote = learnedLow;
    highNote = learnedHigh;
    slot.setNoteRange(lowNote, highNote);
    repaint();
  }

  bool isLearning() const override { return learning; }

private:
  RackSlot &slot;
  int lowNote = 0;
  int highNote = 127;

  juce::TextButton learnButton;
  juce::TextButton resetButton;
  juce::TextButton helpButton{"HAAALP!"};

  bool learning = false;
  int learnedLow = -1;
  int learnedHigh = -1;

  void timerCallback() override { repaint(); }

  void drawKeyboard(juce::Graphics &g, juce::Rectangle<float> area) {
    float noteWidth = area.getWidth() / 128.0f;
    const juce::Colour ivory = ThemeManager::get(Theme::Role::vuFace);
    const juce::Colour ebony(0xFF1B1F22);

    for (int i = 0; i < 128; ++i) {
      bool isBlack = isBlackKey(i);
      juce::Rectangle<float> noteRect(area.getX() + i * noteWidth, area.getY(),
                                      noteWidth, area.getHeight());

      if (!isBlack) {
        g.setColour(ivory);
        g.fillRect(noteRect.reduced(0.4f));

        if (i % 12 == 0) {
          g.setColour(ebony.withAlpha(0.65f));
          g.setFont(juce::FontOptions("Barlow Condensed", 9.5f, juce::Font::bold));
          g.drawText("C" + juce::String(i / 12 - 1), noteRect,
                     juce::Justification::centredBottom);
        }
      }
    }

    for (int i = 0; i < 128; ++i) {
      if (isBlackKey(i)) {
        juce::Rectangle<float> noteRect(area.getX() + i * noteWidth,
                                        area.getY(), noteWidth,
                                        area.getHeight() * 0.6f);
        g.setColour(ebony);
        g.fillRect(noteRect);
      }
    }
  }

  void drawRangeOverlay(juce::Graphics &g, juce::Rectangle<float> area) {
    float noteWidth = area.getWidth() / 128.0f;
    float x1 = area.getX() + lowNote * noteWidth;
    float x2 = area.getX() + (highNote + 1) * noteWidth;

    g.setColour(juce::Colour(0x99111417));
    g.fillRect(area.withWidth(x1 - area.getX()));
    g.fillRect(area.withX(x2).withWidth(area.getRight() - x2));

    const auto fohCol = ThemeManager::get(Theme::Role::foh);
    g.setColour(fohCol.withAlpha(0.36f));
    g.fillRect(juce::Rectangle<float>(x1, area.getY(), x2 - x1, area.getHeight()));

    g.setColour(fohCol);
    g.drawVerticalLine((int)x1, area.getY() - 3.0f, area.getBottom() + 3.0f);
    g.drawVerticalLine((int)x2, area.getY() - 3.0f, area.getBottom() + 3.0f);
  }

  bool isBlackKey(int note) {
    int n = note % 12;
    return (n == 1 || n == 3 || n == 6 || n == 8 || n == 10);
  }

  juce::String midiNoteToName(int note) {
    static const char *names[] = {"C",  "C#", "D",  "D#", "E",  "F",
                                  "F#", "G",  "G#", "A",  "A#", "B"};
    return juce::String(names[note % 12]) + juce::String(note / 12 - 1);
  }

  void updateNoteFromMouse(juce::Point<float> p) {
    auto area = getLocalBounds().toFloat().reduced(10.0f, 5.0f);
    area.removeFromBottom(34.0f);
    area.removeFromTop(28.0f);

    if (!area.contains(p))
      return;

    float normalizedX = (p.getX() - area.getX()) / area.getWidth();
    int note = juce::jlimit(0, 127, (int)(normalizedX * 128.0f));

    if (std::abs(note - lowNote) < std::abs(note - highNote)) {
      lowNote = note;
    } else {
      highNote = note;
    }

    int actualLow = std::min(lowNote, highNote);
    int actualHigh = std::max(lowNote, highNote);

    slot.setNoteRange(actualLow, actualHigh);
    repaint();
  }

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(NoteRangeComponent)
};
