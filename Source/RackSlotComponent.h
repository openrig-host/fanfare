#pragma once

#include "AssetLoader.h"
#include "ChannelStripProcessor.h"
#include "LEDIndicator.h"
#include "MidiLearnBus.h"
#include "FanfareConstants.h"
#include "RackSlot.h"
#include "BoutiqueLookAndFeel.h"
#include <JuceHeader.h>
#include <functional>

class RackSlotComponent : public juce::Component,
                          public juce::Timer,
                          public juce::DragAndDropTarget,
                          public juce::SettableTooltipClient {
public:
  RackSlotComponent(RackSlot &s, int index, juce::LookAndFeel &laf);
  ~RackSlotComponent() override;
  void lookAndFeelChanged() override;

  void timerCallback() override;
  void paint(juce::Graphics &g) override;
  void drawVerticalMeter(juce::Graphics &g, juce::Rectangle<int> area, float L,
                         float R, juce::Colour low, juce::Colour mid,
                         juce::Colour high);
  void resized() override;

  juce::ComboBox &getInputSelector();
  void setSpecialModes(bool monitorIn, bool accordion, bool returns);
  juce::Rectangle<int> getNoteRangeButtonBounds();

  std::function<void(int)> onShowPluginMenu;
  std::function<void(int)> onOpenEditor;
  std::function<void(int)> onEditMidiOut; // edit a chain slot's MIDI OUT destination
  std::function<void(int)> onShowCCDialog; // 0 = none, 1 = FOH, 2 = IEM, 3 = mute learn to arm
  std::function<void()> onShowNoteRangeDialog;
  std::function<void()> onShowChannelStrip;
  std::function<void()> onShowArpeggiator;
  std::function<void()> onShowSampler;
  std::function<void()> onShowMp3Player;
  std::function<void()> onShowSynth;
  std::function<void()> onSaveStrip;
  std::function<void()> onLoadStrip;
  std::function<void(const juce::File&)> onLoadStripFile;
  std::function<void(const juce::File&)> onLoadPresetFile;
  std::function<juce::String(int)> getSlotName;
  std::function<void()> onRename;
  void updateOutputSelector();

  bool isCollapsed = false;
  void setCollapsed(bool shouldCollapse);
  bool getIsCollapsed() const { return isCollapsed; }
  bool isUnusedEmptyStrip() const;
  std::function<void(bool)> onCollapseChanged;
  std::function<void(bool)> onBypassChanged;
  std::function<void(int deltaX)> onDragWidth;
  std::function<void()> onStartDragWidth;
  std::function<void(int deltaPx)> onHorizontalScroll;
  std::function<void()> onRightClickUnusedEmpty;
  bool isDraggingRightEdge = false;

  void mouseDoubleClick(const juce::MouseEvent &e) override;
  void mouseDown(const juce::MouseEvent &e) override;
  void mouseDrag(const juce::MouseEvent &e) override;
  void mouseUp(const juce::MouseEvent &e) override;
  void mouseMove(const juce::MouseEvent &e) override;
  void mouseEnter(const juce::MouseEvent &e) override;
  void mouseExit(const juce::MouseEvent &e) override;
  void mouseWheelMove(const juce::MouseEvent &e,
                      const juce::MouseWheelDetails &wheel) override;
  void showButtonHelpPopup(juce::Component *src);

  static const juce::Array<juce::Colour> &getPalette();
  void showColorPalette();
  void showRenameDialog();
  void showLoadStripMenu();
  void showStripActionsMenu();
  void showPluginConfigMenu(int chainIndex);
  juce::String getActiveNoteRangeString() const;

  bool isInterestedInDragSource(
      const juce::DragAndDropTarget::SourceDetails &details) override;
  void itemDropped(
      const juce::DragAndDropTarget::SourceDetails &details) override;

private:
  // Captures right-clicks on the three slot buttons to open the layer config.
  struct SlotBtnMouseListener : public juce::MouseListener {
    RackSlotComponent *owner = nullptr;
    void mouseDown(const juce::MouseEvent &e) override;
  } slotBtnMouseListener;

  // Routes clicks on the note-range Label to onShowNoteRangeDialog.
  struct NoteRangeLabelListener : public juce::MouseListener {
    RackSlotComponent *owner = nullptr;
    bool leftPressArmed = false;
    juce::int64 lastOpenMs = 0;
    void mouseDown(const juce::MouseEvent &e) override;
    void mouseUp(const juce::MouseEvent &e) override;
  } noteRangeLabelListener;

  // Long-press (600ms) on MUTE / FOH / IEM opens the CC assignment manager
  // with the matching learn armed; the release click is then suppressed.
  struct LongPressMouseListener : public juce::MouseListener,
                                  public juce::Timer {
    RackSlotComponent *owner = nullptr;
    int holdMs = 0;
    bool fired = false;
    int armTarget = 0; // 1 = FOH, 2 = IEM, 3 = mute
    void mouseDown(const juce::MouseEvent &e) override;
    void mouseDrag(const juce::MouseEvent &e) override;
    void mouseUp(const juce::MouseEvent &e) override;
    void timerCallback() override;
  } longPressListener;
  bool suppressLongPressClick = false;
  void longPressLearnArmed(int armTarget);

  RackSlot &slot;
  juce::TextButton slotBtns[3];
  juce::TextButton editGuiBtns[3];
  juce::TextButton bypassButton;
  juce::TextButton ccButton;
  juce::TextButton noteRangeButton;
  juce::TextButton customizeButton;
  juce::TextButton arpButton;
  juce::TextButton samplerButton;
  juce::TextButton mp3Button;
  juce::TextButton synthButton;
  juce::TextButton saveStripBtn;
  juce::TextButton loadStripBtn;
  juce::TextButton stripMenuBtn{"..."};
  juce::TextButton expandBtn{"+"};
  juce::TextButton collapseBtn{"-"};
  juce::TextButton collapsedMuteBtn{"M"};

  juce::TextButton fohRoutingBtn;
  juce::TextButton iemRoutingBtn;
  juce::TextButton linkButton;

  juce::Slider channelSlider;
  juce::Slider iemSlider;
  juce::Slider iemOffsetKnob;
  juce::ComboBox inputSelector;
  juce::ComboBox outputSelector;
  juce::Label noteRangeLabel;

  // Clickable strip pill shown whenever MP3 tracks/playlists are loaded on
  // this strip, so scrolling the mixer immediately reveals the MP3 strip and
  // one click opens the MP3 player.
  class Mp3BadgeButton : public juce::Button {
  public:
    Mp3BadgeButton() : juce::Button("Mp3Badge") {
      setMouseCursor(juce::MouseCursor::PointingHandCursor);
    }
    void setState(int numTracks, bool isNowPlaying, float pulse) {
      if (trackCount != numTracks || playing != isNowPlaying ||
          std::abs(pulsePhase - pulse) > 0.02f) {
        trackCount = numTracks;
        playing = isNowPlaying;
        pulsePhase = pulse;
        repaint();
      }
    }
    void setCompact(bool isCompact) {
      if (compact != isCompact) {
        compact = isCompact;
        repaint();
      }
    }
    void paintButton(juce::Graphics &g, bool isOver, bool isDown) override;

  private:
    int trackCount = 0;
    bool playing = false;
    bool compact = false;
    float pulsePhase = 0.0f;
  };

  LEDIndicator midiLed{ThemeManager::get(Theme::Role::midiNote)};
  Mp3BadgeButton mp3BadgeBtn;

  float curLeft = 0.0f;
  float curRight = 0.0f;
  int slotIndex;
  bool isMonitorIn = false;
  bool isAccordion = false;
  bool isReturn = false;

  juce::Rectangle<int> cachedMeterArea;
  float prevLeft = -1.0f;
  float prevRight = -1.0f;
  juce::StringArray prevSlotNames;
  bool prevBypassed = false;
  bool prevFohEnabled = false;
  bool prevIemEnabled = false;
  float prevIemOffset = -1.0f;
  int prevLowNote = -1;
  int prevHighNote = -1;
  bool cachedHasAnyPlugin = false;
  bool prevUnusedEmpty = false;
  bool prevMp3Loaded = false;
  bool prevMp3Playing = false;
  int prevMp3Count = -1;
  juce::String prevMp3Title;

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(RackSlotComponent)
};
