#pragma once

#include <JuceHeader.h>
#include <map>
#include <utility>
#include <vector>
#include "../../BoutiqueLookAndFeel.h"
#include "../../ThemeManager.h"
#include "../FanfareSynthProcessorImpl.h"
#include "../SynthConstants.h"

namespace FanfareSynth::Ui {

class OnscreenKeyboard : public juce::Component, private juce::Timer {
public:
  explicit OnscreenKeyboard(FanfareSynthProcessor &p) : proc(p) {
    setWantsKeyboardFocus(true);

    int offset = 0;
    for (const char *row : {kRowLower, kRowUpper}) {
      for (int i = 0; row[i] != 0; ++i)
        keyMap.push_back(
            {(int)std::toupper((unsigned char)row[i]), offset + i});
      offset += 12;
    }

    octDownBtn.setButtonText("OCT -");
    octDownBtn.setTooltip("Shift keyboard down an octave (Left Arrow)");
    octDownBtn.onClick = [this] { shiftOctave(-12); };
    addAndMakeVisible(octDownBtn);

    octUpBtn.setButtonText("OCT +");
    octUpBtn.setTooltip("Shift keyboard up an octave (Right Arrow)");
    octUpBtn.onClick = [this] { shiftOctave(12); };
    addAndMakeVisible(octUpBtn);

    panicBtn.setButtonText("ALL OFF");
    panicBtn.setColour(juce::TextButton::buttonColourId,
                       ThemeManager::get(Theme::Role::danger).darker(0.35f));
    panicBtn.setTooltip("Silence all active synth voices");
    panicBtn.onClick = [this] {
      releaseAll();
      proc.setUiModWheel(0.0f);
      proc.setUiPitchBend(0.0f);
      proc.allNotesOff();
      repaint();
    };
    addAndMakeVisible(panicBtn);
  }

  void setKeyboardEnabled(bool enabled) {
    keyboardActive = enabled;
    if (keyboardActive) {
      startTimerHz(18);
    } else {
      stopTimer();
      releaseAll();
    }
  }

  void shiftOctave(int semis) {
    releaseAll();
    baseNote = juce::jlimit(12, 96, baseNote + semis);
    repaint();
  }

  void releaseAllNotes() { releaseAll(); }

