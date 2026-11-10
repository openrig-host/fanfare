#include "RackSlotComponent.h"
#include "ButtonRoles.h"
#include "ThemeManager.h"
#include "Logger.h"
#include "IMidiNoteLearner.h"
#include "MainComponent.h"

namespace {
juce::String midiNoteName(double v) {
  static const char *names[] = {"C",  "C#", "D",  "D#", "E",  "F",
                                "F#", "G",  "G#", "A",  "A#", "B"};
  int n = juce::jlimit(0, 127, (int)v);
  return juce::String(names[n % 12]) + juce::String(n / 12 - 1);
}

bool isBlackKey(int note) {
  int n = note % 12;
  return (n == 1 || n == 3 || n == 6 || n == 8 || n == 10);
}

juce::Colour resolveStripCategoryColor(const RackSlot &slot, int slotIndex,
                                       bool isMonitorIn, bool isAccordion,
                                       bool isReturn) {
  juce::Colour c = slot.getChannelColor();
  if (c == juce::Colour(0xff2a2a2a)) {
    if (isMonitorIn)
      return ThemeManager::get(Theme::Role::catMonitor);
    if (isAccordion)
      return ThemeManager::get(Theme::Role::catAccordion);
    if (isReturn)
      return ThemeManager::get(Theme::Role::catReturn);
    if (slotIndex == 1 || slot.getName().containsIgnoreCase("CK88") ||
        slot.getName().containsIgnoreCase("RD88") ||
        slot.getName().containsIgnoreCase("Keyboard"))
      return ThemeManager::get(Theme::Role::catKeyboard);
    return ThemeManager::get(Theme::Role::catDefault);
  }
  return c;
}

// Per-instrument layer config: enable, level, and key range. Bound live to the
// slot's ChainSlotSettings. Implements IMidiNoteLearner so MainComponent can
// route incoming MIDI notes to it for range learning.
class PluginStackConfigComp : public juce::Component,
                              public juce::Timer,
                              public IMidiNoteLearner {
public:
  PluginStackConfigComp(RackSlot &s, int chainIdx) : slot(s), idx(chainIdx) {
    auto &cs = slot.getChainSlotSettings(idx);

    enableBtn.setButtonText("ENABLED");
    enableBtn.setClickingTogglesState(true);
    enableBtn.setToggleState(cs.enabled.load(), juce::dontSendNotification);
    enableBtn.onClick = [this] {
      slot.getChainSlotSettings(idx).enabled.store(enableBtn.getToggleState());
    };
    addAndMakeVisible(enableBtn);

    levelSlider.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
    levelSlider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 52, 16);
    levelSlider.setRange(0.0, 1.5, 0.01);
    levelSlider.setValue(cs.level.load(), juce::dontSendNotification);
    levelSlider.textFromValueFunction = [](double v) { return juce::String(v, 2); };
    levelSlider.onValueChange = [this] {
      slot.getChainSlotSettings(idx).level.store((float)levelSlider.getValue());
    };
    addAndMakeVisible(levelSlider);

    levelLabel.setText("LAYER LEVEL", juce::dontSendNotification);
    levelLabel.setFont(BoutiqueLookAndFeel::condensedFont(11.0f, 0.06f));
    levelLabel.setColour(juce::Label::textColourId, ThemeManager::get(Theme::Role::engrave));
    levelLabel.setJustificationType(juce::Justification::centred);
    addAndMakeVisible(levelLabel);

    lowNote = cs.lowNote.load();
    highNote = cs.highNote.load();

    // Start in learn mode automatically
    learning = true;
    learnedLow = -1;
    learnedHigh = -1;
    startTimerHz(20);

    doneBtn.setButtonText("DONE");
    Fanfare::setButtonRole(doneBtn, Fanfare::ButtonRole::Primary);
    doneBtn.onClick = [this] {
      if (auto *co = findParentComponentOfClass<juce::CallOutBox>())
        co->dismiss();
    };
    addAndMakeVisible(doneBtn);

    resetBtn.setButtonText("RESET");
    Fanfare::setButtonRole(resetBtn, Fanfare::ButtonRole::Danger);
    resetBtn.onClick = [this] {
      learnedLow = -1;
      learnedHigh = -1;
      lowNote = 0;
      highNote = 127;
      auto &cs2 = slot.getChainSlotSettings(idx);
      cs2.lowNote.store(0);
      cs2.highNote.store(127);
      repaint();
    };
    addAndMakeVisible(resetBtn);

    setSize(380, 250);
  }

  ~PluginStackConfigComp() override { stopTimer(); }

  void resized() override {
    auto b = getLocalBounds().reduced(10);
    enableBtn.setBounds(b.removeFromTop(24));
    b.removeFromTop(4);
    levelLabel.setBounds(b.removeFromTop(14));
    levelSlider.setBounds(b.removeFromTop(58));
    b.removeFromTop(4);

    auto btnRow = b.removeFromBottom(28);
    int bw = btnRow.getWidth() / 2;
    doneBtn.setBounds(btnRow.removeFromLeft(bw).reduced(2));
    resetBtn.setBounds(btnRow.reduced(2));

    b.removeFromBottom(4);
    keyboardArea = b;
  }

  void paint(juce::Graphics &g) override {
    g.fillAll(ThemeManager::get(Theme::Role::panel));

    auto titleArea = keyboardArea.withHeight(16).translated(0, -2);
    g.setColour(ThemeManager::get(Theme::Role::foh));
    g.setFont(BoutiqueLookAndFeel::condensedFont(12.0f, 0.06f));
    g.drawText("PLAY NOTES TO SET RANGE", titleArea, juce::Justification::centred);

    g.setColour(ThemeManager::get(Theme::Role::textDim));
    g.setFont(BoutiqueLookAndFeel::monoFont(11.0f, true));
    g.drawText("LOW: " + midiNoteName(lowNote), keyboardArea.getX(), keyboardArea.getY() - 2,
               88, 16, juce::Justification::left);
    g.drawText("HIGH: " + midiNoteName(highNote), keyboardArea.getRight() - 88, keyboardArea.getY() - 2,
               88, 16, juce::Justification::right);

    auto kbArea = keyboardArea.toFloat().withTrimmedTop(16.0f);
    drawKeyboard(g, kbArea);
    drawRangeOverlay(g, kbArea);
  }

  void mouseDown(const juce::MouseEvent &e) override {
    auto kbArea = keyboardArea.toFloat().withTrimmedTop(16.0f);
    if (kbArea.contains(e.position)) {
      learnedLow = -1;
      learnedHigh = -1;
      updateNoteFromMouse(e.position, kbArea);
    }
  }

  void mouseDrag(const juce::MouseEvent &e) override {
    auto kbArea = keyboardArea.toFloat().withTrimmedTop(16.0f);
    if (kbArea.contains(e.position)) {
      updateNoteFromMouse(e.position, kbArea);
    }
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
    auto &cs = slot.getChainSlotSettings(idx);
    cs.lowNote.store(lowNote);
    cs.highNote.store(highNote);
    repaint();
  }

  bool isLearning() const override { return learning; }

private:
  RackSlot &slot;
  int idx;
  int lowNote = 0;
  int highNote = 127;
  bool learning = true;
  int learnedLow = -1;
  int learnedHigh = -1;

  juce::ToggleButton enableBtn;
  juce::Slider levelSlider;
  juce::Label levelLabel;
  juce::TextButton doneBtn, resetBtn;
  juce::Rectangle<int> keyboardArea;

  void timerCallback() override { repaint(); }

  void drawKeyboard(juce::Graphics &g, juce::Rectangle<float> area) {
    float noteWidth = area.getWidth() / 128.0f;
    for (int i = 0; i < 128; ++i) {
      if (!isBlackKey(i)) {
        juce::Rectangle<float> noteRect(area.getX() + i * noteWidth, area.getY(),
                                        noteWidth, area.getHeight());
        g.setColour(juce::Colour(0xFFE8E6DE));
        g.fillRect(noteRect.reduced(0.3f));
        if (i % 12 == 0) {
          g.setColour(juce::Colour(0xFF454A4F));
          g.setFont(8.0f);
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
        g.setColour(juce::Colour(0xFF1A1E21));
        g.fillRect(noteRect);
      }
    }
  }

  void drawRangeOverlay(juce::Graphics &g, juce::Rectangle<float> area) {
    float noteWidth = area.getWidth() / 128.0f;
    float x1 = area.getX() + lowNote * noteWidth;
    float x2 = area.getX() + (highNote + 1) * noteWidth;

    g.setColour(ThemeManager::get(Theme::Role::sunken).withAlpha(0.72f));
    g.fillRect(area.withWidth(x1 - area.getX()));
    g.fillRect(area.withX(x2).withWidth(area.getRight() - x2));

    g.setColour(ThemeManager::get(Theme::Role::foh).withAlpha(0.34f));
    g.fillRect(juce::Rectangle<float>(x1, area.getY(), x2 - x1, area.getHeight()));

    g.setColour(ThemeManager::get(Theme::Role::foh));
    g.drawVerticalLine((int)x1, area.getY() - 2, area.getBottom() + 2);
    g.drawVerticalLine((int)x2, area.getY() - 2, area.getBottom() + 2);
  }

  void updateNoteFromMouse(juce::Point<float> p, juce::Rectangle<float> area) {
    float normalizedX = (p.getX() - area.getX()) / area.getWidth();
    int note = juce::jlimit(0, 127, (int)(normalizedX * 128.0f));

    if (std::abs(note - lowNote) < std::abs(note - highNote)) {
      lowNote = note;
    } else {
      highNote = note;
    }

    int actualLow = std::min(lowNote, highNote);
    int actualHigh = std::max(lowNote, highNote);
    lowNote = actualLow;
    highNote = actualHigh;

    auto &cs = slot.getChainSlotSettings(idx);
    cs.lowNote.store(actualLow);
    cs.highNote.store(actualHigh);
    repaint();
  }
};
} // namespace

