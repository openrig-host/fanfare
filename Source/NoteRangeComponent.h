#pragma once

#include <JuceHeader.h>
#include "RackSlot.h"
#include "BoutiqueLookAndFeel.h"
#include "ButtonRoles.h"
#include "HelpLauncher.h"
#include "ThemeManager.h"
#include "IMidiNoteLearner.h"

/**
 * NoteRangeComponent: Displays 3 independent note ranges, one for each instrument
 * rack socket (Socket 1, Socket 2, Socket 3).
 *
 * Each socket features:
 *  - Instrument name and enable/mute toggle
 *  - High-precision 128-key piano keyboard with octave guide markers
 *  - Draggable range handles with color-coded active highlights
 *  - Individual MIDI Note Learn with arming and reset controls
 *  - Automatic synchronization of the overarching strip note filter
 */
class NoteRangeComponent : public juce::Component,
                           public juce::Timer,
                           public IMidiNoteLearner {
public:
  explicit NoteRangeComponent(RackSlot &s) : slot(s) {
    // Determine default active socket for learn (prefer first occupied socket)
    activeSocket = 0;
    for (int i = 0; i < 3; ++i) {
      if (slot.isChainSlotSynth(i) || slot.isChainSlotFm(i) ||
          slot.isChainSlotMidiOut(i) || (slot.getPluginInstance(i) != nullptr)) {
        activeSocket = i;
        break;
      }
    }

    // Read initial socket ranges and setup per-socket controls
    for (int i = 0; i < 3; ++i) {
      const auto &cs = slot.getChainSlotSettings(i);
      lowNote[i] = juce::jlimit(0, 127, cs.lowNote.load());
      highNote[i] = juce::jlimit(0, 127, cs.highNote.load());
      if (highNote[i] < lowNote[i])
        highNote[i] = lowNote[i];

      const bool en = cs.enabled.load();

      // Enable / Mute toggle
      enableBtns[i].setClickingTogglesState(true);
      enableBtns[i].setToggleState(en, juce::dontSendNotification);
      updateEnableButtonText(i);
      enableBtns[i].onClick = [this, i] {
        const bool on = enableBtns[i].getToggleState();
        slot.getChainSlotSettings(i).enabled.store(on);
        updateEnableButtonText(i);
        syncOverallSlotRange();
        repaint();
      };
      addAndMakeVisible(enableBtns[i]);

      // Learn button
      learnBtns[i].onClick = [this, i] {
        activeSocket = i;
        learning = true;
        learnedLow = -1;
        learnedHigh = -1;
        updateLearnButtons();
        repaint();
      };
      addAndMakeVisible(learnBtns[i]);

      // Reset button
      resetBtns[i].setButtonText("RESET");
      Fanfare::setButtonRole(resetBtns[i], Fanfare::ButtonRole::Danger);
      resetBtns[i].setTooltip("Reset Socket " + juce::String(i + 1) + " to full range (C-1 - G9)");
      resetBtns[i].onClick = [this, i] {
        lowNote[i] = 0;
        highNote[i] = 127;
        slot.getChainSlotSettings(i).lowNote.store(0);
        slot.getChainSlotSettings(i).highNote.store(127);
        if (activeSocket == i) {
          learnedLow = -1;
          learnedHigh = -1;
        }
        syncOverallSlotRange();
        repaint();
      };
      addAndMakeVisible(resetBtns[i]);
    }

    updateLearnButtons();

    // Global Done button (dismisses CallOutBox)
    doneButton.setButtonText("DONE");
    Fanfare::setButtonRole(doneButton, Fanfare::ButtonRole::Primary);
    doneButton.onClick = [this] {
      learning = false;
      if (auto *co = findParentComponentOfClass<juce::CallOutBox>())
        co->dismiss();
    };
    addAndMakeVisible(doneButton);

    // Global Reset All button
    resetAllButton.setButtonText("RESET ALL");
    Fanfare::setButtonRole(resetAllButton, Fanfare::ButtonRole::Danger);
    resetAllButton.setTooltip("Reset all 3 sockets to full keyboard range (C-1 - G9)");
    resetAllButton.onClick = [this] {
      learnedLow = -1;
      learnedHigh = -1;
      for (int i = 0; i < 3; ++i) {
        lowNote[i] = 0;
        highNote[i] = 127;
        slot.getChainSlotSettings(i).lowNote.store(0);
        slot.getChainSlotSettings(i).highNote.store(127);
      }
      syncOverallSlotRange();
      repaint();
    };
    addAndMakeVisible(resetAllButton);

    // Help launcher
    Fanfare::styleHelpButton(helpButton, "#note-range",
                            "Open the Note Range & Keyboard Split section of the User's Manual");
    addAndMakeVisible(helpButton);

    startTimerHz(25);
    setSize(860, 310);
  }

  ~NoteRangeComponent() override { stopTimer(); }

  void resized() override {
    auto r = getLocalBounds().reduced(10, 6);

    // Header area reserved at top (26px)
    headerArea = r.removeFromTop(26);
    r.removeFromTop(4);

    // Bottom action bar (28px)
    auto bottomArea = r.removeFromBottom(28);
    doneButton.setBounds(bottomArea.removeFromLeft(80));
    bottomArea.removeFromLeft(8);
    resetAllButton.setBounds(bottomArea.removeFromLeft(96));
    helpButton.setBounds(bottomArea.removeFromRight(76));
    r.removeFromBottom(4);

    // 3 Socket lanes
    const int totalAvailable = r.getHeight();
    const int gap = 6;
    const int laneH = (totalAvailable - (gap * 2)) / 3;

    for (int i = 0; i < 3; ++i) {
      auto lane = r.removeFromTop(laneH);
      if (i < 2)
        r.removeFromTop(gap);

      auto headerRow = lane.removeFromTop(22);
      socketHeaderAreas[i] = headerRow;

      // Socket enable/mute button on left
      enableBtns[i].setBounds(headerRow.removeFromLeft(52).reduced(0, 1));
      headerRow.removeFromLeft(6);

      // Buttons on right
      resetBtns[i].setBounds(headerRow.removeFromRight(58).reduced(0, 1));
      headerRow.removeFromRight(6);
      learnBtns[i].setBounds(headerRow.removeFromRight(70).reduced(0, 1));

      // Remaining lane area is the keyboard lane
      lane.removeFromTop(2);
      keyboardAreas[i] = lane.toFloat();
    }
  }

  void paint(juce::Graphics &g) override {
    auto bounds = getLocalBounds().toFloat();

    // Dialog background
    g.setColour(ThemeManager::get(Theme::Role::panel));
    g.fillRoundedRectangle(bounds, 4.0f);

    // Top dialog header bar
    auto headR = bounds.removeFromTop(32.0f);
    g.setColour(ThemeManager::get(Theme::Role::sunken));
    g.fillRect(headR);
    g.setColour(ThemeManager::get(Theme::Role::hairline));
    g.drawHorizontalLine((int)headR.getBottom(), bounds.getX(), bounds.getRight());

    // Dialog Title
    g.setColour(ThemeManager::get(Theme::Role::text));
    g.setFont(BoutiqueLookAndFeel::condensedFont(14.5f, 0.05f));
    g.drawText("KEYBOARD SPLITS & NOTE RANGES", 14, 5, 230, 20, juce::Justification::centredLeft);

    // Strip name pill
    g.setColour(ThemeManager::get(Theme::Role::accent));
    g.setFont(BoutiqueLookAndFeel::monoFont(11.5f, true));
    g.drawText("[STRIP: " + slot.getName() + "]", 244, 6, 200, 18, juce::Justification::centredLeft);

    // Subtitle instruction
    g.setColour(ThemeManager::get(Theme::Role::textDim));
    g.setFont(BoutiqueLookAndFeel::uiFont(11.0f, false));
    g.drawText("Click/drag handles or play MIDI keys to set ranges",
               getWidth() - 320, 5, 306, 20, juce::Justification::centredRight);

    // Frame border
    g.setColour(ThemeManager::get(Theme::Role::borderStrong));
    g.drawRoundedRectangle(getLocalBounds().toFloat().reduced(0.5f), 4.0f, 1.0f);

    // Paint each of the 3 socket rows
    for (int i = 0; i < 3; ++i) {
      paintSocketRow(g, i);
    }
  }

  void mouseDown(const juce::MouseEvent &e) override {
    dragSocket = -1;
    const auto p = e.position;

    for (int i = 0; i < 3; ++i) {
      if (keyboardAreas[i].contains(p)) {
        activeSocket = i;
        learnedLow = -1;
        learnedHigh = -1;
        dragSocket = i;

        const float noteW = keyboardAreas[i].getWidth() / 128.0f;
        const float norm = (p.getX() - keyboardAreas[i].getX()) / keyboardAreas[i].getWidth();
        const int note = juce::jlimit(0, 127, (int)(norm * 128.0f));

        const float lowX = keyboardAreas[i].getX() + lowNote[i] * noteW;
        const float highX = keyboardAreas[i].getX() + (highNote[i] + 1) * noteW;

        if (std::abs(p.getX() - lowX) <= std::abs(p.getX() - highX)) {
          dragIsLow = true;
          lowNote[i] = juce::jmin(note, highNote[i]);
        } else {
          dragIsLow = false;
          highNote[i] = juce::jmax(note, lowNote[i]);
        }

        slot.getChainSlotSettings(i).lowNote.store(lowNote[i]);
        slot.getChainSlotSettings(i).highNote.store(highNote[i]);
        syncOverallSlotRange();
        updateLearnButtons();
        repaint();
        break;
      }
    }
  }

  void mouseDrag(const juce::MouseEvent &e) override {
    if (dragSocket >= 0 && dragSocket < 3) {
      const int i = dragSocket;
      const float norm = (e.position.getX() - keyboardAreas[i].getX()) / keyboardAreas[i].getWidth();
      const int note = juce::jlimit(0, 127, (int)(norm * 128.0f));

      if (dragIsLow) {
        lowNote[i] = juce::jmin(note, highNote[i]);
      } else {
        highNote[i] = juce::jmax(note, lowNote[i]);
      }

      slot.getChainSlotSettings(i).lowNote.store(lowNote[i]);
      slot.getChainSlotSettings(i).highNote.store(highNote[i]);
      syncOverallSlotRange();
      repaint();
    }
  }

  void mouseUp(const juce::MouseEvent &) override {
    dragSocket = -1;
  }

  void mouseMove(const juce::MouseEvent &e) override {
    bool overHandle = false;
    for (int i = 0; i < 3; ++i) {
      if (keyboardAreas[i].contains(e.position)) {
        const float noteW = keyboardAreas[i].getWidth() / 128.0f;
        const float lowX = keyboardAreas[i].getX() + lowNote[i] * noteW;
        const float highX = keyboardAreas[i].getX() + (highNote[i] + 1) * noteW;
        if (std::abs(e.position.getX() - lowX) < 6.0f || std::abs(e.position.getX() - highX) < 6.0f) {
          overHandle = true;
          break;
        }
      }
    }
    setMouseCursor(overHandle ? juce::MouseCursor::LeftRightResizeCursor
                              : juce::MouseCursor::NormalCursor);
  }

  // --- IMidiNoteLearner Interface ---
  void handleMidiNote(int noteNumber) override {
    if (!learning || activeSocket < 0 || activeSocket >= 3)
      return;

    if (learnedLow < 0) {
      learnedLow = noteNumber;
      learnedHigh = noteNumber;
    } else {
      learnedLow = std::min(learnedLow, noteNumber);
      learnedHigh = std::max(learnedHigh, noteNumber);
    }

    lowNote[activeSocket] = learnedLow;
    highNote[activeSocket] = learnedHigh;
    slot.getChainSlotSettings(activeSocket).lowNote.store(learnedLow);
    slot.getChainSlotSettings(activeSocket).highNote.store(learnedHigh);

    syncOverallSlotRange();
    repaint();
  }

  bool isLearning() const override { return learning; }

private:
  RackSlot &slot;

  int lowNote[3] = {0, 0, 0};
  int highNote[3] = {127, 127, 127};

  int activeSocket = 0;
  bool learning = true;
  int learnedLow = -1;
  int learnedHigh = -1;

  int dragSocket = -1;
  bool dragIsLow = false;

  juce::Rectangle<int> headerArea;
  juce::Rectangle<int> socketHeaderAreas[3];
  juce::Rectangle<float> keyboardAreas[3];

  juce::TextButton enableBtns[3];
  juce::TextButton learnBtns[3];
  juce::TextButton resetBtns[3];

  juce::TextButton doneButton;
  juce::TextButton resetAllButton;
  juce::TextButton helpButton{"HAAALP!"};

  void timerCallback() override {
    // Periodically refresh in case slot settings change externally
    bool changed = false;
    for (int i = 0; i < 3; ++i) {
      const auto &cs = slot.getChainSlotSettings(i);
      const int lo = cs.lowNote.load();
      const int hi = cs.highNote.load();
      const bool en = cs.enabled.load();
      if (lo != lowNote[i] || hi != highNote[i]) {
        lowNote[i] = lo;
        highNote[i] = hi;
        changed = true;
      }
      if (en != enableBtns[i].getToggleState()) {
        enableBtns[i].setToggleState(en, juce::dontSendNotification);
        updateEnableButtonText(i);
        changed = true;
      }
    }
    if (changed)
      repaint();
  }

  juce::Colour getSocketColor(int idx) const {
    switch (idx) {
      case 0: return ThemeManager::get(Theme::Role::accent);     // Cyan / Gold
      case 1: return ThemeManager::get(Theme::Role::ok);         // Emerald Green
      case 2: return ThemeManager::get(Theme::Role::warn);       // Amber / Orange
      default: return ThemeManager::get(Theme::Role::accent);
    }
  }

  juce::String getSocketDisplayName(int idx) const {
    juce::String name = slot.getPluginName(idx);
    if (name.isNotEmpty())
      return name;
    if (slot.isChainSlotMidiOut(idx)) {
      juce::String dev = slot.getChainSlotMidiOutName(idx);
      return "MIDI Out: " + (dev.isNotEmpty() ? dev : ("Port " + juce::String(idx + 1)));
    }
    return "(Empty Socket " + juce::String(idx + 1) + ")";
  }

  static juce::String midiNoteToName(int note) {
    static const char *names[] = {"C",  "C#", "D",  "D#", "E",  "F",
                                  "F#", "G",  "G#", "A",  "A#", "B"};
    return juce::String(names[note % 12]) + juce::String(note / 12 - 1);
  }

  static bool isBlackKey(int note) {
    const int n = note % 12;
    return (n == 1 || n == 3 || n == 6 || n == 8 || n == 10);
  }

  void updateEnableButtonText(int i) {
    const bool on = enableBtns[i].getToggleState();
    enableBtns[i].setButtonText(on ? "ON" : "MUTE");
    Fanfare::setButtonRole(enableBtns[i], on ? Fanfare::ButtonRole::Standard
                                            : Fanfare::ButtonRole::Mute);
  }

  void updateLearnButtons() {
    for (int i = 0; i < 3; ++i) {
      if (activeSocket == i && learning) {
        learnBtns[i].setButtonText("ARMED");
        Fanfare::setButtonRole(learnBtns[i], Fanfare::ButtonRole::Primary);
        learnBtns[i].setTooltip("Socket " + juce::String(i + 1) + " is armed for MIDI note learning. Play keys to set range.");
      } else {
        learnBtns[i].setButtonText("LEARN");
        Fanfare::setButtonRole(learnBtns[i], Fanfare::ButtonRole::Standard);
        learnBtns[i].setTooltip("Arm Socket " + juce::String(i + 1) + " for MIDI note learning");
      }
    }
  }

  void syncOverallSlotRange() {
    int minLow = 127;
    int maxHigh = 0;
    bool anyActive = false;

    for (int i = 0; i < 3; ++i) {
      const auto &cs = slot.getChainSlotSettings(i);
      const bool hasInst = slot.isChainSlotSynth(i) || slot.isChainSlotFm(i) ||
                           slot.isChainSlotMidiOut(i) || (slot.getPluginInstance(i) != nullptr);
      if (hasInst && cs.enabled.load()) {
        minLow = std::min(minLow, cs.lowNote.load());
        maxHigh = std::max(maxHigh, cs.highNote.load());
        anyActive = true;
      }
    }

    if (!anyActive) {
      for (int i = 0; i < 3; ++i) {
        const auto &cs = slot.getChainSlotSettings(i);
        minLow = std::min(minLow, cs.lowNote.load());
        maxHigh = std::max(maxHigh, cs.highNote.load());
      }
    }

    if (minLow > maxHigh) {
      minLow = 0;
      maxHigh = 127;
    }

    slot.setNoteRange(minLow, maxHigh);
  }

  void paintSocketRow(juce::Graphics &g, int idx) {
    const auto sockCol = getSocketColor(idx);
    const bool isArmed = (activeSocket == idx && learning);

    // Socket header area
    auto headR = socketHeaderAreas[idx];
    const int badgeW = 20;
    auto badgeR = juce::Rectangle<int>(headR.getX() + 58, headR.getY(), badgeW, headR.getHeight() - 2);

    // Socket badge [ 1 / 2 / 3 ]
    g.setColour(sockCol.withAlpha(isArmed ? 0.35f : 0.18f));
    g.fillRoundedRectangle(badgeR.toFloat(), 2.0f);
    g.setColour(sockCol);
    g.drawRoundedRectangle(badgeR.toFloat(), 2.0f, 1.0f);
    g.setFont(BoutiqueLookAndFeel::monoFont(10.5f, true));
    g.drawText(juce::String(idx + 1), badgeR, juce::Justification::centred);

    // Socket instrument name
    const int nameX = badgeR.getRight() + 8;
    const int nameW = 230;
    juce::Rectangle<int> nameRect(nameX, headR.getY(), nameW, headR.getHeight());
    g.setColour(ThemeManager::get(Theme::Role::text));
    g.setFont(BoutiqueLookAndFeel::condensedFont(13.0f, 0.05f));
    g.drawText(getSocketDisplayName(idx), nameRect, juce::Justification::centredLeft, true);

    // Note Range Readout (e.g. LOW: C1 (36)   HIGH: B3 (59))
    const int readoutX = nameRect.getRight() + 8;
    const int readoutW = 220;
    juce::Rectangle<int> readoutRect(readoutX, headR.getY(), readoutW, headR.getHeight());

    const juce::String loStr = midiNoteToName(lowNote[idx]) + " (" + juce::String(lowNote[idx]) + ")";
    const juce::String hiStr = midiNoteToName(highNote[idx]) + " (" + juce::String(highNote[idx]) + ")";
    const juce::String readout = "LOW: " + loStr + "   HIGH: " + hiStr;

    g.setColour(sockCol);
    g.setFont(BoutiqueLookAndFeel::monoFont(11.0f, true));
    g.drawText(readout, readoutRect, juce::Justification::centredLeft, false);

    // Armed indicator or hint
    if (isArmed) {
      const int hintX = readoutRect.getRight() + 4;
      const int hintW = juce::jmax(10, socketHeaderAreas[idx].getRight() - 146 - hintX);
      if (hintW > 40) {
        g.setColour(ThemeManager::get(Theme::Role::accent));
        g.setFont(BoutiqueLookAndFeel::condensedFont(10.5f, 0.04f));
        g.drawText("PLAY KEYS TO SET", hintX, headR.getY(), hintW, headR.getHeight(),
                   juce::Justification::centredRight, false);
      }
    }

    // Keyboard Well & Keys
    const auto kbArea = keyboardAreas[idx];
    Fanfare::ModalFrame::paintRecessedWell(g, kbArea.expanded(1.0f), 2.0f);
    drawKeyboard(g, kbArea);
    drawRangeOverlay(g, kbArea, idx);

    // Highlight border around armed lane
    if (isArmed) {
      g.setColour(sockCol.withAlpha(0.6f));
      g.drawRoundedRectangle(kbArea.expanded(1.0f), 2.0f, 1.0f);
    }
  }

  void drawKeyboard(juce::Graphics &g, juce::Rectangle<float> area) {
    const float noteWidth = area.getWidth() / 128.0f;
    const juce::Colour ivory = ThemeManager::get(Theme::Role::vuFace);
    const juce::Colour ebony(0xFF181B1E);

    // White keys
    for (int i = 0; i < 128; ++i) {
      if (!isBlackKey(i)) {
        juce::Rectangle<float> noteRect(area.getX() + i * noteWidth, area.getY(),
                                        noteWidth, area.getHeight());
        g.setColour(ivory);
        g.fillRect(noteRect.reduced(0.35f, 0.0f));

        // Octave C label
        if (i % 12 == 0) {
          g.setColour(ebony.withAlpha(0.60f));
          g.setFont(BoutiqueLookAndFeel::condensedFont(8.5f, 0.02f));
          g.drawText("C" + juce::String(i / 12 - 1), noteRect,
                     juce::Justification::centredBottom, false);
        }
      }
    }

    // Black keys
    for (int i = 0; i < 128; ++i) {
      if (isBlackKey(i)) {
        juce::Rectangle<float> noteRect(area.getX() + i * noteWidth, area.getY(),
                                        noteWidth, area.getHeight() * 0.58f);
        g.setColour(ebony);
        g.fillRect(noteRect);
      }
    }
  }

  void drawRangeOverlay(juce::Graphics &g, juce::Rectangle<float> area, int idx) {
    const float noteWidth = area.getWidth() / 128.0f;
    const float x1 = area.getX() + lowNote[idx] * noteWidth;
    const float x2 = area.getX() + (highNote[idx] + 1) * noteWidth;
    const auto sockCol = getSocketColor(idx);

    // Dimmed out-of-range regions
    g.setColour(juce::Colour(0x9E111317));
    if (x1 > area.getX())
      g.fillRect(area.withWidth(x1 - area.getX()));
    if (x2 < area.getRight())
      g.fillRect(area.withX(x2).withWidth(area.getRight() - x2));

    // Active range highlight
    g.setColour(sockCol.withAlpha(0.30f));
    g.fillRect(juce::Rectangle<float>(x1, area.getY(), x2 - x1, area.getHeight()));

    // Boundary handles
    g.setColour(sockCol);
    g.drawVerticalLine((int)x1, area.getY() - 1.0f, area.getBottom() + 1.0f);
    g.drawVerticalLine((int)x2, area.getY() - 1.0f, area.getBottom() + 1.0f);

    // Handle notches at top
    juce::Path lowCap;
    lowCap.addTriangle(x1, area.getY(), x1 + 4.0f, area.getY(), x1, area.getY() + 5.0f);
    g.fillPath(lowCap);

    juce::Path highCap;
    highCap.addTriangle(x2, area.getY(), x2 - 4.0f, area.getY(), x2, area.getY() + 5.0f);
    g.fillPath(highCap);
  }

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(NoteRangeComponent)
};
