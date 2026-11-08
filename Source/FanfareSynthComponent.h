#pragma once

#include <JuceHeader.h>
#include <array>
#include <map>
#include <vector>
#include "RackSlot.h"
#include "FanfareConstants.h"
#include "MidiLearnBus.h"

// ==============================================================================
// FanfareSynthComponent — modal editor overlay for the native Fanfare synth,
// following the SamplerComponent pattern: owns a RackSlot reference, writes
// straight to the processor's atomics, themed via ThemeManager.
//
// The control grid is generated from FanfareSynthProcessor::paramDefs(), so
// the UI can never drift out of sync with the engine.
//
// The five performance-critical params (cutoff / resonance / attack / release /
// volume) ship with GM-standard CC defaults (74/71/73/72/7) and can be
// re-learned via their small "CC nn" buttons.
// ==============================================================================

namespace SynthUIHelpers {
inline juce::String formatParamValue(const FanfareSynthProcessor::ParamDef &d,
                                     float v) {
  if (d.isPercent)
    return juce::String((int)std::lround(v * 100.0f)) + "%";
  const juce::String unit(d.unit);
  if (unit == "Hz")
    return juce::String((int)std::lround(v)) + " Hz";
  if (unit == "s")
    return juce::String(v, 2) + " s";
  if (unit == "ct")
    return (v > 0 ? "+" : "") + juce::String((int)std::lround(v)) + " ct";
  return juce::String((int)std::lround(v));
}
} // namespace SynthUIHelpers

// One parameter control: rotary knob or enum combo, name label, optional CC tag.
class SynthParamControl : public juce::Component {
public:
  SynthParamControl(FanfareSynthProcessor &proc, int paramIdx, bool vertical = false)
      : procRef(proc), idx(paramIdx), vertical(vertical),
        isEnum(FanfareSynthProcessor::paramDefs()[paramIdx].kind == 1) {
    const auto &d = FanfareSynthProcessor::paramDefs()[idx];

    nameLabel.setText(d.label, juce::dontSendNotification);
    nameLabel.setFont(juce::FontOptions(11.0f));
    nameLabel.setColour(juce::Label::textColourId,
                        ThemeManager::get(Theme::Role::textDim));
    nameLabel.setJustificationType(juce::Justification::centred);
    addAndMakeVisible(nameLabel);

    if (isEnum) {
      combo.addItemList(juce::StringArray::fromTokens(d.choices, ",", ""), 1);
      // Enum params store their RAW value (e.g. octave -2..+2), the combo is
      // 0-based: index = value - min
      combo.setSelectedItemIndex(
          juce::jlimit(0, combo.getNumItems() - 1,
                       (int)proc.getParam(idx) - (int)d.min),
          juce::dontSendNotification);
      combo.onChange = [this, min = (int)d.min] {
        procRef.setParam(idx, (float)(combo.getSelectedItemIndex() + min));
      };
      combo.setTooltip(juce::String(d.label));
      addAndMakeVisible(combo);
    } else {
      juce::NormalisableRange<double> range(d.min, d.max, 0.0, d.skew);
      slider.setNormalisableRange(range);
      // Juno-style: vertical faders on the oscillator and envelope sections,
      // rotary knobs everywhere else. Text formatter must be installed
      // BEFORE the first setValue() or preset values render unformatted.
      slider.setSliderStyle(vertical ? juce::Slider::LinearVertical
                                     : juce::Slider::RotaryHorizontalVerticalDrag);
      // Narrow Juno-style faders carry no text box; a value readout label sits
      // underneath instead (see resized()). Knobs keep their attached readout.
      slider.setTextBoxStyle(vertical ? juce::Slider::NoTextBox
                                      : juce::Slider::TextBoxBelow,
                             false, 46, 14);
      slider.setColour(juce::Slider::rotarySliderFillColourId,
                       ThemeManager::get(Theme::Role::accent));
      slider.setColour(juce::Slider::thumbColourId,
                       ThemeManager::get(Theme::Role::knobThumb));
      slider.setColour(juce::Slider::trackColourId,
                       ThemeManager::get(Theme::Role::accentDim));
      slider.setColour(juce::Slider::textBoxTextColourId,
                       ThemeManager::get(Theme::Role::textDim));
      slider.textFromValueFunction = [this](double v) {
        return SynthUIHelpers::formatParamValue(
            FanfareSynthProcessor::paramDefs()[idx], (float)v);
      };
      slider.setValue(proc.getParam(idx), juce::dontSendNotification);
      slider.updateText();
      slider.setTooltip(juce::String(d.label));
      if (vertical) {
        valueLabel.setText(
            SynthUIHelpers::formatParamValue(
                FanfareSynthProcessor::paramDefs()[idx], proc.getParam(idx)),
            juce::dontSendNotification);
        valueLabel.setFont(juce::FontOptions(10.5f));
        valueLabel.setColour(juce::Label::textColourId,
                             ThemeManager::get(Theme::Role::text));
        valueLabel.setJustificationType(juce::Justification::centred);
        addAndMakeVisible(valueLabel);
      }
      slider.onValueChange = [this] {
        procRef.setParam(idx, (float)slider.getValue());
        if (this->vertical)
          valueLabel.setText(
              SynthUIHelpers::formatParamValue(
                  FanfareSynthProcessor::paramDefs()[idx],
                  (float)slider.getValue()),
              juce::dontSendNotification);
      };
      addAndMakeVisible(slider);
    }

    // Small clickable "CC nn" tag for the mappable performance params
    if (isCCLearnable()) {
      ccBtn.setConnectedEdges(juce::Button::ConnectedOnLeft |
                              juce::Button::ConnectedOnRight);
      ccBtn.setColour(juce::TextButton::buttonColourId,
                      ThemeManager::get(Theme::Role::panel));
      ccBtn.setColour(juce::TextButton::buttonOnColourId,
                      ThemeManager::get(Theme::Role::accent));
      ccBtn.onClick = [this] { showCCMenu(); };
      addAndMakeVisible(ccBtn);
      refreshCC();
    }
  }