RackSlotComponent::RackSlotComponent(RackSlot &s, int index, juce::LookAndFeel &laf)
    : slot(s), slotIndex(index) {
  slotBtnMouseListener.owner = this;
  longPressListener.owner = this;

  addAndMakeVisible(channelSlider);
  channelSlider.setLookAndFeel(&laf);
  channelSlider.setComponentID("foh");
  channelSlider.setSliderStyle(juce::Slider::LinearVertical);
  channelSlider.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
  channelSlider.setRange(0.0, 1.0);
  channelSlider.onValueChange = [this]() {
    float val = (float)channelSlider.getValue();
    slot.setFohLevel(val);
    if (slot.areFadersLinked()) {
      slot.setIemLevel(val);
      iemSlider.setValue(val, juce::dontSendNotification);
    }
  };

  addAndMakeVisible(iemSlider);
  iemSlider.setLookAndFeel(&laf);
  iemSlider.setComponentID("iem");
  iemSlider.setSliderStyle(juce::Slider::LinearVertical);
  iemSlider.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
  iemSlider.setRange(0.0, 1.0);
  iemSlider.onValueChange = [this]() {
    float val = (float)iemSlider.getValue();
    slot.setIemLevel(val);
    if (slot.areFadersLinked()) {
      slot.setFohLevel(val);
      channelSlider.setValue(val, juce::dontSendNotification);
    }
  };

  addAndMakeVisible(linkButton);
  linkButton.setVisible(false);
  linkButton.setButtonText("LINK");
  Fanfare::setButtonRole(linkButton, Fanfare::ButtonRole::Subtle);
  linkButton.setClickingTogglesState(true);
  linkButton.setToggleState(true, juce::dontSendNotification);
  linkButton.setTooltip("Link FOH/IEM faders");
  linkButton.onClick = [this]() {
    bool linked = linkButton.getToggleState();
    slot.setFadersLinked(linked);
    linkButton.setButtonText(linked ? "LINK" : "UNLK");
  };

  addAndMakeVisible(iemOffsetKnob);
  iemOffsetKnob.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
  iemOffsetKnob.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
  iemOffsetKnob.setRange(0.0, 2.0);
  iemOffsetKnob.setValue(1.0);
  iemOffsetKnob.setTooltip("IEM Offset (relative to FOH)");
  iemOffsetKnob.onValueChange = [this] {
    slot.setIemOffset((float)iemOffsetKnob.getValue());
  };

  addAndMakeVisible(inputSelector);
  inputSelector.setTooltip("Select hardware input channel");
  inputSelector.addItem("---", 1);
  inputSelector.setSelectedId(1, juce::dontSendNotification);
  inputSelector.onChange = [this]() {
    slot.setInputChannelIndex(inputSelector.getSelectedId() - 2);
  };

  addAndMakeVisible(outputSelector);
  outputSelector.setTooltip("Select output target (Main FOH/IEM, Direct Hardware Out, or Subgroup)");
  outputSelector.onChange = [this]() {
    int selId = outputSelector.getSelectedId();
    int target = selId - 2;
    slot.setOutputTarget(target);
    if (onRename) {
      onRename();
    }
  };

  addAndMakeVisible(fohRoutingBtn);
  fohRoutingBtn.setClickingTogglesState(true);
  fohRoutingBtn.setButtonText("FOH");
  Fanfare::setButtonRole(fohRoutingBtn, Fanfare::ButtonRole::Foh);
  fohRoutingBtn.getProperties().set("isSendToggle", true);
  fohRoutingBtn.setTooltip("FOH Send: Routes this channel's signal to the Front of House mix. Hold to learn a FOH fader CC.");
  fohRoutingBtn.setToggleState(slot.isFohEnabled(), juce::dontSendNotification);
  fohRoutingBtn.onClick = [this]() {
    if (suppressLongPressClick) {
      suppressLongPressClick = false;
      fohRoutingBtn.setToggleState(slot.isFohEnabled(), juce::dontSendNotification);
      return;
    }
    slot.setFohEnabled(fohRoutingBtn.getToggleState());
  };
  fohRoutingBtn.addMouseListener(&longPressListener, false);

  if (slot.getName() == "Monitor In") {
    fohRoutingBtn.setVisible(false);
  }

  addAndMakeVisible(iemRoutingBtn);
  iemRoutingBtn.setClickingTogglesState(true);
  iemRoutingBtn.setButtonText("IEM");
  Fanfare::setButtonRole(iemRoutingBtn, Fanfare::ButtonRole::Iem);
  iemRoutingBtn.getProperties().set("isSendToggle", true);
  iemRoutingBtn.setTooltip("IEM Send: Routes this channel's signal to your In-Ear Monitors mix. Hold to learn an IEM fader CC.");
  iemRoutingBtn.setToggleState(slot.isIemEnabled(), juce::dontSendNotification);
  iemRoutingBtn.onClick = [this]() {
    if (suppressLongPressClick) {
      suppressLongPressClick = false;
      iemRoutingBtn.setToggleState(slot.isIemEnabled(), juce::dontSendNotification);
      return;
    }
    slot.setIemEnabled(iemRoutingBtn.getToggleState());
  };
  iemRoutingBtn.addMouseListener(&longPressListener, false);

  if (slot.getName() == "Monitor In") {
    iemRoutingBtn.setEnabled(false);
    iemRoutingBtn.setToggleState(true, juce::dontSendNotification);
  }

  addAndMakeVisible(bypassButton);
  bypassButton.setButtonText("MUTE");
  Fanfare::setButtonRole(bypassButton, Fanfare::ButtonRole::Mute);
  bypassButton.setTooltip("Mute Strip: Instantly cuts all audio output for this strip. Hold to learn a mute CC.");
  bypassButton.setClickingTogglesState(true);
  bypassButton.onClick = [this] {
    if (suppressLongPressClick) {
      suppressLongPressClick = false;
      bypassButton.setToggleState(slot.isBypassed(), juce::dontSendNotification);
      return;
    }
    bool m = bypassButton.getToggleState();
    slot.setBypass(m);
    if (onBypassChanged)
      onBypassChanged(m);
  };
  bypassButton.addMouseListener(&longPressListener, false);

  addChildComponent(expandBtn);
  expandBtn.setButtonText("+");
  Fanfare::setButtonRole(expandBtn, Fanfare::ButtonRole::Subtle);
  expandBtn.setTooltip("Click to Expand Channel Strip");
  expandBtn.onClick = [this] { setCollapsed(false); };

  addChildComponent(collapsedMuteBtn);
  collapsedMuteBtn.setButtonText("M");
  Fanfare::setButtonRole(collapsedMuteBtn, Fanfare::ButtonRole::Mute);
  collapsedMuteBtn.setTooltip("Muted: Click to Unmute and Expand. Hold to learn a mute CC.");
  collapsedMuteBtn.setClickingTogglesState(true);
  collapsedMuteBtn.onClick = [this] {
    if (suppressLongPressClick) {
      suppressLongPressClick = false;
      collapsedMuteBtn.setToggleState(slot.isBypassed(), juce::dontSendNotification);
      return;
    }
    bool m = collapsedMuteBtn.getToggleState();
    slot.setBypass(m);
    bypassButton.setToggleState(m, juce::dontSendNotification);
    if (!m) setCollapsed(false);
    if (onBypassChanged) onBypassChanged(m);
  };
  collapsedMuteBtn.addMouseListener(&longPressListener, false);

  addAndMakeVisible(collapseBtn);
  collapseBtn.setButtonText("-");
  Fanfare::setButtonRole(collapseBtn, Fanfare::ButtonRole::Subtle);
  collapseBtn.setTooltip("Collapse Channel Strip");
  collapseBtn.onClick = [this] { setCollapsed(true); };

  for (int i = 0; i < 3; ++i) {
    addAndMakeVisible(slotBtns[i]);
    Fanfare::setButtonRole(slotBtns[i], Fanfare::ButtonRole::PluginSlot);
    slotBtns[i].getProperties().set("isPluginSlot", true);
    slotBtns[i].addMouseListener(&slotBtnMouseListener, false);
    const auto initName = slot.getPluginName(i);
    slotBtns[i].setButtonText(initName.isEmpty() ? ("SLOT " + juce::String(i + 1)) : initName);
    slotBtns[i].onClick = [this, i] {
      if (onShowPluginMenu)
        onShowPluginMenu(i);
    };

    addAndMakeVisible(editGuiBtns[i]);
    editGuiBtns[i].setButtonText("E");
    Fanfare::setButtonRole(editGuiBtns[i], Fanfare::ButtonRole::Subtle);
    editGuiBtns[i].setTooltip("Open Plugin Editor Window");
    editGuiBtns[i].onClick = [this, i] {
      if (slot.isChainSlotMidiOut(i)) {
        if (onEditMidiOut)
          onEditMidiOut(i);
      } else if (onOpenEditor) {
        onOpenEditor(i);
      }
    };
  }

  addAndMakeVisible(ccButton);
  ccButton.setButtonText("CC");
  Fanfare::setButtonRole(ccButton, Fanfare::ButtonRole::Standard);
  ccButton.setTooltip("CC Mapping: Opens the assignment manager for mapping MIDI continuous controllers.");
  ccButton.onClick = [this] {
    if (onShowCCDialog)
      onShowCCDialog(0);
  };

  addAndMakeVisible(noteRangeButton);
  noteRangeButton.setButtonText("NR");
  Fanfare::setButtonRole(noteRangeButton, Fanfare::ButtonRole::Standard);
  noteRangeButton.setTooltip("Note Range: Restricts this channel strip to a specific key range on your controller.");
  noteRangeButton.onClick = [this] {
    if (onShowNoteRangeDialog)
      onShowNoteRangeDialog();
  };

  addAndMakeVisible(customizeButton);
  customizeButton.setButtonText("DYN");
  Fanfare::setButtonRole(customizeButton, Fanfare::ButtonRole::Standard);
  customizeButton.setTooltip("Dynamics (DYN): Opens the channel strip processor (Gate, EQ, Compressor).");
  customizeButton.onClick = [this] {
    if (onShowChannelStrip)
      onShowChannelStrip();
  };

  addAndMakeVisible(arpButton);
  arpButton.setButtonText("MIDI");
  Fanfare::setButtonRole(arpButton, Fanfare::ButtonRole::Standard);
  arpButton.setTooltip("MIDI FX: Opens the built-in arpeggiator and harmonizer controls.");
  arpButton.onClick = [this] {
    if (onShowArpeggiator)
      onShowArpeggiator();
  };

  addAndMakeVisible(samplerButton);
  samplerButton.setButtonText("SMP");
  Fanfare::setButtonRole(samplerButton, Fanfare::ButtonRole::Standard);
  samplerButton.setTooltip("Open Sampler");
  samplerButton.onClick = [this] {
    if (onShowSampler)
      onShowSampler();
  };

  addAndMakeVisible(mp3Button);
  mp3Button.setButtonText("MP3");
  Fanfare::setButtonRole(mp3Button, Fanfare::ButtonRole::Standard);
  mp3Button.setTooltip("Open MP3 Playlist Player");
  mp3Button.onClick = [this] {
    if (onShowMp3Player)
      onShowMp3Player();
  };

  addAndMakeVisible(synthButton);
  synthButton.setButtonText("SYN");
  Fanfare::setButtonRole(synthButton, Fanfare::ButtonRole::Standard);
  synthButton.setTooltip("Open the built-in BLEEP-216 synth (pads / leads / basses)");
  synthButton.onClick = [this] {
    if (onShowSynth)
      onShowSynth();
  };

  addAndMakeVisible(saveStripBtn);
  saveStripBtn.setButtonText("SAVE");
  Fanfare::setButtonRole(saveStripBtn, Fanfare::ButtonRole::Standard);
  saveStripBtn.setTooltip("Save this strip to file");
  saveStripBtn.onClick = [this] {
    if (onSaveStrip)
      onSaveStrip();
  };

  addAndMakeVisible(loadStripBtn);
  loadStripBtn.setButtonText("LOAD");
  Fanfare::setButtonRole(loadStripBtn, Fanfare::ButtonRole::Standard);
  loadStripBtn.setTooltip("Load a strip from file");
  loadStripBtn.onClick = [this] {
    if (onLoadStripFile || onLoadStrip)
      showLoadStripMenu();
  };

  addChildComponent(stripMenuBtn);
  Fanfare::setButtonRole(stripMenuBtn, Fanfare::ButtonRole::Subtle);
  stripMenuBtn.setTooltip("Strip actions: Note range, MIDI FX, Sampler, Save/Load strip, Rename, Colour");
  stripMenuBtn.onClick = [this] { showStripActionsMenu(); };

  addAndMakeVisible(midiLed);
  addChildComponent(mp3BadgeBtn);
  mp3BadgeBtn.setTooltip("Open MP3 Playlist Player");
  mp3BadgeBtn.onClick = [this] {
    if (onShowMp3Player)
      onShowMp3Player();
  };

  addAndMakeVisible(noteRangeLabel);
  noteRangeLabel.setJustificationType(juce::Justification::centred);
  noteRangeLabel.setFont(BoutiqueLookAndFeel::monoFont(9.5f, true));
  noteRangeLabel.setColour(juce::Label::textColourId, ThemeManager::get(Theme::Role::engrave));
  noteRangeLabel.setMouseCursor(juce::MouseCursor::PointingHandCursor);
  noteRangeLabel.setTooltip("Click to edit note range & keyboard split");
  noteRangeLabelListener.owner = this;
  noteRangeLabel.addMouseListener(&noteRangeLabelListener, false);

  for (auto *b : {&ccButton, &noteRangeButton, &customizeButton, &arpButton,
                  &samplerButton, &mp3Button, &synthButton, &bypassButton, &fohRoutingBtn, &iemRoutingBtn,
                  &saveStripBtn, &loadStripBtn})
    b->addMouseListener(this, false);

  prevUnusedEmpty = isUnusedEmptyStrip();
  startTimer(50);
}

