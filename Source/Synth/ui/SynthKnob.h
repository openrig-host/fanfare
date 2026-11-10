#pragma once

#include <JuceHeader.h>
#include <cmath>
#include "../../BoutiqueLookAndFeel.h"
#include "../../ThemeManager.h"
#include "../FanfareSynthProcessorImpl.h"
#include "../SynthConstants.h"
#include "../SynthParams.h"
#include "SynthControlCallbacks.h"
#include "SynthValueReadout.h"

namespace FanfareSynth::Ui {

class SynthKnob : public SynthParamWidget {
public:
  SynthKnob(FanfareSynthProcessor &processor, int parameterIndex,
            SynthTelemetrySink &telemetry, bool horizontalCompact = false,
            const juce::String &overrideShortLabel = {})
      : SynthParamWidget(parameterIndex), proc(processor), sink(telemetry),
        compactHorizontal(horizontalCompact),
        readout(processor, parameterIndex, telemetry) {
    const auto &d = paramDefs()[(size_t)paramIdx];
    shortTitle = overrideShortLabel.isNotEmpty() ? overrideShortLabel
                                                 : juce::String(d.shortLabel);
    readout.onWheelStep = [this](float deltaY, bool fine) {
      applyWheelDelta(deltaY, fine);
    };
    addAndMakeVisible(readout);
    syncFromProcessor();
  }

  void syncFromProcessor() override {
    const auto &d = paramDefs()[(size_t)paramIdx];
    normValue = valueToNormalized(d, proc.getParam(paramIdx));
    readout.refreshText();
    repaint();
  }

  void setControlDimmed(bool dimmed) override {
    if (isDimmed == dimmed)
      return;
    isDimmed = dimmed;
    setAlpha(dimmed ? UiMetrics::kDisabledControlAlpha : 1.0f);
    repaint();
  }

  void beginInlineValueEdit() override { readout.openEditor(); }

  void resized() override {
    auto b = getLocalBounds();
    if (compactHorizontal) {
      const int dialSize = juce::jmin(b.getHeight() - 2, 26);
      dialArea = b.removeFromLeft(dialSize + 2).withSizeKeepingCentre(dialSize, dialSize);
      b.removeFromLeft(2);
      labelArea = b.removeFromTop(11);
      readout.setBounds(b.withHeight(juce::jmin(15, b.getHeight())));
    } else {
      labelArea = b.removeFromTop(UiMetrics::kControlLabelHeight);
      readout.setBounds(
          b.removeFromBottom(UiMetrics::kValueReadoutHeight).reduced(2, 0));
      const int dialSize = juce::jlimit(
          18, 48, juce::jmin(b.getWidth() - 4, b.getHeight() - 4));
      dialArea = b.withSizeKeepingCentre(dialSize, dialSize);
    }
  }

  void paint(juce::Graphics &g) override {
    g.setColour(ThemeManager::get(hovered || dragging ? Theme::Role::text
                                                      : Theme::Role::engrave));
    g.setFont(BoutiqueLookAndFeel::condensedFont(
        compactHorizontal ? 10.5f : 11.5f, 0.05f));
    g.drawFittedText(shortTitle, labelArea,
                     compactHorizontal ? juce::Justification::centredLeft
                                       : juce::Justification::centred,
                     1, 0.80f);

    if (dialArea.isEmpty())
      return;

    const auto df = dialArea.toFloat();
    const float cx = df.getCentreX();
    const float cy = df.getCentreY();
    const float outerR = df.getWidth() * 0.5f - 1.5f;
    const float bodyR = juce::jmax(5.0f, outerR - 3.2f);

    constexpr float kStartAngle = -2.35619449f; // -135 deg
    constexpr float kEndAngle = 2.35619449f;    // +135 deg
    const float valueAngle =
        kStartAngle + normValue * (kEndAngle - kStartAngle);

    juce::Path bgArc;
    bgArc.addCentredArc(cx, cy, outerR, outerR, 0.0f, kStartAngle, kEndAngle,
                        true);
    g.setColour(ThemeManager::get(Theme::Role::trackGroove));
    g.strokePath(bgArc, juce::PathStrokeType(2.2f, juce::PathStrokeType::curved,
                                             juce::PathStrokeType::rounded));

    const auto &d = paramDefs()[(size_t)paramIdx];
    const float originAngle = d.bipolar ? 0.0f : kStartAngle;
    if (std::abs(valueAngle - originAngle) > 0.02f) {
      juce::Path valArc;
      valArc.addCentredArc(cx, cy, outerR, outerR, 0.0f,
                           juce::jmin(originAngle, valueAngle),
                           juce::jmax(originAngle, valueAngle), true);
      g.setColour(ThemeManager::get(dragging ? Theme::Role::accent
                                             : Theme::Role::accentDim));
      g.strokePath(valArc,
                   juce::PathStrokeType(2.3f, juce::PathStrokeType::curved,
                                        juce::PathStrokeType::rounded));
    }

    const juce::Rectangle<float> body(cx - bodyR, cy - bodyR, bodyR * 2.0f,
                                      bodyR * 2.0f);
    g.setColour(ThemeManager::get(Theme::Role::knobFace));
    g.fillEllipse(body);
    g.setColour(ThemeManager::get(dragging || hovered ? Theme::Role::accent
                                                      : Theme::Role::borderStrong));
    g.drawEllipse(body, 1.0f);

    const float sinA = std::sin(valueAngle);
    const float cosA = -std::cos(valueAngle);
    g.setColour(ThemeManager::get(fineDragActive ? Theme::Role::accent
                                                 : Theme::Role::knobThumb));
    g.drawLine(cx + sinA * (bodyR * 0.28f), cy + cosA * (bodyR * 0.28f),
               cx + sinA * (bodyR - 1.2f), cy + cosA * (bodyR - 1.2f), 1.8f);
  }