  bool isCCLearnable() const {
    auto id = juce::String(FanfareSynthProcessor::paramDefs()[idx].id);
    return id == "cutoff" || id == "reso" || id == "atk" || id == "rel" ||
           id == "volume";
  }

  void syncFromProcessor() {
    const auto &d = FanfareSynthProcessor::paramDefs()[idx];
    if (isEnum) {
      combo.setSelectedItemIndex(
          juce::jlimit(0, combo.getNumItems() - 1,
                       (int)procRef.getParam(idx) - (int)d.min),
          juce::dontSendNotification);
    } else {
      slider.setValue(procRef.getParam(idx), juce::dontSendNotification);
      if (vertical)
        valueLabel.setText(
            SynthUIHelpers::formatParamValue(d, procRef.getParam(idx)),
            juce::dontSendNotification);
    }
  }

  void refreshCC() {
    if (!isCCLearnable())
      return;
    int cc =
        procRef.getCCForParam(FanfareSynthProcessor::paramDefs()[idx].id);
    ccBtn.setButtonText(cc >= 0 ? "CC " + juce::String(cc) : "CC off");
    ccBtn.setColour(juce::TextButton::textColourOffId,
                    cc >= 0 ? ThemeManager::get(Theme::Role::accentDim)
                            : ThemeManager::get(Theme::Role::textFaint));
  }

  void resized() override {
    auto b = getLocalBounds();
    if (ccBtn.isVisible())
      ccBtn.setBounds(b.removeFromBottom(14).reduced(3, 1));
    nameLabel.setBounds(b.removeFromTop(13));
    if (vertical) {
      valueLabel.setBounds(b.removeFromBottom(15));
      slider.setBounds(b.withSizeKeepingCentre(16, b.getHeight()));
    } else if (isEnum) {
      combo.setBounds(b.reduced(2, 10));
    } else {
      slider.setBounds(b);
    }
  }

private:
  void showCCMenu() {
    const auto &d = FanfareSynthProcessor::paramDefs()[idx];
    const juce::String id = d.id;
    const int defCC = defaultCCFor(id);
    juce::PopupMenu m;
    m.addSectionHeader(juce::String(d.label) + " MIDI CC");
    m.addItem(1, "MIDI Learn...", true, learning);
    if (procRef.getCCForParam(id) >= 0)
      m.addItem(2, "Clear mapping");
    m.addSeparator();
    m.addItem(3, "Reset to default (CC " + juce::String(defCC) + ")", true,
              procRef.getCCForParam(id) == defCC);

    m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&ccBtn),
                    [this, id, defCC](int result) {
                      if (result == 1) {
                        if (learning) {
                          Fanfare::MidiLearnBus::getInstance().disarm();
                          learning = false;
                          return;
                        }
                        Fanfare::MidiLearnBus::Target t;
                        t.label = "Synth: " + id;
                        t.targetId = "synth." + id;
                        t.onBind = [this, id](int cc) {
                          procRef.setCCForParam(id, cc);
                          refreshCC();
                          learning = false;
                        };
                        Fanfare::MidiLearnBus::getInstance().arm(t, this);
                        learning = true;
                      } else if (result == 2) {
                        procRef.setCCForParam(id, -1);
                        refreshCC();
                      } else if (result == 3) {
                        procRef.setCCForParam(id, defCC);
                        refreshCC();
                      }
                    });
  }

  static int defaultCCFor(const juce::String &id) {
    if (id == "cutoff")
      return 74;
    if (id == "reso")
      return 71;
    if (id == "atk")
      return 73;
    if (id == "rel")
      return 72;
    if (id == "volume")
      return 7;
    return -1;
  }

  FanfareSynthProcessor &procRef;
  int idx;
  bool vertical;
  bool isEnum;

  juce::Label nameLabel;
  juce::Label valueLabel;
  juce::Slider slider;
  juce::ComboBox combo;
  juce::TextButton ccBtn{"CC"};
  bool learning = false;
};