RackSlotComponent::~RackSlotComponent() {
  channelSlider.setLookAndFeel(nullptr);
  iemSlider.setLookAndFeel(nullptr);
  stopTimer();
}

void RackSlotComponent::lookAndFeelChanged() {
  const auto rangeStr = getActiveNoteRangeString();
  const bool isCustomSplit = !rangeStr.containsIgnoreCase("(Full)");
  noteRangeLabel.setColour(juce::Label::textColourId,
                           isCustomSplit ? ThemeManager::get(Theme::Role::foh)
                                         : ThemeManager::get(Theme::Role::engrave));
  resized();
  repaint();
}

bool RackSlotComponent::isUnusedEmptyStrip() const {
  if (isMonitorIn || isReturn || slotIndex == 0 || slotIndex >= 100)
    return false;
  if (slot.getInputChannelIndex() >= 0)
    return false;
  if (kMp3DecksEnabled && slot.getMp3Player().hasAnyTracks())
    return false;
  for (int i = 0; i < 3; ++i) {
    if (slot.getPluginInstance(i) != nullptr || slot.isChainSlotMidiOut(i))
      return false;
  }
  const auto n = slot.getName().trim();
  if (n.isNotEmpty() && !n.startsWithIgnoreCase("Slot "))
    return false;
  return true;
}

void RackSlotComponent::Mp3BadgeButton::paintButton(juce::Graphics &g,
                                                    bool isOver,
                                                    bool isDown) {
  const auto &t = ThemeManager::getInstance().active();
  auto r = getLocalBounds().toFloat().reduced(0.5f);
  if (r.isEmpty())
    return;

  const juce::Colour baseCol = playing ? t.ok : t.accent;
  const float fillAlpha =
      isDown ? 0.30f
             : (isOver ? 0.24f
                       : (playing ? (0.18f + 0.08f * pulsePhase) : 0.16f));
  const float borderAlpha = (isOver || playing) ? 0.90f : 0.65f;
  const float radius = compact ? 3.0f : 3.5f;

  g.setColour(baseCol.withAlpha(fillAlpha));
  g.fillRoundedRectangle(r, radius);
  g.setColour(baseCol.withAlpha(borderAlpha));
  g.drawRoundedRectangle(r, radius, 1.0f);

  const juce::Colour fgCol =
      isOver ? t.text : baseCol.interpolatedWith(t.text, 0.32f);
  g.setColour(fgCol);

  if (compact) {
    g.setFont(t.isStudio() ? BoutiqueLookAndFeel::monoFont(9.5f, true)
                           : BoutiqueLookAndFeel::condensedFont(9.5f, 0.04f));
    g.drawFittedText("MP3", getLocalBounds().reduced(1, 0),
                     juce::Justification::centred, 1, 0.75f);
    return;
  }

  auto inner = r.reduced(6.0f, 0.0f);
  auto iconArea = inner.removeFromLeft(13.0f).withSizeKeepingCentre(11.0f, 11.0f);

  if (playing) {
    // Crisp play triangle
    juce::Path tri;
    tri.addTriangle(iconArea.getX() + 1.5f, iconArea.getY() + 1.2f,
                    iconArea.getX() + 1.5f, iconArea.getBottom() - 1.2f,
                    iconArea.getRight() - 1.0f, iconArea.getCentreY());
    g.fillPath(tri);
  } else {
    // Crisp beamed eighth-notes icon
    const float xL = iconArea.getX() + 2.6f;
    const float xR = iconArea.getRight() - 2.0f;
    const float yTop = iconArea.getY() + 1.5f;
    const float yBotL = iconArea.getBottom() - 2.2f;
    const float yBotR = iconArea.getBottom() - 2.8f;

    juce::Path notes;
    notes.addEllipse(xL - 2.3f, yBotL - 1.6f, 3.8f, 3.0f);
    notes.addEllipse(xR - 2.3f, yBotR - 1.6f, 3.8f, 3.0f);
    g.fillPath(notes);

    juce::Path stems;
    stems.startNewSubPath(xL + 1.1f, yBotL);
    stems.lineTo(xL + 1.1f, yTop + 1.2f);
    stems.lineTo(xR + 1.1f, yTop);
    stems.lineTo(xR + 1.1f, yBotR);
    g.strokePath(stems, juce::PathStrokeType(1.35f, juce::PathStrokeType::curved,
                                             juce::PathStrokeType::rounded));
  }

  inner.removeFromLeft(3.0f);
  juce::String label = "MP3";
  if (trackCount > 0 && getWidth() >= 84)
    label << "  " << juce::String(trackCount);

  g.setFont(t.isStudio() ? BoutiqueLookAndFeel::uiFont(11.0f, true)
                         : BoutiqueLookAndFeel::condensedFont(11.5f, 0.05f));
  g.drawFittedText(label, inner.toNearestInt(), juce::Justification::centredLeft,
                   1, 0.85f);
}