  bool handleKeyPressed(const juce::KeyPress &key) {
    if (!keyboardActive || isTextEditorFocused())
      return false;

    if (key.isKeyCode(juce::KeyPress::leftKey)) {
      shiftOctave(-12);
      return true;
    }
    if (key.isKeyCode(juce::KeyPress::rightKey)) {
      shiftOctave(12);
      return true;
    }

    const int code = normalizeCode(key.getKeyCode());
    for (const auto &km : keyMap) {
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

  bool handleKeyStateChange() {
    if (!keyboardActive || isTextEditorFocused())
      return false;

    for (const auto &km : keyMap) {
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

  void paint(juce::Graphics &g) override {
    const auto b = getLocalBounds().toFloat();
    g.setColour(ThemeManager::get(Theme::Role::panel));
    g.fillRoundedRectangle(b, 3.0f);
    g.setColour(ThemeManager::get(Theme::Role::border));
    g.drawRoundedRectangle(b.reduced(0.5f), 3.0f, 1.0f);

    drawWheelStrip(g, bendStripRect, "BEND",
                   0.5f + 0.5f * proc.getPitchBend(), true);
    drawWheelStrip(g, modStripRect, "MOD", proc.getModWheel(), false);

    auto headerRect = keysHeaderRect;
    g.setColour(ThemeManager::get(Theme::Role::textDim));
    g.setFont(BoutiqueLookAndFeel::monoFont(10.5f, false));
    g.drawText(noteName(baseNote) + " - " + noteName(baseNote + 24) +
                   "  |  QWERTY: Z-M lower / Q-U upper  |  Arrows: Octave",
               headerRect, juce::Justification::centredLeft, true);

    const auto keys = pianoRect.toFloat();
    if (keys.isEmpty())
      return;

    constexpr int kNumWhiteKeys = 15;
    const float wW = keys.getWidth() / (float)kNumWhiteKeys;

    for (int s = 0; s <= 24; ++s) {
      if (isBlackPc(s % 12))
        continue;
      const auto r = juce::Rectangle<float>(
          keys.getX() + (float)whiteIndexOf(s) * wW, keys.getY(), wW - 1.0f,
          keys.getHeight());
      const bool on = isNoteOn(baseNote + s);
      g.setColour(on ? ThemeManager::get(Theme::Role::accent).withAlpha(0.80f)
                     : ThemeManager::get(Theme::Role::raised));
      g.fillRoundedRectangle(r, 2.5f);
      g.setColour(ThemeManager::get(Theme::Role::border));
      g.drawRoundedRectangle(r, 2.5f, 1.0f);

      const char ch = charForOffset(s);
      if (ch != 0) {
        g.setColour(on ? ThemeManager::get(Theme::Role::textOnAccent)
                       : ThemeManager::get(Theme::Role::textFaint));
        g.setFont(BoutiqueLookAndFeel::monoFont(9.5f, false));
        g.drawText(juce::String::charToString(
                       (juce::juce_wchar)std::toupper((unsigned char)ch)),
                   r.toNearestInt().removeFromBottom(14),
                   juce::Justification::centred, false);
      }
    }

    for (int s = 0; s <= 24; ++s) {
      if (!isBlackPc(s % 12))
        continue;
      const float bx =
          keys.getX() + (float)whiteIndexOf(s) * wW - wW * 0.30f;
      const auto r = juce::Rectangle<float>(bx, keys.getY(), wW * 0.60f,
                                            keys.getHeight() * 0.58f);
      const bool on = isNoteOn(baseNote + s);
      g.setColour(on ? ThemeManager::get(Theme::Role::accent)
                     : ThemeManager::get(Theme::Role::sunken).darker(0.6f));
      g.fillRoundedRectangle(r, 2.0f);
      g.setColour(ThemeManager::get(Theme::Role::borderStrong));
      g.drawRoundedRectangle(r, 2.0f, 0.8f);
    }
  }

  void resized() override {
    auto b = getLocalBounds().reduced(5, 4);
    auto wheelsArea =
        b.removeFromLeft(UiMetrics::kWheelStripWidth * 2 + 8);
    bendStripRect = wheelsArea.removeFromLeft(UiMetrics::kWheelStripWidth);
    wheelsArea.removeFromLeft(4);
    modStripRect = wheelsArea.removeFromLeft(UiMetrics::kWheelStripWidth);

    b.removeFromLeft(6);
    auto topBar = b.removeFromTop(19);
    panicBtn.setBounds(topBar.removeFromRight(60).reduced(1, 0));
    topBar.removeFromRight(3);
    octUpBtn.setBounds(topBar.removeFromRight(46).reduced(1, 0));
    topBar.removeFromRight(2);
    octDownBtn.setBounds(topBar.removeFromRight(46).reduced(1, 0));
    topBar.removeFromRight(6);
    keysHeaderRect = topBar;

    b.removeFromTop(3);
    pianoRect = b;
  }

  void focusLost(juce::Component::FocusChangeType) override { releaseAll(); }
  bool keyPressed(const juce::KeyPress &key) override {
    return handleKeyPressed(key);
  }
  bool keyStateChanged(bool) override { return handleKeyStateChange(); }
  void timerCallback() override {
    handleKeyStateChange();
    const float curBend = proc.getPitchBend();
    const float curMod = proc.getModWheel();
    if (std::abs(curBend - lastPaintedBend) > 0.005f ||
        std::abs(curMod - lastPaintedMod) > 0.005f) {
      lastPaintedBend = curBend;
      lastPaintedMod = curMod;
      repaint(bendStripRect.getUnion(modStripRect));
    }
  }

  void mouseDown(const juce::MouseEvent &e) override {
    grabKeyboardFocus();
    const auto pt = e.getPosition();
    if (bendStripRect.contains(pt)) {
      dragTarget = DragBend;
      updateWheelFromPos(e.position);
      return;
    }
    if (modStripRect.contains(pt)) {
      dragTarget = DragMod;
      updateWheelFromPos(e.position);
      return;
    }

    dragTarget = DragKeys;
    mouseNote = noteFromPos(e.position);
    if (mouseNote >= 0)
      proc.injectNote(mouseNote, true);
    repaint();
  }

  void mouseDrag(const juce::MouseEvent &e) override {
    if (dragTarget == DragBend || dragTarget == DragMod) {
      updateWheelFromPos(e.position);
      return;
    }
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
    if (dragTarget == DragBend) {
      proc.setUiPitchBend(0.0f);
    } else if (dragTarget == DragKeys && mouseNote >= 0) {
      proc.injectNote(mouseNote, false);
      mouseNote = -1;
    }
    dragTarget = DragNone;
    repaint();
  }

  void mouseDoubleClick(const juce::MouseEvent &e) override {
    if (modStripRect.contains(e.getPosition())) {
      proc.setUiModWheel(0.0f);
      repaint();
    }
  }

private:
  enum DragTarget { DragNone = 0, DragKeys, DragBend, DragMod };

  static bool isTextEditorFocused() {
    return dynamic_cast<juce::TextEditor *>(
               juce::Component::getCurrentlyFocusedComponent()) != nullptr;
  }

  void drawWheelStrip(juce::Graphics &g, juce::Rectangle<int> area,
                      const char *label, float normPos, bool bipolar) const {
    if (area.isEmpty())
      return;
    auto labelBox = area.removeFromTop(13);
    g.setColour(ThemeManager::get(Theme::Role::engrave));
    g.setFont(BoutiqueLookAndFeel::condensedFont(10.0f, 0.05f));
    g.drawText(label, labelBox, juce::Justification::centred, false);

    const auto slot = area.reduced(6, 2).toFloat();
    g.setColour(ThemeManager::get(Theme::Role::sunken));
    g.fillRoundedRectangle(slot, 3.0f);
    g.setColour(ThemeManager::get(Theme::Role::border));
    g.drawRoundedRectangle(slot, 3.0f, 1.0f);

    const float clamped = juce::jlimit(0.0f, 1.0f, normPos);
    const float thumbY =
        slot.getBottom() - 4.0f - clamped * (slot.getHeight() - 8.0f);

    g.setColour(ThemeManager::get(Theme::Role::accentDim));
    if (bipolar) {
      const float midY = slot.getCentreY();
      const float top = juce::jmin(midY, thumbY);
      g.fillRect(juce::Rectangle<float>(slot.getX() + 2.0f, top,
                                        slot.getWidth() - 4.0f,
                                        std::abs(thumbY - midY)));
    } else {
      g.fillRect(juce::Rectangle<float>(slot.getX() + 2.0f, thumbY,
                                        slot.getWidth() - 4.0f,
                                        slot.getBottom() - 2.0f - thumbY));
    }

    g.setColour(ThemeManager::get(Theme::Role::knobThumb));
    g.fillRoundedRectangle(
        juce::Rectangle<float>(slot.getX() + 1.0f, thumbY - 2.5f,
                               slot.getWidth() - 2.0f, 5.0f),
        1.5f);
  }

  void updateWheelFromPos(juce::Point<float> p) {
    const auto rect = (dragTarget == DragBend) ? bendStripRect : modStripRect;
    const auto slot = rect.withTrimmedTop(13).reduced(6, 4).toFloat();
    if (slot.getHeight() <= 4.0f)
      return;
    const float norm = juce::jlimit(
        0.0f, 1.0f, (slot.getBottom() - p.y) / slot.getHeight());
    if (dragTarget == DragBend)
      proc.setUiPitchBend(norm * 2.0f - 1.0f);
    else if (dragTarget == DragMod)
      proc.setUiModWheel(norm);
    repaint(bendStripRect.getUnion(modStripRect));
  }

  static bool isBlackPc(int pc) noexcept {
    return pc == 1 || pc == 3 || pc == 6 || pc == 8 || pc == 10;
  }

  static int normalizeCode(int code) noexcept {
    return (code >= 'a' && code <= 'z') ? code - 32 : code;
  }

  static char charForOffset(int s) noexcept {
    if (s >= 0 && s < 12)
      return kRowLower[s];
    if (s >= 12 && s < 24)
      return kRowUpper[s - 12];
    return 0;
  }

  static int whiteIndexOf(int s) noexcept {
    int w = 0;
    for (int i = 0; i < s; ++i)
      if (!isBlackPc(i % 12))
        ++w;
    return w;
  }

  static juce::String noteName(int midi) {
    static constexpr const char *kNames[] = {
        "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    return juce::String(kNames[midi % 12]) + juce::String(midi / 12 - 1);
  }

  bool isNoteOn(int n) const {
    if (n == mouseNote)
      return true;
    for (const auto &kv : keyHeldByCode)
      if (kv.second == n)
        return true;
    return false;
  }

  int noteFromPos(juce::Point<float> p) const {
    const auto keys = pianoRect.toFloat();
    if (!keys.contains(p))
      return -1;
    constexpr int kNumWhites = 15;
    const float wW = keys.getWidth() / (float)kNumWhites;
    if (p.y < keys.getY() + keys.getHeight() * 0.58f) {
      for (int s = 0; s <= 24; ++s) {
        if (!isBlackPc(s % 12))
          continue;
        const float bx =
            keys.getX() + (float)whiteIndexOf(s) * wW - wW * 0.30f;
        if (p.x >= bx && p.x < bx + wW * 0.60f)
          return baseNote + s;
      }
    }
    const int wi =
        juce::jlimit(0, kNumWhites - 1, (int)((p.x - keys.getX()) / wW));
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
    for (const auto &kv : keyHeldByCode)
      proc.injectNote(kv.second, false);
    keyHeldByCode.clear();
    if (mouseNote >= 0) {
      proc.injectNote(mouseNote, false);
      mouseNote = -1;
    }
    dragTarget = DragNone;
    repaint();
  }

  static inline constexpr const char *kRowLower = "zsxdcvgbhnjm";
  static inline constexpr const char *kRowUpper = "q2w3er5t6y7ui";

  FanfareSynthProcessor &proc;
  int baseNote = 48;
  int mouseNote = -1;
  DragTarget dragTarget = DragNone;
  float lastPaintedBend = 0.0f;
  float lastPaintedMod = 0.0f;
  std::vector<std::pair<int, int>> keyMap;
  std::map<int, int> keyHeldByCode;

  juce::TextButton octDownBtn, octUpBtn, panicBtn;
  juce::Rectangle<int> bendStripRect, modStripRect, keysHeaderRect, pianoRect;
  bool keyboardActive = false;

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(OnscreenKeyboard)
};

} // namespace FanfareSynth::Ui