// A titled, themed panel hosting a grid of SynthParamControls (it owns them).
class SynthSectionPanel : public juce::Component {
public:
  explicit SynthSectionPanel(const juce::String &title) : titleText(title) {}

  void paint(juce::Graphics &g) override {
    auto b = getLocalBounds().toFloat();
    g.setColour(ThemeManager::get(Theme::Role::panelAlt));
    g.fillRoundedRectangle(b, 5.0f);
    g.setColour(ThemeManager::get(Theme::Role::border));
    g.drawRoundedRectangle(b.reduced(0.5f), 5.0f, 1.0f);
    g.setColour(ThemeManager::get(Theme::Role::accent));
    g.setFont(juce::FontOptions(12.0f, juce::Font::bold));
    g.drawText(titleText, getLocalBounds().removeFromTop(18).reduced(6, 2),
               juce::Justification::centredLeft);
  }

  void addControl(SynthParamControl *c, int x, int y) {
    c->setBounds(x, y, kCellW, kCellH);
    addAndMakeVisible(c);
    controls.add(c); // panel owns the control
  }

  static constexpr int kCellW = 50;
  static constexpr int kCellH = 96;

  void syncAll() {
    for (auto *c : controls) {
      c->syncFromProcessor();
      c->refreshCC();
    }
  }

private:
  juce::String titleText;
  juce::OwnedArray<SynthParamControl> controls;
};


// ==============================================================================
// OnscreenKeyboard — piano strip driven by the QWERTY rows (Z-M = lower
// octave, Q-U = upper octave, cursor keys shift octave) so the synth can be
// played without a MIDI controller. Mouse click/drag works too. Notes go to
// the processor via injectNote(); pressed keys are highlighted.
// ==============================================================================
class OnscreenKeyboard : public juce::Component {
public:
  explicit OnscreenKeyboard(FanfareSynthProcessor &p) : proc(p) {
    setWantsKeyboardFocus(true);

    int offset = 0;
    for (const char *row : {rowLo, rowHi}) {
      for (int i = 0; row[i] != 0; ++i)
        keyMap.push_back({(int)std::toupper((unsigned char)row[i]), offset + i});
      offset += 12;
    }

    octDownBtn.setButtonText("OCT -");
    octDownBtn.setColour(juce::TextButton::buttonColourId,
                         ThemeManager::get(Theme::Role::panel));
    octDownBtn.setTooltip("Shift the keyboard down an octave (cursor left)");
    octDownBtn.onClick = [this] { shiftOctave(-12); };
    addAndMakeVisible(octDownBtn);

    octUpBtn.setButtonText("OCT +");
    octUpBtn.setColour(juce::TextButton::buttonColourId,
                       ThemeManager::get(Theme::Role::panel));
    octUpBtn.setTooltip("Shift the keyboard up an octave (cursor right)");
    octUpBtn.onClick = [this] { shiftOctave(12); };
    addAndMakeVisible(octUpBtn);

    panicBtn.setButtonText("ALL OFF");
    panicBtn.setColour(juce::TextButton::buttonColourId,
                       ThemeManager::get(Theme::Role::danger).darker(0.4f));
    panicBtn.setTooltip("Silence all synth voices");
    panicBtn.onClick = [this] {
      releaseAll();
      proc.allNotesOff();
    };
    addAndMakeVisible(panicBtn);
  }

  void shiftOctave(int semis) {
    releaseAll();
    baseNote = juce::jlimit(12, 96, baseNote + semis);
    repaint();
  }