void RackSlotComponent::timerCallback() {
  float decay = 0.85f;

  float peakL = FanfareLog::amplitudeToLogScale(slot.getLeftPeak() *
                                                slot.getChannelLevel());
  float peakR = FanfareLog::amplitudeToLogScale(slot.getRightPeak() *
                                                slot.getChannelLevel());

  curLeft = std::max(peakL, curLeft * decay);
  curRight = std::max(peakR, curRight * decay);

  if (slot.getAndClearMidiActivity())
    midiLed.flash();

  // MP3 strip badge/button: visible whenever MP3 tracks/playlists are loaded on this strip
  if (kMp3DecksEnabled && !isReturn && !isMonitorIn) {
    auto &mp3 = slot.getMp3Player();
    const bool mp3Loaded = mp3.hasAnyTracks();
    const bool mp3Playing = mp3Loaded && mp3.isPlaying();
    const int mp3Count = mp3Loaded ? mp3.getPlaylistCount() : 0;
    const float pulse =
        mp3Playing ? (0.5f + 0.5f * std::sin((float)juce::Time::getMillisecondCounter() * 0.008f))
                   : 0.0f;

    mp3BadgeBtn.setState(mp3Count, mp3Playing, pulse);

    if (mp3Loaded != prevMp3Loaded) {
      prevMp3Loaded = mp3Loaded;
      Fanfare::setButtonRole(mp3Button, mp3Loaded ? Fanfare::ButtonRole::Primary
                                                  : Fanfare::ButtonRole::Standard);
      mp3Button.setToggleState(mp3Loaded, juce::dontSendNotification);
      resized();
      repaint();
    }
    if (mp3Playing != prevMp3Playing || mp3Count != prevMp3Count) {
      prevMp3Playing = mp3Playing;
      prevMp3Count = mp3Count;
      const juce::String curTitle = mp3Loaded ? mp3.getCurrentOrFirstTrackTitle() : juce::String();
      prevMp3Title = curTitle;
      juce::String tip = "Open MP3 Playlist Player";
      if (mp3Loaded) {
        tip = mp3Playing ? "MP3 Player (Playing" : "MP3 Player (Loaded";
        if (mp3Count > 0)
          tip << " - " << juce::String(mp3Count) << (mp3Count == 1 ? " track" : " tracks");
        if (curTitle.isNotEmpty())
          tip << ": " << curTitle;
        tip << ") - Click to open";
      }
      mp3BadgeBtn.setTooltip(tip);
      mp3Button.setTooltip(tip);
    }
  }

  const auto rangeStr = getActiveNoteRangeString();
  noteRangeLabel.setText(rangeStr, juce::dontSendNotification);
  const bool isCustomSplit = !rangeStr.containsIgnoreCase("(Full)");
  noteRangeLabel.setColour(juce::Label::textColourId,
                           isCustomSplit ? ThemeManager::get(Theme::Role::foh)
                                         : ThemeManager::get(Theme::Role::engrave));

  bool metersChanged = (std::abs(curLeft - prevLeft) > 0.001f ||
                        std::abs(curRight - prevRight) > 0.001f);
  if (metersChanged) {
    prevLeft = curLeft;
    prevRight = curRight;
    if (cachedMeterArea.getWidth() > 0)
      repaint(cachedMeterArea);
  }

  bool stateChanged = false;
  int numBtns = isReturn ? 2 : 3;
  bool anyPlugin = false;
  const juce::Colour categoryCol =
      resolveStripCategoryColor(slot, slotIndex, isMonitorIn, isAccordion, isReturn);
  const bool hasDistinctCat =
      (categoryCol != ThemeManager::get(Theme::Role::catDefault) &&
       categoryCol != juce::Colour(0xff2a2a2a));
  const juce::Colour slotAccentCol =
      hasDistinctCat ? categoryCol : ThemeManager::get(Theme::Role::accent);

  for (int i = 0; i < 3; ++i) {
    if (i >= numBtns) {
      slotBtns[i].setVisible(false);
      editGuiBtns[i].setVisible(false);
      continue;
    }

    auto name = slot.getPluginName(i);
    bool isMidiOut = slot.isChainSlotMidiOut(i);
    if (isMidiOut)
      name = "MIDI > " + slot.getChainSlotMidiOutName(i) +
             " ch" + juce::String(slot.getChainSlotMidiOutChannel(i));
    if (i >= prevSlotNames.size()) {
      prevSlotNames.add("");
    }
    if (prevSlotNames[i] != name) {
      prevSlotNames.set(i, name);
      slotBtns[i].setButtonText(name.isEmpty() ? ("SLOT " + juce::String(i + 1)) : name);
      stateChanged = true;
    }
    bool hasPlugin = slot.getPluginInstance(i) != nullptr || isMidiOut;
    if (hasPlugin)
      anyPlugin = true;
    editGuiBtns[i].setButtonText(isMidiOut ? "M" : "E");
    slotBtns[i].getProperties().set("isPluginSlot", true);
    slotBtns[i].getProperties().set("hasPlugin", hasPlugin);
    slotBtns[i].getProperties().set("isMidiOut", isMidiOut);
    slotBtns[i].getProperties().set("slotAccent", (int)slotAccentCol.getARGB());
  }

  if (anyPlugin != cachedHasAnyPlugin) {
    cachedHasAnyPlugin = anyPlugin;
    stateChanged = true;
  }

  bool unusedNow = isUnusedEmptyStrip();
  if (unusedNow != prevUnusedEmpty) {
    prevUnusedEmpty = unusedNow;
    resized();
    stateChanged = true;
    if (onCollapseChanged)
      onCollapseChanged(isCollapsed);
  }

  if (slot.isBypassed() != prevBypassed) {
    prevBypassed = slot.isBypassed();
    bypassButton.setToggleState(prevBypassed, juce::dontSendNotification);
    collapsedMuteBtn.setToggleState(prevBypassed, juce::dontSendNotification);
    stateChanged = true;
  }
  if (slot.isFohEnabled() != prevFohEnabled) {
    prevFohEnabled = slot.isFohEnabled();
    fohRoutingBtn.setToggleState(prevFohEnabled, juce::dontSendNotification);
    stateChanged = true;
  }
  if (slot.isIemEnabled() != prevIemEnabled) {
    prevIemEnabled = slot.isIemEnabled();
    iemRoutingBtn.setToggleState(prevIemEnabled, juce::dontSendNotification);
    stateChanged = true;
  }
  const float curIemOffset = slot.getIemOffset();
  if (!isMonitorIn && !isReturn && std::abs(curIemOffset - prevIemOffset) > 0.005f) {
    prevIemOffset = curIemOffset;
    if (std::abs(curIemOffset - 1.0f) <= 0.015f) {
      iemRoutingBtn.setButtonText("IEM");
      iemRoutingBtn.setTooltip(
          "IEM Send: Routes this channel's signal to your In-Ear Monitors mix (0.0 dB trim; adjust IEM Trim in DYN). Hold to learn an IEM fader CC.");
    } else if (curIemOffset <= 0.005f) {
      iemRoutingBtn.setButtonText("IEM OFF");
      iemRoutingBtn.setTooltip(
          "IEM Send: Strip IEM Trim is set to OFF (-inf dB) in DYN. Open DYN to raise IEM Trim.");
    } else {
      const float db = juce::Decibels::gainToDecibels(curIemOffset, -60.0f);
      const juce::String dbStr =
          (db >= 0.0f ? "+" : "") + juce::String(db, 1) + "dB";
      iemRoutingBtn.setButtonText("IEM " + dbStr);
      iemRoutingBtn.setTooltip(
          "IEM Send: Strip IEM Trim is " + dbStr +
          " relative to FOH (adjust in DYN). Hold to learn an IEM fader CC.");
    }
    stateChanged = true;
  }
  if (slot.getLowNote() != prevLowNote || slot.getHighNote() != prevHighNote) {
    prevLowNote = slot.getLowNote();
    prevHighNote = slot.getHighNote();
    stateChanged = true;
  }

  if (std::abs(channelSlider.getValue() - slot.getFohLevel()) > 0.001f)
    channelSlider.setValue(slot.getFohLevel(), juce::dontSendNotification);
  if (isAccordion) {
    if (std::abs(iemOffsetKnob.getValue() - slot.getIemOffset()) > 0.001f)
      iemOffsetKnob.setValue(slot.getIemOffset(), juce::dontSendNotification);
  } else {
    if (std::abs(iemSlider.getValue() - slot.getIemLevel()) > 0.001f)
      iemSlider.setValue(slot.getIemLevel(), juce::dontSendNotification);
  }

  if (stateChanged)
    repaint();
}

void RackSlotComponent::setCollapsed(bool shouldCollapse) {
  if (isCollapsed != shouldCollapse) {
    isCollapsed = shouldCollapse;
    resized();
    repaint();
    if (onCollapseChanged)
      onCollapseChanged(isCollapsed);
  }
}