  void mouseEnter(const juce::MouseEvent &) override {
    hovered = true;
    sink.handleParamHovered(paramIdx);
    repaint();
  }

  void mouseExit(const juce::MouseEvent &) override {
    hovered = false;
    repaint();
  }

  void mouseDown(const juce::MouseEvent &e) override {
    rightButtonGesture = e.mods.isPopupMenu();
    rightButtonDragged = false;
    dragging = true;
    fineDragActive = isFineModifier(e.mods);
    anchorMouseY = e.position.y;
    anchorMouseX = e.position.x;
    anchorNorm = normValue;
    sink.handleParamTouchState(paramIdx, true, fineDragActive);
    repaint();
  }

  void mouseDrag(const juce::MouseEvent &e) override {
    if (!dragging)
      return;

    if (rightButtonGesture && e.getDistanceFromDragStart() >= 3)
      rightButtonDragged = true;

    const bool nowFine = isFineModifier(e.mods);
    if (nowFine != fineDragActive) {
      anchorMouseY = e.position.y;
      anchorMouseX = e.position.x;
      anchorNorm = normValue;
      fineDragActive = nowFine;
      sink.handleParamTouchState(paramIdx, true, fineDragActive);
    }

    const float span = fineDragActive
                           ? (UiMetrics::kKnobVirtualSpanPx *
                              UiMetrics::kFineTuneGearDivisor)
                           : UiMetrics::kKnobVirtualSpanPx;
    const float deltaPixels =
        (anchorMouseY - e.position.y) + 0.35f * (e.position.x - anchorMouseX);
    float candidate = juce::jlimit(0.0f, 1.0f, anchorNorm + deltaPixels / span);

    const auto &d = paramDefs()[(size_t)paramIdx];
    if (d.bipolar && !fineDragActive &&
        std::abs(candidate - 0.5f) < UiMetrics::kBipolarDetentRadius) {
      candidate = 0.5f;
    }

    applyNormalizedFromUi(candidate);
  }

  void mouseUp(const juce::MouseEvent &) override {
    const bool showPopup = rightButtonGesture && !rightButtonDragged;
    dragging = false;
    fineDragActive = false;
    rightButtonGesture = false;
    sink.handleParamTouchState(paramIdx, false, false);
    repaint();

    if (showPopup)
      sink.showParamContextMenu(paramIdx, this);
  }

  void mouseDoubleClick(const juce::MouseEvent &e) override {
    if (e.mods.isPopupMenu())
      return;
    const auto &d = paramDefs()[(size_t)paramIdx];
    proc.setParam(paramIdx, d.def, true);
    syncFromProcessor();
    sink.handleParamEdited(paramIdx);
  }

  void mouseWheelMove(const juce::MouseEvent &e,
                      const juce::MouseWheelDetails &wheel) override {
    if (std::abs(wheel.deltaY) > 1e-5f) {
      const bool fine = e.mods.isShiftDown() || e.mods.isCommandDown();
      applyWheelDelta(wheel.deltaY, fine);
    }
  }

private:
  static bool isFineModifier(const juce::ModifierKeys &mods) noexcept {
    return mods.isShiftDown() || mods.isCommandDown() || mods.isCtrlDown() ||
           mods.isRightButtonDown();
  }

  void applyNormalizedFromUi(float newNorm) {
    const auto &d = paramDefs()[(size_t)paramIdx];
    const float val = normalizedToValue(d, newNorm);
    proc.setParam(paramIdx, val, true);
    normValue = valueToNormalized(d, proc.getParam(paramIdx));
    readout.refreshText();
    sink.handleParamEdited(paramIdx);
    repaint();
  }

  void applyWheelDelta(float wheelDeltaY, bool fine) {
    const auto &d = paramDefs()[(size_t)paramIdx];
    const float dir = (wheelDeltaY > 0.0f) ? 1.0f : -1.0f;
    if (d.step >= 1.0f) {
      const float nextVal =
          clampAndSnapParam(d, proc.getParam(paramIdx) + dir * d.step);
      proc.setParam(paramIdx, nextVal, true);
      syncFromProcessor();
      sink.handleParamEdited(paramIdx);
      return;
    }
    const float step =
        d.wheelNormStep * (fine ? UiMetrics::kWheelFineMultiplier : 1.0f);
    applyNormalizedFromUi(juce::jlimit(0.0f, 1.0f, normValue + dir * step));
  }

  FanfareSynthProcessor &proc;
  SynthTelemetrySink &sink;
  bool compactHorizontal = false;
  juce::String shortTitle;
  SynthValueReadout readout;

  juce::Rectangle<int> labelArea;
  juce::Rectangle<int> dialArea;
  float normValue = 0.0f;
  float anchorMouseY = 0.0f;
  float anchorMouseX = 0.0f;
  float anchorNorm = 0.0f;

  bool hovered = false;
  bool dragging = false;
  bool fineDragActive = false;
  bool rightButtonGesture = false;
  bool rightButtonDragged = false;
  bool isDimmed = false;
};

} // namespace FanfareSynth::Ui