  void paint(juce::Graphics &g) override {
    auto b = getLocalBounds().toFloat();
    g.setColour(ThemeManager::get(Theme::Role::panelAlt));
    g.fillRoundedRectangle(b, 5.0f);
    g.setColour(ThemeManager::get(Theme::Role::border));
    g.drawRoundedRectangle(b.reduced(0.5f), 5.0f, 1.0f);

    g.setColour(ThemeManager::get(Theme::Role::textDim));
    g.setFont(juce::FontOptions(11.0f, juce::Font::bold));
    g.drawText(noteName(baseNote) + juce::String(" - ") + noteName(baseNote + 24) +
                   juce::String("   QWERTY: Z-M lower / Q-U upper, cursor keys = octave"),
               getLocalBounds().reduced(8, 0).removeFromTop(17),
               juce::Justification::centredLeft);

    auto keys = keysArea().toFloat();
    const int numWhites = 15;
    const float wW = keys.getWidth() / (float)numWhites;

    for (int s = 0; s <= 24; ++s) {
      if (isBlackPc(s % 12))
        continue;
      auto r = juce::Rectangle<float>(
          keys.getX() + whiteIndexOf(s) * wW, keys.getY(), wW - 1.0f,
          keys.getHeight());
      const bool on = isNoteOn(baseNote + s);
      g.setColour(on ? ThemeManager::get(Theme::Role::accent).withAlpha(0.75f)
                     : ThemeManager::get(Theme::Role::raised));
      g.fillRoundedRectangle(r, 3.0f);
      g.setColour(ThemeManager::get(Theme::Role::border));
      g.drawRoundedRectangle(r, 3.0f, 1.0f);
      const char ch = charForOffset(s);
      if (ch != 0) {
        g.setColour(on ? ThemeManager::get(Theme::Role::textOnAccent)
                       : ThemeManager::get(Theme::Role::textFaint));
        g.setFont(juce::FontOptions(10.0f));
        g.drawText(juce::String::charToString(
                       (juce::juce_wchar)std::toupper((unsigned char)ch)),
                   r.toNearestInt().removeFromBottom(13),
                   juce::Justification::centred);
      }
    }

    for (int s = 0; s <= 24; ++s) {
      if (!isBlackPc(s % 12))
        continue;
      const float bx =
          keys.getX() + (float)whiteIndexOf(s) * wW - wW * 0.3f;
      auto r = juce::Rectangle<float>(bx, keys.getY(), wW * 0.6f,
                                      keys.getHeight() * 0.58f);
      const bool on = isNoteOn(baseNote + s);
      g.setColour(on ? ThemeManager::get(Theme::Role::accent)
                     : juce::Colours::black);
      g.fillRoundedRectangle(r, 2.0f);
    }
  }

  void resized() override {
    auto b = getLocalBounds().reduced(4);
    panicBtn.setBounds(b.removeFromRight(58).removeFromTop(18).reduced(1));
    octUpBtn.setBounds(b.removeFromRight(44).removeFromTop(18).reduced(1));
    octDownBtn.setBounds(b.removeFromRight(44).removeFromTop(18).reduced(1));
  }

  bool keyPressed(const juce::KeyPress &key) override {
    if (key.isKeyCode(juce::KeyPress::leftKey)) {
      shiftOctave(-12);
      return true;
    }
    if (key.isKeyCode(juce::KeyPress::rightKey)) {
      shiftOctave(12);
      return true;
    }
    const int code = normalizeCode(key.getKeyCode());
    for (auto &km : keyMap) {
      if (km.first != code)
        continue;
      if (keyHeldByCode.find(km.first) == keyHeldByCode.end()) {
        const int n = baseNote + km.second;
        keyHeldByCode[km.first] = n;
        proc.injectNote(n, true);
        repaint();
      }
      return true;
    }
    return false;
  }

  bool keyStateChanged(bool /*isKeyDown*/) override {
    // Poll every mapped key: self-heals missed key-ups after focus switches
    for (auto &km : keyMap) {
      const bool down = juce::KeyPress::isKeyCurrentlyDown(km.first);
      auto it = keyHeldByCode.find(km.first);
      if (down && it == keyHeldByCode.end()) {
        const int n = baseNote + km.second;
        keyHeldByCode[km.first] = n;
        proc.injectNote(n, true);
        repaint();
      } else if (!down && it != keyHeldByCode.end()) {
        proc.injectNote(it->second, false);
        keyHeldByCode.erase(it);
        repaint();
      }
    }
    return !keyHeldByCode.empty();
  }

  void focusLost(juce::Component::FocusChangeType) override { releaseAll(); }

  // Public escape hatch for the owner's destructor (releaseAll is private)
  void releaseAllNotes() { releaseAll(); }

  void mouseDown(const juce::MouseEvent &e) override {
    grabKeyboardFocus();
    mouseNote = noteFromPos(e.position);
    if (mouseNote >= 0)
      proc.injectNote(mouseNote, true);
    repaint();
  }

  void mouseDrag(const juce::MouseEvent &e) override {
    const int n = noteFromPos(e.position);
    if (n != mouseNote) {
      if (mouseNote >= 0)
        proc.injectNote(mouseNote, false);
      mouseNote = n;
      if (n >= 0)
        proc.injectNote(n, true);
      repaint();
    }
  }

  void mouseUp(const juce::MouseEvent &) override {
    if (mouseNote >= 0)
      proc.injectNote(mouseNote, false);
    mouseNote = -1;
    repaint();
  }