void RackSlotComponent::paint(juce::Graphics &g) {
  const auto &t = ThemeManager::getInstance().active();
  const juce::Colour categoryCol =
      resolveStripCategoryColor(slot, slotIndex, isMonitorIn, isAccordion, isReturn);
  const bool hasDistinctColor =
      (categoryCol != juce::Colour(0xff2a2a2a) && categoryCol != t.catDefault);

  if (isCollapsed) {
    auto bounds = getLocalBounds().toFloat().reduced(0.5f);

    if (isUnusedEmptyStrip()) {
      const bool hovered = isMouseOver(true);
      g.setColour(hovered ? t.panelAlt : t.sunken);
      g.fillRoundedRectangle(bounds, 3.0f);

      g.setColour(hovered ? t.accent.withAlpha(0.75f) : t.hairline);
      g.drawRoundedRectangle(bounds, 3.0f, 1.0f);

      // Top strip number
      g.setFont(t.isStudio() ? BoutiqueLookAndFeel::monoFont(10.5f, false)
                             : BoutiqueLookAndFeel::condensedFont(10.5f, 0.04f));
      g.setColour(hovered ? t.text : t.textDim);
      g.drawText(juce::String(slotIndex + 1),
                 juce::Rectangle<float>(bounds.getX(), bounds.getY() + 4.0f, bounds.getWidth(), 14.0f),
                 juce::Justification::centred, false);

      // Center "+" badge + Add label
      g.setFont(t.isStudio() ? BoutiqueLookAndFeel::uiFont(17.0f, false)
                             : BoutiqueLookAndFeel::condensedFont(18.0f, 0.0f));
      g.setColour(hovered ? t.accent : t.textDim);
      g.drawText("+",
                 juce::Rectangle<float>(bounds.getX(), bounds.getCentreY() - 18.0f,
                                        bounds.getWidth(), 20.0f),
                 juce::Justification::centred, false);
      g.setFont(t.isStudio() ? BoutiqueLookAndFeel::uiFont(10.0f, false)
                             : BoutiqueLookAndFeel::condensedFont(9.5f, 0.05f));
      g.setColour(hovered ? t.accent : t.engrave);
      g.drawText(t.isStudio() ? "Add" : "ADD",
                 juce::Rectangle<float>(bounds.getX(), bounds.getCentreY() + 2.0f,
                                        bounds.getWidth(), 13.0f),
                 juce::Justification::centred, false);
      return;
    }

    // Collapsed populated / muted strip
    juce::Colour bg = slot.isBypassed() ? t.sunken
                    : ((slotIndex % 2 == 0) ? t.panel : t.panelAlt);
    g.setColour(bg);
    g.fillRoundedRectangle(bounds, 3.0f);

    // Top stage-tape stripe (2px in Studio, 3px in Front Panel)
    const float collapsedTapeH = t.isStudio() ? 2.0f : 3.0f;
    g.setColour(slot.isBypassed() ? categoryCol.withAlpha(0.45f) : categoryCol);
    g.fillRect(bounds.getX() + 1.0f, bounds.getY() + 1.0f,
               bounds.getWidth() - 2.0f, collapsedTapeH);

    g.setColour(t.border);
    g.drawRoundedRectangle(bounds, 3.0f, 1.0f);

    // Vertical strip title
    const float titleTop = mp3BadgeBtn.isVisible() ? 58.0f : 36.0f;
    const float titleBottomPad = mp3BadgeBtn.isVisible() ? 90.0f : 68.0f;
    auto textArea = juce::Rectangle<float>(3.0f, titleTop, bounds.getWidth() - 10.0f,
                                           bounds.getHeight() - titleBottomPad);
    if (textArea.getHeight() > 30.0f) {
      juce::Graphics::ScopedSaveState sss(g);
      float cx = textArea.getCentreX();
      float cy = textArea.getCentreY();
      g.addTransform(juce::AffineTransform::rotation(juce::MathConstants<float>::halfPi, cx, cy));

      juce::String nameText = slot.getName();
      if (nameText.isEmpty()) nameText = "Slot " + juce::String(slotIndex + 1);
      juce::String label = (isReturn || isMonitorIn)
                               ? nameText
                               : (juce::String(slotIndex + 1) + ". " + nameText);
      if (slot.isBypassed() && !t.isStudio()) label += " [MUTED]";

      g.setColour(slot.isBypassed() ? t.textDim : t.text);
      g.setFont(t.isStudio() ? BoutiqueLookAndFeel::uiFont(12.0f, true)
                             : BoutiqueLookAndFeel::condensedFont(12.0f, 0.05f));
      juce::Rectangle<float> rotRect(cx - textArea.getHeight() / 2.0f,
                                     cy - textArea.getWidth() / 2.0f,
                                     textArea.getHeight(), textArea.getWidth());
      g.drawText(t.isStudio() ? label : label.toUpperCase(), rotRect,
                 juce::Justification::centred, true);
    }

    if (!cachedMeterArea.isEmpty()) {
      drawVerticalMeter(g, cachedMeterArea, curLeft, curRight,
                        t.meterLow, t.meterMid, t.meterPeak);
    }
    return;
  }

  // Expanded channel strip
  auto bounds = getLocalBounds().toFloat().reduced(0.5f);
  juce::Colour panelFace = slot.isBypassed() ? t.sunken
                         : ((slotIndex % 2 == 0) ? t.panel : t.panelAlt);

  g.setColour(panelFace);
  g.fillRoundedRectangle(bounds, 3.0f);

  // Top stage-tape category stripe (2px flat in Studio, 3px + bevel in Front Panel)
  juce::Colour tapeCol = hasDistinctColor ? categoryCol : t.borderStrong;
  if (slot.isBypassed())
    tapeCol = tapeCol.withAlpha(0.45f);
  g.setColour(tapeCol);
  g.fillRect(bounds.getX() + 1.0f, bounds.getY() + 1.0f,
             bounds.getWidth() - 2.0f, t.isStudio() ? 2.0f : 3.0f);

  if (!t.isStudio()) {
    g.setColour(t.bevelHi);
    g.drawHorizontalLine((int)bounds.getY() + 4, bounds.getX() + 2.0f, bounds.getRight() - 2.0f);
  }

  // Hairline strip border
  g.setColour(t.border);
  g.drawRoundedRectangle(bounds, 3.0f, 1.0f);

  // Strip Nameplate well (y = 107..139)
  auto nameplateRect = juce::Rectangle<float>(3.0f, 107.0f, bounds.getWidth() - 5.0f, 32.0f);
  Fanfare::ModalFrame::paintRecessedWell(g, nameplateRect, 2.5f);

  if (hasDistinctColor) {
    g.setColour(slot.isBypassed() ? categoryCol.withAlpha(0.35f) : categoryCol);
    g.fillRect(nameplateRect.getX() + 1.0f, nameplateRect.getY() + 2.0f,
               2.5f, nameplateRect.getHeight() - 4.0f);
  }

  g.setColour(slot.isBypassed() ? t.textDim : t.text);
  g.setFont(t.isStudio() ? BoutiqueLookAndFeel::uiFont(12.5f, true)
                         : BoutiqueLookAndFeel::condensedFont(12.5f, 0.03f));
  juce::String plateTitle = slot.getName();
  if (!isReturn && !isMonitorIn)
    plateTitle = juce::String(slotIndex + 1) + (t.isStudio() ? "  " : " ") + plateTitle;
  g.drawFittedText(t.isStudio() ? plateTitle : plateTitle.toUpperCase(),
                   (int)nameplateRect.getX() + 4, (int)nameplateRect.getY() + 2,
                   (int)nameplateRect.getWidth() - 8, 16,
                   juce::Justification::centred, 1, 0.78f);

  // Return strip static label in slot 3 area
  if (isReturn) {
    g.setColour(t.engrave);
    g.setFont(t.isStudio() ? BoutiqueLookAndFeel::uiFont(11.0f, false)
                           : BoutiqueLookAndFeel::condensedFont(10.5f, 0.06f));
    auto vacantArea = slotBtns[2].getBounds();
    if (!vacantArea.isEmpty())
      g.drawFittedText(t.isStudio() ? "Aux return" : "AUX RETURN",
                       vacantArea, juce::Justification::centred, 1, 0.80f);
  }

  // Dedicated stereo segmented LED meter beside the fader
  if (!cachedMeterArea.isEmpty()) {
    drawVerticalMeter(g, cachedMeterArea, curLeft, curRight,
                      t.meterLow, t.meterMid, t.meterPeak);
  }

  // Subtle 2px per-strip DSP load bar at bottom edge
  {
    float load = juce::jlimit(0.0f, 1.0f, slot.getCpuUsage());
    if (load > 0.005f) {
      juce::Colour loadCol = (load < 0.6f) ? t.meterLow
                           : (load < 0.85f ? t.meterMid : t.meterPeak);
      g.setColour(loadCol.withAlpha(0.85f));
      g.fillRect(bounds.getX() + 2.0f, bounds.getBottom() - 2.5f,
                 (bounds.getWidth() - 4.0f) * load, 2.0f);
    }
  }
}

void RackSlotComponent::drawVerticalMeter(juce::Graphics &g, juce::Rectangle<int> area,
                                          float L, float R, juce::Colour low,
                                          juce::Colour mid, juce::Colour high) {
  if (area.isEmpty())
    return;
  int segs = juce::jlimit(10, 22, area.getHeight() / 9);
  BoutiqueLookAndFeel::drawStereoSegmentedMeter(g, area.toFloat(), L, R, segs, low, mid, high);
}

