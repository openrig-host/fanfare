#pragma once

#include <JuceHeader.h>
#include <functional>
#include "../../BoutiqueLookAndFeel.h"
#include "../../ThemeManager.h"
#include "../FanfareSynthProcessorImpl.h"
#include "../SynthConstants.h"
#include "../SynthParams.h"
#include "SynthControlCallbacks.h"

namespace FanfareSynth::Ui {

class SynthValueReadout : public juce::Component,
                          public juce::SettableTooltipClient {
public:
  SynthValueReadout(FanfareSynthProcessor &processor, int parameterIndex,
                    SynthTelemetrySink &telemetry)
      : proc(processor), idx(parameterIndex), sink(telemetry) {
    setMouseCursor(juce::MouseCursor::IBeamCursor);
    refreshText();
  }

  std::function<void(float wheelDelta, bool fine)> onWheelStep;

  void refreshText() {
    const auto &d = paramDefs()[(size_t)idx];
    const float val = proc.getParam(idx);
    displayText = formatParamValue(d, val, false);
    mappedCC = proc.getCCForParamIndex(idx);

    juce::String tip =
        juce::String(d.label) + ": " + formatParamValue(d, val, true);
    if (mappedCC >= 0)
      tip += "  [CC " + juce::String(mappedCC) + "]";
    tip += "  (Click to type value, Shift+Drag for 10x fine, Right-click for MIDI CC)";
    setTooltip(tip);
    repaint();
  }

  void openEditor() {
    if (editor != nullptr)
      return;
    const auto &d = paramDefs()[(size_t)idx];
    if (d.kind == KindEnum)
      return;

    editor = std::make_unique<juce::TextEditor>();
    editor->setBounds(getLocalBounds());
    editor->setFont(BoutiqueLookAndFeel::monoFont(10.5f, true));
    editor->setJustification(juce::Justification::centred);
    editor->setColour(juce::TextEditor::backgroundColourId,
                      ThemeManager::get(Theme::Role::sunken));
    editor->setColour(juce::TextEditor::textColourId,
                      ThemeManager::get(Theme::Role::text));
    editor->setColour(juce::TextEditor::highlightColourId,
                      ThemeManager::get(Theme::Role::accent).withAlpha(0.35f));
    editor->setColour(juce::TextEditor::outlineColourId,
                      ThemeManager::get(Theme::Role::accent));
    editor->setColour(juce::TextEditor::focusedOutlineColourId,
                      ThemeManager::get(Theme::Role::accent));
    editor->setIndents(2, 1);
    editor->setText(formatEditableText(d, proc.getParam(idx)), false);
    addAndMakeVisible(*editor);
    editor->grabKeyboardFocus();
    editor->selectAll();

    editor->onReturnKey = [this] { commitAndClose(true); };
    editor->onEscapeKey = [this] { commitAndClose(false); };
    editor->onFocusLost = [this] { commitAndClose(true); };
  }

  void paint(juce::Graphics &g) override {
    if (editor != nullptr)
      return;

    const auto b = getLocalBounds().toFloat().reduced(0.5f, 0.5f);
    g.setColour(ThemeManager::get(Theme::Role::sunken));
    g.fillRoundedRectangle(b, 2.5f);

    g.setColour(hovered ? ThemeManager::get(Theme::Role::borderStrong)
                        : ThemeManager::get(Theme::Role::border));
    g.drawRoundedRectangle(b, 2.5f, 1.0f);

    if (mappedCC >= 0) {
      auto ccStripe = b.withWidth(2.2f).reduced(0.0f, 2.0f);
      ccStripe.setX(b.getX() + 1.2f);
      g.setColour(ThemeManager::get(Theme::Role::accent));
      g.fillRoundedRectangle(ccStripe, 1.0f);
    }

    g.setColour(ThemeManager::get(hovered ? Theme::Role::text
                                          : Theme::Role::textDim));
    g.setFont(BoutiqueLookAndFeel::monoFont(10.0f, false));
    g.drawFittedText(displayText, getLocalBounds().reduced(3, 0),
                     juce::Justification::centred, 1, 0.78f);
  }

  void resized() override {
    if (editor != nullptr)
      editor->setBounds(getLocalBounds());
  }

  void mouseEnter(const juce::MouseEvent &) override {
    hovered = true;
    sink.handleParamHovered(idx);
    repaint();
  }

  void mouseExit(const juce::MouseEvent &) override {
    hovered = false;
    repaint();
  }

  void mouseDown(const juce::MouseEvent &e) override {
    if (e.mods.isPopupMenu()) {
      sink.showParamContextMenu(idx, this);
      return;
    }
    openEditor();
  }

  void mouseWheelMove(const juce::MouseEvent &e,
                      const juce::MouseWheelDetails &wheel) override {
    if (onWheelStep && std::abs(wheel.deltaY) > 1e-5f) {
      const bool fine = e.mods.isShiftDown() || e.mods.isCommandDown();
      onWheelStep(wheel.deltaY, fine);
    }
  }

private:
  void commitAndClose(bool applyValue) {
    if (closingEditor || editor == nullptr)
      return;
    closingEditor = true;

    const juce::String typed = editor->getText();
    juce::Component::SafePointer<SynthValueReadout> safeThis(this);
    juce::MessageManager::callAsync([safeThis, applyValue, typed] {
      if (safeThis == nullptr)
        return;
      if (applyValue) {
        const auto &d = paramDefs()[(size_t)safeThis->idx];
        float parsed = 0.0f;
        if (parseParamText(d, typed, parsed)) {
          safeThis->proc.setParam(safeThis->idx, parsed, true);
          safeThis->sink.handleParamEdited(safeThis->idx);
        }
      }
      safeThis->editor.reset();
      safeThis->closingEditor = false;
      safeThis->refreshText();
    });
  }

  FanfareSynthProcessor &proc;
  int idx = 0;
  SynthTelemetrySink &sink;
  juce::String displayText;
  int mappedCC = -1;
  bool hovered = false;
  bool closingEditor = false;
  std::unique_ptr<juce::TextEditor> editor;
};

} // namespace FanfareSynth::Ui