  void mouseExit(const juce::MouseEvent &) override {
    if (mouseNote >= 0) {
      proc.injectNote(mouseNote, false);
      mouseNote = -1;
      repaint();
    }
  }

private:
  FanfareSynthProcessor &proc;
  int baseNote = 48; // C3 — leftmost visible key
  int mouseNote = -1;
  std::vector<std::pair<int, int>> keyMap; // keycode -> semitone offset
  std::map<int, int> keyHeldByCode;        // keycode -> sounding note

  juce::TextButton octDownBtn, octUpBtn, panicBtn;

  static inline const char *rowLo = "zsxdcvgbhnjm"; // Z-M: base octave
  static inline const char *rowHi = "q2w3er5t6y7ui"; // Q-U: base+12 octave

  static bool isBlackPc(int pc) {
    return pc == 1 || pc == 3 || pc == 6 || pc == 8 || pc == 10;
  }

  static int normalizeCode(int code) {
    return (code >= 'a' && code <= 'z') ? code - 32 : code;
  }

  static char charForOffset(int s) {
    if (s >= 0 && s < 12)
      return rowLo[s];
    if (s >= 12 && s < 24)
      return rowHi[s - 12];
    return 0;
  }

  static int whiteIndexOf(int s) {
    int w = 0;
    for (int i = 0; i < s; ++i)
      if (!isBlackPc(i % 12))
        ++w;
    return w;
  }

  static juce::String noteName(int midi) {
    static const char *names[] = {"C",  "C#", "D",  "D#", "E",  "F",
                                  "F#", "G",  "G#", "A",  "A#", "B"};
    return juce::String(names[midi % 12]) + juce::String(midi / 12 - 1);
  }

  juce::Rectangle<int> keysArea() const {
    return getLocalBounds().reduced(6).withTrimmedTop(18);
  }

  bool isNoteOn(int n) const {
    if (n == mouseNote)
      return true;
    for (auto &kv : keyHeldByCode)
      if (kv.second == n)
        return true;
    return false;
  }

  int noteFromPos(juce::Point<float> p) const {
    auto keys = keysArea().toFloat();
    if (!keys.contains(p))
      return -1;
    const int numWhites = 15;
    const float wW = keys.getWidth() / (float)numWhites;
    if (p.y < keys.getY() + keys.getHeight() * 0.58f) {
      for (int s = 0; s <= 24; ++s) {
        if (!isBlackPc(s % 12))
          continue;
        const float bx =
            keys.getX() + (float)whiteIndexOf(s) * wW - wW * 0.3f;
        if (p.x >= bx && p.x < bx + wW * 0.6f)
          return baseNote + s;
      }
    }
    int wi = juce::jlimit(0, numWhites - 1,
                          (int)((p.x - keys.getX()) / wW));
    int w = 0;
    for (int s = 0; s <= 24; ++s) {
      if (!isBlackPc(s % 12)) {
        if (w == wi)
          return baseNote + s;
        ++w;
      }
    }
    return -1;
  }

  void releaseAll() {
    for (auto &kv : keyHeldByCode)
      proc.injectNote(kv.second, false);
    keyHeldByCode.clear();
    if (mouseNote >= 0) {
      proc.injectNote(mouseNote, false);
      mouseNote = -1;
    }
    repaint();
  }

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(OnscreenKeyboard)
};