void RackSlotComponent::resized() {
  if (isCollapsed) {
    for (int i = 0; i < 3; ++i) {
      slotBtns[i].setVisible(false);
      editGuiBtns[i].setVisible(false);
    }
    inputSelector.setVisible(false);
    outputSelector.setVisible(false);
    bypassButton.setVisible(false);
    fohRoutingBtn.setVisible(false);
    iemRoutingBtn.setVisible(false);
    ccButton.setVisible(false);
    noteRangeButton.setVisible(false);
    customizeButton.setVisible(false);
    arpButton.setVisible(false);
    samplerButton.setVisible(false);
    synthButton.setVisible(false);
    mp3Button.setVisible(false);
    saveStripBtn.setVisible(false);
    loadStripBtn.setVisible(false);
    stripMenuBtn.setVisible(false);
    linkButton.setVisible(false);
    channelSlider.setVisible(false);
    iemSlider.setVisible(false);
    iemOffsetKnob.setVisible(false);
    noteRangeLabel.setVisible(false);
    collapseBtn.setVisible(false);

    if (isUnusedEmptyStrip()) {
      expandBtn.setVisible(false);
      collapsedMuteBtn.setVisible(false);
      midiLed.setVisible(false);
      mp3BadgeBtn.setVisible(false);
      cachedMeterArea = {};
      setTooltip("Empty Strip " + juce::String(slotIndex + 1) +
                 " - Click to expand next strip (Right-click to choose any empty strip)");
      return;
    }

    setTooltip(juce::String());
    expandBtn.setVisible(true);
    expandBtn.setBounds(2, 5, getWidth() - 4, 20);

    collapsedMuteBtn.setVisible(true);
    collapsedMuteBtn.setBounds(2, getHeight() - 25, getWidth() - 4, 20);
    collapsedMuteBtn.setToggleState(slot.isBypassed(), juce::dontSendNotification);

    const bool showMp3Badge =
        kMp3DecksEnabled && !isReturn && !isMonitorIn && slot.getMp3Player().hasAnyTracks();
    mp3BadgeBtn.setCompact(true);
    mp3BadgeBtn.setVisible(showMp3Badge);
    if (showMp3Badge) {
      mp3BadgeBtn.setBounds(2, 27, getWidth() - 4, 18);
      midiLed.setVisible(true);
      midiLed.setBounds(getWidth() / 2 - 4, 48, 8, 8);
      cachedMeterArea = juce::Rectangle<int>(getWidth() - 7, 58, 5, juce::jmax(0, getHeight() - 88));
    } else {
      midiLed.setVisible(true);
      midiLed.setBounds(getWidth() / 2 - 4, 27, 8, 8);
      cachedMeterArea = juce::Rectangle<int>(getWidth() - 7, 38, 5, juce::jmax(0, getHeight() - 68));
    }
    return;
  }

  // Expanded mode
  setTooltip(juce::String());
  expandBtn.setVisible(false);
  collapsedMuteBtn.setVisible(false);

  for (int i = 0; i < (isReturn ? 2 : 3); ++i) {
    slotBtns[i].setVisible(true);
    editGuiBtns[i].setVisible(true);
  }
  const bool studio = ThemeManager::isStudio();
  inputSelector.setVisible(true);
  outputSelector.setVisible(!isReturn);
  bypassButton.setVisible(true);
  fohRoutingBtn.setVisible(!isMonitorIn);
  iemRoutingBtn.setVisible(true);
  channelSlider.setVisible(true);
  ccButton.setVisible(!isReturn);
  noteRangeButton.setVisible(!isReturn && !studio);
  noteRangeLabel.setVisible(!isReturn);
  customizeButton.setVisible(!isReturn && !isMonitorIn);
  arpButton.setVisible(!isReturn && !isMonitorIn && !studio);
  samplerButton.setVisible(!isReturn && !isMonitorIn && !studio);
  synthButton.setVisible(!isReturn && !isMonitorIn && !studio);
  saveStripBtn.setVisible(!isReturn && !studio);
  loadStripBtn.setVisible(!isReturn && !studio);
  stripMenuBtn.setVisible(!isReturn && studio);
  iemOffsetKnob.setVisible(isAccordion);
  linkButton.setVisible(false);
  iemSlider.setVisible(false);
  midiLed.setVisible(true);

  const bool showMp3Badge =
      kMp3DecksEnabled && !isReturn && !isMonitorIn && slot.getMp3Player().hasAnyTracks();
  mp3BadgeBtn.setCompact(false);
  mp3BadgeBtn.setVisible(showMp3Badge);

  auto bounds = getLocalBounds();
  bounds.removeFromTop(5); // clear 3px top tape + bevel

  // Top area: 3 plugin rows + input selector + output selector (y = 5..105)
  auto topArea = bounds.removeFromTop(100);
  for (int i = 0; i < 3; ++i) {
    auto row = topArea.removeFromTop(20).reduced(3, 1);
    if (i == 0) {
      auto collapseBounds = row.removeFromRight(19);
      row.removeFromRight(1);
      collapseBtn.setBounds(collapseBounds);
      collapseBtn.setVisible(true);
      collapseBtn.toFront(true);
    }
    int editBtnWidth = juce::jlimit(18, 22, (int)(row.getWidth() * 0.21f));
    editGuiBtns[i].setBounds(row.removeFromRight(editBtnWidth));
    row.removeFromRight(1);
    slotBtns[i].setBounds(row);
  }

  auto inputRow = topArea.removeFromTop(20).reduced(3, 1);
  auto ledBox = inputRow.removeFromRight(12);
  midiLed.setBounds(ledBox.withSizeKeepingCentre(8, 8));
  inputRow.removeFromRight(1);
  inputSelector.setBounds(inputRow);

  auto outputRow = topArea.removeFromTop(20).reduced(3, 1);
  outputSelector.setBounds(outputRow);

  // Reserve Strip Nameplate well (y = 107..139)
  bounds.removeFromTop(36);
  noteRangeLabel.setBounds(4, 123, getWidth() - 8, 14);

  // Bottom area: MUTE / FOH / IEM caps
  auto buttonArea = bounds.removeFromBottom(64);
  const int btnHeight = 21;
  bypassButton.setBounds(buttonArea.removeFromTop(btnHeight).reduced(3, 1));
  fohRoutingBtn.setBounds(buttonArea.removeFromTop(btnHeight).reduced(3, 1));
  iemRoutingBtn.setBounds(buttonArea.removeFromTop(btnHeight).reduced(3, 1));

  // CC / NR / DYN row above MUTE (in Studio: CC / DYN / ... menu)
  auto ctrlRow = bounds.removeFromBottom(20);
  if (studio && !isReturn) {
    if (isMonitorIn) {
      int colWidth = ctrlRow.getWidth() / 2;
      ccButton.setBounds(ctrlRow.removeFromLeft(colWidth).reduced(2, 1));
      stripMenuBtn.setBounds(ctrlRow.reduced(2, 1));
    } else {
      int colWidth = ctrlRow.getWidth() / 3;
      ccButton.setBounds(ctrlRow.removeFromLeft(colWidth).reduced(2, 1));
      customizeButton.setBounds(ctrlRow.removeFromLeft(colWidth).reduced(1, 1));
      stripMenuBtn.setBounds(ctrlRow.reduced(2, 1));
    }
  } else {
    int colWidth = ctrlRow.getWidth() / 3;
    ccButton.setBounds(ctrlRow.removeFromLeft(colWidth).reduced(2, 1));
    noteRangeButton.setBounds(ctrlRow.removeFromLeft(colWidth).reduced(1, 1));
    customizeButton.setBounds(ctrlRow.reduced(2, 1));
  }

  bounds.removeFromBottom(2);
  auto faderArea = bounds.reduced(2, 2);
  if (showMp3Badge) {
    mp3BadgeBtn.setBounds(faderArea.removeFromTop(21).reduced(2, 0));
    faderArea.removeFromTop(3);
  }

  // Side utility buttons (Front Panel) + dedicated non-overlapping fader & stereo LED meter
  const int w = getWidth();
  if (studio) {
    mp3Button.setVisible(false);
    int centerSpanLeft = 8;
    int centerSpanRight = w - 8;
    int centerSpanW = juce::jmax(32, centerSpanRight - centerSpanLeft);

    int meterW = juce::jlimit(10, 14, centerSpanW / 5);
    int faderW = juce::jlimit(22, 30, centerSpanW - meterW - 4);
    int pairW = faderW + 4 + meterW;
    int pairX = centerSpanLeft + (centerSpanW - pairW) / 2;

    if (isAccordion) {
      int knobSize = 38;
      iemOffsetKnob.setBounds(pairX + (pairW - knobSize) / 2, faderArea.getY(),
                              knobSize, knobSize);
      faderArea.removeFromTop(knobSize + 4);
    }

    channelSlider.setBounds(pairX, faderArea.getY(), faderW, faderArea.getHeight());
    iemSlider.setVisible(false);
    cachedMeterArea = juce::Rectangle<int>(pairX + faderW + 4, faderArea.getY() + 4,
                                           meterW, juce::jmax(16, faderArea.getHeight() - 8));
    return;
  }

  const bool hasSideBtns = (!isReturn && !isMonitorIn);
  const int sideWidth = hasSideBtns ? juce::jlimit(23, 38, (w - 40) / 2) : 4;
  const int leftX = 3;
  const int centerY = faderArea.getCentreY();

  arpButton.setBounds(leftX, centerY - 26, sideWidth, 22);
  samplerButton.setBounds(leftX, centerY, sideWidth, 22);
  synthButton.setBounds(leftX, centerY + 26, sideWidth, 22);
  mp3Button.setVisible(kMp3DecksEnabled && !isReturn && !isMonitorIn);
  mp3Button.setBounds(leftX, centerY + 52, sideWidth, 22);

  if (!isReturn) {
    int utilW = hasSideBtns ? sideWidth : juce::jlimit(24, 36, (w - 40) / 2);
    saveStripBtn.setBounds(w - utilW - 3, centerY - 26, utilW, 22);
    loadStripBtn.setBounds(w - utilW - 3, centerY, utilW, 22);
  }

  int centerSpanLeft = hasSideBtns ? (leftX + sideWidth + 2) : 6;
  int centerSpanRight = (!isReturn) ? (saveStripBtn.getX() - 2) : (w - 6);
  int centerSpanW = juce::jmax(28, centerSpanRight - centerSpanLeft);

  int meterW = juce::jlimit(9, 13, centerSpanW / 4);
  int faderW = juce::jlimit(20, 28, centerSpanW - meterW - 3);
  int pairW = faderW + 3 + meterW;
  int pairX = centerSpanLeft + (centerSpanW - pairW) / 2;

  if (isAccordion) {
    int knobSize = 38;
    iemOffsetKnob.setBounds(pairX + (pairW - knobSize) / 2, faderArea.getY(),
                            knobSize, knobSize);
    faderArea.removeFromTop(knobSize + 4);
  }

  channelSlider.setBounds(pairX, faderArea.getY(), faderW, faderArea.getHeight());
  iemSlider.setVisible(false);
  cachedMeterArea = juce::Rectangle<int>(pairX + faderW + 3, faderArea.getY() + 4,
                                         meterW, juce::jmax(16, faderArea.getHeight() - 8));
}

void RackSlotComponent::setSpecialModes(bool monitorIn, bool accordion, bool returns) {
  isMonitorIn = monitorIn;
  isAccordion = accordion;
  isReturn = returns;

  if (isCollapsed)
    return;

  linkButton.setVisible(false);
  iemSlider.setVisible(false);
  iemOffsetKnob.setVisible(isAccordion);

  customizeButton.setVisible(!isReturn && !isMonitorIn);
  arpButton.setVisible(!isReturn && !isMonitorIn);
  samplerButton.setVisible(!isReturn && !isMonitorIn);
  synthButton.setVisible(!isReturn && !isMonitorIn);

  ccButton.setVisible(!isReturn);
  noteRangeButton.setVisible(!isReturn);
  noteRangeLabel.setVisible(!isReturn);
  saveStripBtn.setVisible(!isReturn);
  loadStripBtn.setVisible(!isReturn);
  outputSelector.setVisible(!isReturn);

  resized();
}

juce::ComboBox &RackSlotComponent::getInputSelector() { return inputSelector; }

juce::Rectangle<int> RackSlotComponent::getNoteRangeButtonBounds() {
  if (noteRangeButton.isVisible() && !noteRangeButton.getBounds().isEmpty())
    return getLocalArea(this, noteRangeButton.getBounds());
  if (noteRangeLabel.isVisible() && !noteRangeLabel.getBounds().isEmpty())
    return getLocalArea(this, noteRangeLabel.getBounds());
  return getLocalArea(this, stripMenuBtn.getBounds());
}

void RackSlotComponent::mouseDoubleClick(const juce::MouseEvent &e) {
  if (!isCollapsed && e.y >= 105 && e.y <= 140) {
    showRenameDialog();
    return;
  }
  if (onOpenEditor)
    onOpenEditor(0);
}