class FanfareSynthComponent : public juce::Component {
public:
  FanfareSynthComponent(RackSlot &slot, std::function<void()> onClose)
      : slotRef(slot), synth(slot.getSynth()), closeCallback(onClose),
        keyboard(synth) {
    titleLabel.setText("FANFARE SYNTH", juce::dontSendNotification);
    titleLabel.setFont(juce::FontOptions(17.0f, juce::Font::bold));
    titleLabel.setColour(juce::Label::textColourId,
                         ThemeManager::get(Theme::Role::accent));
    addAndMakeVisible(titleLabel);

    enableBtn.setButtonText(synth.enabled.load() ? "SYNTH: ON" : "SYNTH: OFF");
    enableBtn.setClickingTogglesState(true);
    enableBtn.setToggleState(synth.enabled.load(), juce::dontSendNotification);
    enableBtn.setColour(juce::TextButton::buttonOnColourId,
                        ThemeManager::get(Theme::Role::ok));
    enableBtn.setTooltip("Enable the native synth on this channel strip");
    enableBtn.onClick = [this] {
      bool on = enableBtn.getToggleState();
      synth.enabled.store(on);
      enableBtn.setButtonText(on ? "SYNTH: ON" : "SYNTH: OFF");
    };
    addAndMakeVisible(enableBtn);

    // --- Preset browser ---
    presetBox.setJustificationType(juce::Justification::centredLeft);
    presetBox.setTextWhenNothingSelected("Select preset...");
    presetBox.setTooltip("Factory + user presets (pads / leads / basses / keys)");
    presetBox.onChange = [this] { applySelectedPreset(); };
    addAndMakeVisible(presetBox);

    prevBtn.setButtonText("<");
    prevBtn.setTooltip("Previous preset");
    prevBtn.onClick = [this] { stepPreset(-1); };
    addAndMakeVisible(prevBtn);

    nextBtn.setButtonText(">");
    nextBtn.setTooltip("Next preset");
    nextBtn.onClick = [this] { stepPreset(1); };
    addAndMakeVisible(nextBtn);

    saveBtn.setButtonText("SAVE");
    saveBtn.setTooltip("Save the current patch as a user preset");
    saveBtn.onClick = [this] { saveUserPreset(); };
    addAndMakeVisible(saveBtn);

    delBtn.setButtonText("DEL");
    delBtn.setTooltip("Delete the selected user preset");
    delBtn.setColour(juce::TextButton::buttonColourId,
                     ThemeManager::get(Theme::Role::danger).darker(0.4f));
    delBtn.onClick = [this] { deleteUserPreset(); };
    addAndMakeVisible(delBtn);

    closeBtn.onClick = [this] {
      if (closeCallback)
        closeCallback();
    };
    addAndMakeVisible(closeBtn);

    buildSections();
    refreshPresetList();
    selectInitialPreset();

    hintLabel.setText(
        "Unison forces mono \u2022 Glide applies in Mono/Legato \u2022 "
        "Default CCs: cutoff 74, resonance 71, attack 73, release 72, volume 7",
        juce::dontSendNotification);
    hintLabel.setFont(juce::FontOptions(11.0f));
    hintLabel.setColour(juce::Label::textColourId,
                        ThemeManager::get(Theme::Role::textFaint));
    hintLabel.setJustificationType(juce::Justification::centred);
    addAndMakeVisible(hintLabel);
  }

  ~FanfareSynthComponent() override {
    keyboard.releaseAllNotes();
    synth.allNotesOff();
    Fanfare::MidiLearnBus::getInstance().disarmIfOwner(this);
  }

  void visibilityChanged() override {
    if (isVisible())
      keyboard.grabKeyboardFocus();
  }

  // ===== Layout ===============================================================

  static constexpr int kCellW = 50;
  static constexpr int kCellH = 96;
  static constexpr int kHeaderH = 34;
  static constexpr int kPanelPad = 6;

  void buildSections() {
    // Group param indices by section, preserving table order
    std::array<std::vector<int>, FanfareSynthProcessor::kNumSections> bySec;
    const auto &defs = FanfareSynthProcessor::paramDefs();
    for (int i = 0; i < FanfareSynthProcessor::kNumParams; ++i)
      bySec[defs[i].section].push_back(i);

    static const char *sectionNames[] = {
        "OSCILLATORS", "FILTER",     "AMP ENVELOPE", "FILTER ENVELOPE",
        "LFO",         "PERFORMANCE", "CHORUS",      "DELAY",
        "REVERB",      "OUTPUT"};

    const int rowA[] = {FanfareSynthProcessor::SecOsc,
                        FanfareSynthProcessor::SecFilter,
                        FanfareSynthProcessor::SecAmpEnv,
                        FanfareSynthProcessor::SecFltEnv};
    const int rowB[] = {FanfareSynthProcessor::SecLfo,
                        FanfareSynthProcessor::SecPerf,
                        FanfareSynthProcessor::SecChorus,
                        FanfareSynthProcessor::SecDelay,
                        FanfareSynthProcessor::SecReverb,
                        FanfareSynthProcessor::SecMaster};

    for (int sec : rowA)
      addSection(sec, bySec[(size_t)sec], sectionNames[sec]);
    for (int sec : rowB)
      addSection(sec, bySec[(size_t)sec], sectionNames[sec]);
  }