void RackSlotComponent::mouseEnter(const juce::MouseEvent &) {
  if (isCollapsed && isUnusedEmptyStrip())
    repaint();
}

void RackSlotComponent::mouseExit(const juce::MouseEvent &) {
  if (isCollapsed && isUnusedEmptyStrip())
    repaint();
}

void RackSlotComponent::mouseMove(const juce::MouseEvent &e) {
  if (isCollapsed && isUnusedEmptyStrip()) {
    setMouseCursor(juce::MouseCursor::PointingHandCursor);
  } else if (!isCollapsed && e.x >= getWidth() - 6) {
    setMouseCursor(juce::MouseCursor::LeftRightResizeCursor);
  } else {
    setMouseCursor(juce::MouseCursor::NormalCursor);
  }
}

void RackSlotComponent::mouseWheelMove(const juce::MouseEvent &e,
                                       const juce::MouseWheelDetails &wheel) {
  if (onHorizontalScroll) {
    if (std::abs(wheel.deltaX) > 0.001f) {
      onHorizontalScroll(juce::roundToInt(-wheel.deltaX * 260.0f));
      return;
    }
    const auto rel = e.getEventRelativeTo(this);
    if (isCollapsed || e.mods.isShiftDown() || rel.y < 145) {
      if (std::abs(wheel.deltaY) > 0.001f) {
        onHorizontalScroll(juce::roundToInt(-wheel.deltaY * 220.0f));
        return;
      }
    }
  }
  juce::Component::mouseWheelMove(e, wheel);
}

void RackSlotComponent::mouseDown(const juce::MouseEvent &e) {
  if (isCollapsed) {
    if (isUnusedEmptyStrip() && e.mods.isRightButtonDown() &&
        onRightClickUnusedEmpty) {
      onRightClickUnusedEmpty();
      return;
    }
    if (!e.mods.isRightButtonDown() || isUnusedEmptyStrip()) {
      setCollapsed(false);
    }
    return;
  }

  // Right-edge grab handle to resize expanded channel strip width
  if (!isCollapsed && e.x >= getWidth() - 6 && !e.mods.isRightButtonDown()) {
    isDraggingRightEdge = true;
    if (onStartDragWidth)
      onStartDragWidth();
    return;
  }

  if (e.mods.isRightButtonDown()) {
    if (e.eventComponent != nullptr && e.eventComponent != this) {
      showButtonHelpPopup(e.eventComponent);
      return;
    }
    if (ThemeManager::isStudio() && !isReturn) {
      showStripActionsMenu();
      return;
    }
    if (e.y < 140) {
      if (onShowChannelStrip)
        onShowChannelStrip();
    }
    return;
  }
  if (e.eventComponent == this && e.y <= 8) {
    showColorPalette();
  }
}

void RackSlotComponent::mouseDrag(const juce::MouseEvent &e) {
  if (isDraggingRightEdge) {
    if (onDragWidth)
      onDragWidth(e.getDistanceFromDragStartX());
  }
}

void RackSlotComponent::mouseUp(const juce::MouseEvent &) {
  if (isDraggingRightEdge) {
    isDraggingRightEdge = false;
  }
}

void RackSlotComponent::showButtonHelpPopup(juce::Component *src) {
  juce::String title, description;

  auto match = [&](juce::Component &btn, const juce::String &t,
                   const juce::String &d) -> bool {
    if (src == &btn) {
      title = t;
      description = d;
      return true;
    }
    return false;
  };

  if (!(match(ccButton, "CC Parameter Mappings",
              "Opens the assignment manager for mapping MIDI continuous "
              "controllers to plugin parameters on this channel.") ||
        match(noteRangeButton, "Note Range Filter",
              "Restricts this channel strip to a specific key range on your "
              "controller. Notes outside the range are ignored.") ||
        match(customizeButton, "Dynamics Section (DYN)",
              "Opens the channel strip processor: Noise Gate, 10-band EQ, "
              "Compressor, and Aux Send routing for spatial mixing.") ||
        match(arpButton, "MIDI FX (Arpeggiator)",
              "Opens the built-in arpeggiator and harmonizer controls for "
              "this channel.") ||
        match(samplerButton, "Sampler (SMP)",
              "Opens the sample playback engine, allowing you to load and "
              "trigger audio samples across the keyboard.") ||
        match(mp3Button, "MP3 Playlist",
              "Opens the MP3 playlist engine for between-set music playback.") ||
        match(bypassButton, "Mute Strip",
              "Instantly cuts all audio output for this track. Click again "
              "to restore. Hold to learn a mute CC.") ||
        match(fohRoutingBtn, "FOH Send",
              "Routes this channel's signal to the Front of House mix "
              "(audience speakers). Hold to learn a FOH fader CC.") ||
        match(iemRoutingBtn, "IEM Send",
              "Routes this channel's signal to your In-Ear Monitor mix "
              "(personal headphones). Hold to learn an IEM fader CC.") ||
        match(saveStripBtn, "Save Strip",
              "Saves this entire channel strip configuration to a file for "
              "reuse in other songs.") ||
        match(loadStripBtn, "Load Strip",
              "Loads a previously saved channel strip from a file or the "
              "library sidebar.")))
    return;

  juce::PopupMenu menu;
  menu.addItem(1, "What is this?");
  menu.showMenuAsync(
      juce::PopupMenu::Options().withTargetComponent(src),
      [title, description](int result) {
        if (result == 1) {
          juce::AlertWindow::showMessageBoxAsync(
              juce::MessageBoxIconType::InfoIcon, title, description, "Got it");
        }
      });
}

const juce::Array<juce::Colour> &RackSlotComponent::getPalette() {
  // Stage-tape tones matched to the Tablet HUD ZONE_COLORS
  static const juce::Array<juce::Colour> p = {
      juce::Colour(0xff2a2a2a), // Default
      juce::Colour(0xff3d8fb8), // Stage Blue
      juce::Colour(0xff4a9a68), // Sage Green
      juce::Colour(0xffc98a2e), // Warm Amber
      juce::Colour(0xffb8564f), // Coral Red
      juce::Colour(0xff8668b0), // Muted Violet
      juce::Colour(0xff3a9a8c), // Deep Teal
      juce::Colour(0xffb59b3b), // Brass Gold
      juce::Colour(0xff6e7f8d), // Slate Steel
      juce::Colour(0xffa86488)  // Rose Tape
  };
  return p;
}

void RackSlotComponent::showColorPalette() {
  static const char *names[] = {
      "Default",     "Stage Blue",   "Sage Green", "Warm Amber", "Coral Red",
      "Muted Violet","Deep Teal",    "Brass Gold", "Slate Steel","Rose Tape"};
  juce::PopupMenu menu;
  menu.addSectionHeader("Strip Stage-Tape Color");
  const auto &pal = getPalette();
  for (int i = 0; i < pal.size(); ++i) {
    juce::String label = (i < 10) ? juce::String(names[i]) : pal[i].toDisplayString(true);
    menu.addItem(i + 1, label, true, slot.getChannelColor() == pal[i]);
  }

  juce::Component::SafePointer<RackSlotComponent> safe(this);
  menu.showMenuAsync(juce::PopupMenu::Options(),
                     [safe, pal](int result) {
                       if (safe == nullptr || result <= 0)
                         return;
                       safe->slot.setChannelColor(pal[result - 1]);
                       safe->repaint();
                     });
}

void RackSlotComponent::showRenameDialog() {
  auto *alertWindow = new juce::AlertWindow(
      "Rename Strip",
      "Enter a new name for this strip:", juce::MessageBoxIconType::NoIcon);
  alertWindow->addTextEditor("name", slot.getName(), "Name:");
  alertWindow->addButton("OK", 1, juce::KeyPress(juce::KeyPress::returnKey));
  alertWindow->addButton("Cancel", 0,
                         juce::KeyPress(juce::KeyPress::escapeKey));

  juce::Component::SafePointer<RackSlotComponent> safe(this);
  alertWindow->enterModalState(
      true,
      juce::ModalCallbackFunction::create([safe, alertWindow](int result) {
        if (safe != nullptr && result == 1) {
          auto newName = alertWindow->getTextEditorContents("name");
          safe->slot.setName(newName);
          if (safe->onRename) {
            safe->onRename();
          }
          safe->repaint();
        }
        delete alertWindow;
      }));
}

void RackSlotComponent::showStripActionsMenu() {
  juce::PopupMenu menu;
  const juce::String hdr = slot.getName().isNotEmpty()
                               ? slot.getName()
                               : ("Strip " + juce::String(slotIndex + 1));
  menu.addSectionHeader(hdr);
  menu.addItem(1, "Note range & transpose...");
  if (!isMonitorIn) {
    menu.addItem(2, "Dynamics, EQ & sends (DYN)...");
    menu.addItem(3, "MIDI effects (Arp / Harmonizer)...");
    menu.addItem(4, "8-pad sampler...");
    menu.addItem(5, "BLEEP-216 synth...");
    if (kMp3DecksEnabled)
      menu.addItem(6, "MP3 playlist deck...");
  }
  menu.addItem(7, "CC parameter mappings...");
  menu.addSeparator();
  menu.addItem(8, "Save strip preset...");
  menu.addItem(9, "Load strip preset...");
  menu.addSeparator();
  menu.addItem(10, "Rename strip...");
  menu.addItem(11, "Strip colour...");

  juce::Component::SafePointer<RackSlotComponent> safe(this);
  menu.showMenuAsync(
      juce::PopupMenu::Options().withTargetComponent(stripMenuBtn.isVisible() ? (juce::Component*)&stripMenuBtn : this),
      [safe](int r) {
        if (safe == nullptr || r == 0)
          return;
        switch (r) {
          case 1: if (safe->onShowNoteRangeDialog) safe->onShowNoteRangeDialog(); break;
          case 2: if (safe->onShowChannelStrip) safe->onShowChannelStrip(); break;
          case 3: if (safe->onShowArpeggiator) safe->onShowArpeggiator(); break;
          case 4: if (safe->onShowSampler) safe->onShowSampler(); break;
          case 5: if (safe->onShowSynth) safe->onShowSynth(); break;
          case 6: if (safe->onShowMp3Player) safe->onShowMp3Player(); break;
          case 7: if (safe->onShowCCDialog) safe->onShowCCDialog(0); break;
          case 8: if (safe->onSaveStrip) safe->onSaveStrip(); break;
          case 9: safe->showLoadStripMenu(); break;
          case 10: safe->showRenameDialog(); break;
          case 11: safe->showColorPalette(); break;
          default: break;
        }
      });
}

void RackSlotComponent::showLoadStripMenu() {
  auto stripsDir = FanfareConstants::getAppDirectory().getChildFile("strips");
  juce::Array<juce::File> stripFiles;
  if (stripsDir.isDirectory())
    stripsDir.findChildFiles(stripFiles, juce::File::findFiles, false,
                             "*.orstrip");
  stripFiles.sort();

  if (stripFiles.isEmpty()) {
    if (onLoadStrip)
      onLoadStrip();
    return;
  }

  juce::PopupMenu menu;
  int id = 1;
  for (const auto &f : stripFiles)
    menu.addItem(id++, f.getFileNameWithoutExtension());
  menu.addSeparator();
  menu.addItem(999, "Browse for file...");

  auto filesCopy = stripFiles;
  juce::Component::SafePointer<RackSlotComponent> safe(this);
  menu.showMenuAsync(juce::PopupMenu::Options(),
                     [safe, filesCopy](int result) {
                       if (safe == nullptr || result == 0)
                         return;
                       if (result == 999) {
                         if (safe->onLoadStrip)
                           safe->onLoadStrip();
                         return;
                       }
                       int idx = result - 1;
                       if (idx >= 0 && idx < filesCopy.size()) {
                         if (safe->onLoadStripFile)
                           safe->onLoadStripFile(filesCopy[idx]);
                       }
                     });
}

bool RackSlotComponent::isInterestedInDragSource(
    const juce::DragAndDropTarget::SourceDetails &details) {
  if (slot.isBypassed())
    return false;
  return details.description.toString().endsWithIgnoreCase(".orstrip");
}

void RackSlotComponent::itemDropped(
    const juce::DragAndDropTarget::SourceDetails &details) {
  juce::File presetFile(details.description.toString());
  if (presetFile.existsAsFile() && onLoadPresetFile)
    onLoadPresetFile(presetFile);
}

void RackSlotComponent::showPluginConfigMenu(int chainIndex) {
  if (chainIndex < 0 || chainIndex >= slot.getChainSize())
    return;
  if (!slot.getPluginInstance(chainIndex) && !slot.isChainSlotMidiOut(chainIndex))
    return;

  auto *comp = new PluginStackConfigComp(slot, chainIndex);

  if (auto *mainComp = findParentComponentOfClass<MainComponent>()) {
    mainComp->setActiveMidiNoteLearner(comp);
  }

  juce::CallOutBox::launchAsynchronously(
      std::unique_ptr<juce::Component>(comp),
      slotBtns[chainIndex].getScreenBounds(), nullptr);
}

void RackSlotComponent::SlotBtnMouseListener::mouseDown(const juce::MouseEvent &e) {
  if (!owner || !e.mods.isRightButtonDown())
    return;
  auto *src = e.originalComponent != nullptr ? e.originalComponent : e.eventComponent;
  for (int i = 0; i < 3; ++i) {
    for (auto *c = src; c != nullptr; c = c->getParentComponent()) {
      if (c == &owner->slotBtns[i]) {
        owner->showPluginConfigMenu(i);
        return;
      }
    }
  }
}

void RackSlotComponent::NoteRangeLabelListener::mouseDown(const juce::MouseEvent &e) {
  leftPressArmed = (owner != nullptr &&
                    e.originalComponent == &owner->noteRangeLabel &&
                    e.mods.isLeftButtonDown() &&
                    !e.mods.isPopupMenu() &&
                    e.getNumberOfClicks() == 1);
}

void RackSlotComponent::NoteRangeLabelListener::mouseUp(const juce::MouseEvent &e) {
  const bool wasArmed = leftPressArmed;
  leftPressArmed = false;
  if (!wasArmed || owner == nullptr || !owner->onShowNoteRangeDialog)
    return;
  const auto rel = e.getEventRelativeTo(&owner->noteRangeLabel);
  if (!owner->noteRangeLabel.getLocalBounds().contains(rel.getPosition()))
    return;
  const juce::int64 nowMs = juce::Time::getCurrentTime().toMilliseconds();
  if (nowMs - lastOpenMs < 300)
    return;
  lastOpenMs = nowMs;
  owner->onShowNoteRangeDialog();
}

void RackSlotComponent::LongPressMouseListener::mouseDown(const juce::MouseEvent &e) {
  if (!owner)
    return;
  fired = false;
  holdMs = 0;
  owner->suppressLongPressClick = false;
  if (e.eventComponent == &owner->bypassButton ||
      e.eventComponent == &owner->collapsedMuteBtn)
    armTarget = 3; // mute
  else if (e.eventComponent == &owner->fohRoutingBtn)
    armTarget = 1; // FOH fader
  else
    armTarget = 2; // IEM fader
  startTimer(50);
}

void RackSlotComponent::LongPressMouseListener::mouseDrag(const juce::MouseEvent &e) {
  (void)e;
  stopTimer();
}

void RackSlotComponent::LongPressMouseListener::mouseUp(const juce::MouseEvent &e) {
  (void)e;
  stopTimer();
  // Component::mouseUp (and any synchronous Button::onClick) has already run
  // before MouseListener::mouseUp. If the pointer was released outside the
  // button so onClick did not fire, clear suppressLongPressClick and resync
  // button toggle states so the flag never eats a subsequent click.
  if (owner != nullptr && owner->suppressLongPressClick) {
    owner->suppressLongPressClick = false;
    owner->fohRoutingBtn.setToggleState(owner->slot.isFohEnabled(), juce::dontSendNotification);
    owner->iemRoutingBtn.setToggleState(owner->slot.isIemEnabled(), juce::dontSendNotification);
    owner->bypassButton.setToggleState(owner->slot.isBypassed(), juce::dontSendNotification);
    owner->collapsedMuteBtn.setToggleState(owner->slot.isBypassed(), juce::dontSendNotification);
  }
  fired = false;
}

void RackSlotComponent::LongPressMouseListener::timerCallback() {
  holdMs += 50;
  if (holdMs >= 600 && !fired) {
    fired = true;
    stopTimer();
    owner->longPressLearnArmed(armTarget);
  }
}

void RackSlotComponent::longPressLearnArmed(int armTarget) {
  suppressLongPressClick = true;
  if (onShowCCDialog)
    onShowCCDialog(armTarget);
}

juce::String RackSlotComponent::getActiveNoteRangeString() const {
  int stripLow = slot.getLowNote();
  int stripHigh = slot.getHighNote();

  int stackLow = 127;
  int stackHigh = 0;
  bool hasInstruments = false;

  for (int i = 0; i < 3; ++i) {
    bool isInst = slot.isChainSlotMidiOut(i);
    if (auto *p = slot.getPluginInstance(i))
      isInst = isInst || p->getPluginDescription().isInstrument;
    if (isInst && slot.getChainSlotSettings(i).enabled.load()) {
      stackLow = juce::jmin(stackLow, slot.getChainSlotSettings(i).lowNote.load());
      stackHigh = juce::jmax(stackHigh, slot.getChainSlotSettings(i).highNote.load());
      hasInstruments = true;
    }
  }

  int activeLow = stripLow;
  int activeHigh = stripHigh;

  if (hasInstruments) {
    activeLow = juce::jmax(stripLow, stackLow);
    activeHigh = juce::jmin(stripHigh, stackHigh);
  }

  if (activeLow == 0 && activeHigh == 127)
    return "C-1 - G9 (Full)";

  return midiNoteName(activeLow) + " - " + midiNoteName(activeHigh);
}

void RackSlotComponent::updateOutputSelector() {
  outputSelector.clear(juce::dontSendNotification);
  outputSelector.addItem("Main FOH/IEM", 1);

  outputSelector.addSeparator();
  if (auto *rootMenu = outputSelector.getRootMenu())
    rootMenu->addSectionHeader("Direct Hardware Outs");
  outputSelector.addItem("Direct: Out 1+2 (Stereo)", 200 + 2);
  outputSelector.addItem("Direct: Out 3+4 (Stereo)", 202 + 2);
  outputSelector.addItem("Direct: Out 5 (Mono)", 104 + 2);
  outputSelector.addItem("Direct: Out 6 (Mono)", 105 + 2);
  outputSelector.addItem("Direct: Out 7 (Mono)", 106 + 2);
  outputSelector.addItem("Direct: Out 8 (Mono)", 107 + 2);
  outputSelector.addItem("Direct: Out 1 (Mono)", 100 + 2);
  outputSelector.addItem("Direct: Out 2 (Mono)", 101 + 2);
  outputSelector.addItem("Direct: Out 3 (Mono)", 102 + 2);
  outputSelector.addItem("Direct: Out 4 (Mono)", 103 + 2);
  outputSelector.addItem("Direct: Out 5+6 (Stereo)", 204 + 2);
  outputSelector.addItem("Direct: Out 7+8 (Stereo)", 206 + 2);

  int numSlots = FanfareConstants::kNumSlots;
  if (slotIndex >= 0 && slotIndex < numSlots) {
    bool headerAdded = false;
    for (int i = slotIndex + 1; i < numSlots; ++i) {
      if (!headerAdded) {
        outputSelector.addSeparator();
        if (auto *rootMenu = outputSelector.getRootMenu())
          rootMenu->addSectionHeader("Subgroup to Strip");
        headerAdded = true;
      }
      juce::String name = "Strip " + juce::String(i + 1);
      if (getSlotName) {
        juce::String customName = getSlotName(i);
        if (customName.isNotEmpty()) {
          name += ": " + customName;
        }
      }
      outputSelector.addItem(name, i + 2);
    }
  }

  int target = slot.getOutputTarget();
  outputSelector.setSelectedId(target + 2, juce::dontSendNotification);
}