  void resized() override {
    auto b = getLocalBounds().reduced(10);

    auto top = b.removeFromTop(kHeaderH);
    closeBtn.setBounds(top.removeFromRight(30).reduced(1));
    delBtn.setBounds(top.removeFromRight(38).reduced(1));
    saveBtn.setBounds(top.removeFromRight(48).reduced(1));
    nextBtn.setBounds(top.removeFromRight(26).reduced(1));
    prevBtn.setBounds(top.removeFromRight(26).reduced(1));
    presetBox.setBounds(top.removeFromRight(250).reduced(1, 2));
    enableBtn.setBounds(top.removeFromRight(110).reduced(1));
    titleLabel.setBounds(top);

    b.removeFromTop(4);
    auto hint = b.removeFromBottom(16);
    hintLabel.setBounds(hint);
    b.removeFromBottom(2);
    keyboard.setBounds(b.removeFromBottom(106));
    b.removeFromBottom(2);

    // Row A: OSC | FILTER | AMP ENV | FILTER ENV
    const juce::Point<int> origin = b.getPosition();
    int x = 0;
    const int secA[] = {FanfareSynthProcessor::SecOsc,
                        FanfareSynthProcessor::SecFilter,
                        FanfareSynthProcessor::SecAmpEnv,
                        FanfareSynthProcessor::SecFltEnv};
    int rowAHeight = 0;
    for (int sec : secA) {
      if (auto *p = panels[(size_t)sec]) {
        p->setTopLeftPosition(origin.x + x, origin.y);
        x += p->getWidth() + 8;
        rowAHeight = juce::jmax(rowAHeight, p->getHeight());
      }
    }

    // Row B: LFO | PERFORMANCE | CHORUS | DELAY | REVERB | OUTPUT
    x = 0;
    const int secB[] = {FanfareSynthProcessor::SecLfo,
                        FanfareSynthProcessor::SecPerf,
                        FanfareSynthProcessor::SecChorus,
                        FanfareSynthProcessor::SecDelay,
                        FanfareSynthProcessor::SecReverb,
                        FanfareSynthProcessor::SecMaster};
    for (int sec : secB) {
      if (auto *p = panels[(size_t)sec]) {
        p->setTopLeftPosition(origin.x + x, origin.y + rowAHeight + 8);
        x += p->getWidth() + 8;
      }
    }
  }

  void paint(juce::Graphics &g) override {
    auto bounds = getLocalBounds().toFloat();
    g.fillAll(ThemeManager::get(Theme::Role::background));
    g.setColour(ThemeManager::get(Theme::Role::border));
    g.drawRoundedRectangle(bounds.reduced(0.5f), 8.0f, 1.0f);
  }

  // ===== Presets ==============================================================

  struct PresetRef {
    bool isUser = false;
    int factoryIndex = -1;
    juce::String userName;
    juce::String name;
  };

  void refreshPresetList() {
    presetBox.clear(juce::dontSendNotification);
    presetList.clear();

    const auto &factory = FanfareSynthProcessor::factoryPresets();
    juce::String lastCat;
    for (int i = 0; i < factory.size(); ++i) {
      auto cat = factory[i].getProperty("cat", "Patches").toString();
      if (cat != lastCat) {
        presetBox.addSectionHeading(cat);
        lastCat = cat;
      }
      auto name = factory[i].getProperty("name", "?").toString();
      presetBox.addItem(name, (int)presetList.size() + 1);
      PresetRef ref;
      ref.factoryIndex = i;
      ref.name = name;
      presetList.add(ref);
    }

    userPresetDir =
        FanfareConstants::getAppDirectory().getChildFile("synth-presets");
    if (userPresetDir.isDirectory()) {
      auto files =
          userPresetDir.findChildFiles(juce::File::findFiles, false, "*.json");
      files.sort();
      if (files.size() > 0)
        presetBox.addSectionHeading("User");
      for (auto &f : files) {
        presetBox.addItem(f.getFileNameWithoutExtension(),
                          (int)presetList.size() + 1);
        PresetRef ref;
        ref.isUser = true;
        ref.userName = f.getFullPathName();
        ref.name = f.getFileNameWithoutExtension();
        presetList.add(ref);
      }
    }

    delBtn.setEnabled(currentIsUserPreset());
  }

  void selectInitialPreset() {
    for (int i = 0; i < presetList.size(); ++i) {
      if (presetList[i].name == "Analog Strings") {
        presetBox.setSelectedItemIndex(i, juce::dontSendNotification);
        applySelectedPreset();
        return;
      }
    }
    applyPresetByName("Init Patch");
  }

  void applySelectedPreset() {
    int sel = presetBox.getSelectedItemIndex();
    if (sel < 0 || sel >= presetList.size())
      return;
    const auto &ref = presetList.getReference(sel);
    if (ref.isUser) {
      auto parsed = juce::JSON::parse(juce::File(ref.userName));
      if (parsed.isObject())
        synth.applyState(parsed);
    } else {
      synth.applyPreset(
          FanfareSynthProcessor::factoryPresets()[ref.factoryIndex]);
    }
    // Selecting a preset is an intent to hear it
    synth.enabled.store(true);
    enableBtn.setButtonText("SYNTH: ON");
    enableBtn.setToggleState(true, juce::dontSendNotification);
    syncAllControls();
    delBtn.setEnabled(currentIsUserPreset());
  }

  void applyPresetByName(const juce::String &name) {
    for (int i = 0; i < presetList.size(); ++i) {
      if (presetList[i].name == name) {
        presetBox.setSelectedItemIndex(i, juce::dontSendNotification);
        applySelectedPreset();
        return;
      }
    }
  }

  void stepPreset(int dir) {
    int n = presetList.size();
    if (n == 0)
      return;
    int sel = presetBox.getSelectedItemIndex();
    if (sel < 0)
      sel = 0;
    sel = (sel + dir + n) % n;
    presetBox.setSelectedItemIndex(sel, juce::sendNotificationSync);
  }

  void saveUserPreset() {
    juce::Component::SafePointer<FanfareSynthComponent> safeThis(this);
    auto *w = new juce::AlertWindow("Save Synth Preset",
                                    "Name for this patch:",
                                    juce::MessageBoxIconType::NoIcon, this);
    w->addTextEditor("name", "My Patch");
    w->addButton("Save", 1, juce::KeyPress(juce::KeyPress::returnKey));
    w->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
    w->enterModalState(
        true,
        juce::ModalCallbackFunction::create([safeThis, w](int result) {
          juce::String name = w->getTextEditorContents("name").trim();
          delete w; // standard JUCE pattern: delete inside the modal callback
          if (result != 1 || name.isEmpty() || safeThis == nullptr)
            return;
          auto dir = FanfareConstants::getAppDirectory()
                         .getChildFile("synth-presets");
          if (!dir.exists())
            dir.createDirectory();
          auto state = safeThis->synth.getState();
          if (auto *obj = state.getDynamicObject()) {
            obj->setProperty("enabled", juce::var(true));
            obj->setProperty("presetName", juce::String(name));
          }
          auto file =
              dir.getChildFile(juce::File::createLegalFileName(name) + ".json");
          file.replaceWithText(juce::JSON::toString(state));
          safeThis->refreshPresetList();
          safeThis->applyPresetByName(name);
        }));
  }

  void deleteUserPreset() {
    int sel = presetBox.getSelectedItemIndex();
    if (sel < 0 || sel >= presetList.size())
      return;
    const auto &ref = presetList.getReference(sel);
    if (!ref.isUser)
      return;
    juce::File(ref.userName).deleteFile();
    refreshPresetList();
    presetBox.setSelectedItemIndex(0, juce::sendNotificationSync);
  }

  void syncAllControls() {
    for (auto *p : panels)
      if (p != nullptr)
        p->syncAll();
  }

private:
  void addSection(int sec, std::vector<int> &params, const char *name) {
    auto *panel = new SynthSectionPanel(name);
    // Column-major fill, 2 rows of cells per section band. The two envelope
    // sections are special-cased Juno-style: ADSR as four narrow faders side
    // by side on the top row (velocity depth as a knob below right on AMP).
    const int rows = 2;
    const bool vertical = (sec == FanfareSynthProcessor::SecOsc ||
                           sec == FanfareSynthProcessor::SecAmpEnv ||
                           sec == FanfareSynthProcessor::SecFltEnv);
    (void)vertical;
    int maxCol = 0;
    for (int i = 0; i < (int)params.size(); ++i) {
      int col, row;
      if (sec == FanfareSynthProcessor::SecFltEnv) {
        col = i;
        row = 0;
      } else if (sec == FanfareSynthProcessor::SecAmpEnv) {
        if (i < 4) {
          col = i;
          row = 0;
        } else {
          col = 3;
          row = 1;
        }
      } else {
        col = i / rows;
        row = i % rows;
      }
      panel->addControl(new SynthParamControl(synth, params[i], vertical),
                        kPanelPad + col * kCellW, kHeaderH + row * kCellH + 2);
      maxCol = juce::jmax(maxCol, col);
    }
    // Uniform panel heights keep the two section bands aligned
    panel->setSize(kPanelPad * 2 + (maxCol + 1) * kCellW,
                   kHeaderH + rows * kCellH + 4);
    addAndMakeVisible(panel);
    ownedPanels.add(panel); // component owns the panel
    panels[(size_t)sec] = panel;
  }

  bool currentIsUserPreset() const {
    int sel = presetBox.getSelectedItemIndex();
    return sel >= 0 && sel < presetList.size() &&
           presetList.getReference(sel).isUser;
  }

  RackSlot &slotRef;
  FanfareSynthProcessor &synth;
  std::function<void()> closeCallback;

  juce::Label titleLabel;
  juce::TextButton enableBtn;
  juce::ComboBox presetBox;
  juce::TextButton prevBtn, nextBtn, saveBtn, delBtn;
  juce::TextButton closeBtn{"X"};
  juce::Label hintLabel;
  OnscreenKeyboard keyboard;

  juce::OwnedArray<SynthSectionPanel> ownedPanels;
  std::array<SynthSectionPanel *, FanfareSynthProcessor::kNumSections> panels{};

  juce::Array<PresetRef> presetList;
  juce::File userPresetDir;

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(FanfareSynthComponent)
};
