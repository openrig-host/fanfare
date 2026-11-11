#pragma once

#include <JuceHeader.h>
#include <cmath>
#include "../../BoutiqueLookAndFeel.h"
#include "../../ThemeManager.h"
#include "../FanfareSynthProcessorImpl.h"
#include "../SynthConstants.h"
#include "../SynthParams.h"
#include "SynthControlCallbacks.h"
#include "SynthSkin.h"
#include "SynthValueReadout.h"

namespace FanfareSynth::Ui {

class SynthFader : public SynthParamWidget {
public:
  SynthFader(FanfareSynthProcessor &processor, int parameterIndex,
             SynthTelemetrySink &telemetry,
             const juce::String &overrideShortLabel = {})
      : SynthParamWidget(parameterIndex), proc(processor), sink(telemetry),
        readout(processor, parameterIndex, telemetry) {
    const auto &d = paramDefs()[(size_t)paramIdx];
    shortTitle =
        overrideShortLabel.isNotEmpty() ? overrideShortLabel : juce::String(d.shortLabel);

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
    labelArea = b.removeFromTop(UiMetrics::kControlLabelHeight);
    readout.setBounds(
        b.removeFromBottom(UiMetrics::kValueReadoutHeight).reduced(2, 0));
    trackBounds = b.reduced(2, 5);
  }

  void paint(juce::Graphics &g) override {
    using namespace Skin;
    g.setColour(ThemeManager::get(hovered || dragging ? Theme::Role::text
                                                      : Theme::Role::engrave));
    g.setFont(BoutiqueLookAndFeel::condensedFont(11.5f, 0.05f));
    g.drawFittedText(shortTitle, labelArea, juce::Justification::centred, 1,
                     0.80f);

    if (trackBounds.getHeight() < 20)
      return;

    const float capH = 13.0f;
    const float capW = juce::jlimit(16.0f, 26.0f, (float)trackBounds.getWidth() * 0.62f);
    const float topY = (float)trackBounds.getY() + capH * 0.5f;
    const float botY = (float)trackBounds.getBottom() - capH * 0.5f;
    const float travel = juce::jmax(8.0f, botY - topY);
    const float centerX = (float)trackBounds.getCentreX();

    const juce::Colour majorTickCol =
        ThemeManager::get(Theme::Role::textFaint).withAlpha(0.85f);
    const juce::Colour minorTickCol =
        ThemeManager::get(Theme::Role::borderStrong).withAlpha(0.60f);
    const float tickInner = 5.5f;
    const bool roomForNumerals = trackBounds.getWidth() >= 40;
    for (int step = 0; step <= 10; ++step) {
      const float t = (float)step / 10.0f;
      const float y = botY - t * travel;
      const bool major = (step == 0 || step == 5 || step == 10);
      const float len = major ? 6.0f : 3.0f;
      g.setColour(major ? majorTickCol : minorTickCol);
      g.drawHorizontalLine((int)std::lround(y), centerX - tickInner - len,
                           centerX - tickInner);
      g.drawHorizontalLine((int)std::lround(y), centerX + tickInner,
                           centerX + tickInner + len);
      if (major && roomForNumerals) {
        g.setColour(ThemeManager::get(Theme::Role::textFaint).withAlpha(0.70f));
        g.setFont(BoutiqueLookAndFeel::monoFont(7.5f, false));
        g.drawText(juce::String(step),
                   juce::Rectangle<float>(centerX - tickInner - len - 13.0f,
                                          y - 4.0f, 11.0f, 8.0f)
                       .toNearestInt(),
                   juce::Justification::centredRight, false);
      }
    }

    const juce::Rectangle<float> recess(centerX - 4.0f, topY - 2.0f, 8.0f,
                                        travel + 4.0f);
    drawRecess(g, recess, 3.0f);

    const juce::Rectangle<float> groove(centerX - 2.0f, topY, 4.0f, travel);
    g.setColour(ThemeManager::get(Theme::Role::trackGroove));
    g.fillRoundedRectangle(groove, 2.0f);

    const auto &d = paramDefs()[(size_t)paramIdx];
    const float thumbY = botY - normValue * travel;
    const juce::Colour fillCol =
        ThemeManager::get(dragging ? Theme::Role::accent
                                   : Theme::Role::accentDim);
    if (d.bipolar) {
      const float midY = botY - 0.5f * travel;
      const float fillTop = juce::jmin(midY, thumbY);
      const float fillH = std::abs(thumbY - midY);
      if (fillH > 0.5f)
        glowFill(g, juce::Rectangle<float>(centerX - 1.5f, fillTop, 3.0f,
                                           fillH),
                 1.5f, fillCol);
      g.setColour(ThemeManager::get(Theme::Role::knobThumb).withAlpha(0.55f));
      g.fillRect(juce::Rectangle<float>(centerX - tickInner - 2.0f, midY - 0.5f,
                                        3.0f, 1.0f));
      g.fillRect(juce::Rectangle<float>(centerX + tickInner - 1.0f, midY - 0.5f,
                                        3.0f, 1.0f));
    } else {
      const float fillH = botY - thumbY;
      if (fillH > 0.5f)
        glowFill(g, juce::Rectangle<float>(centerX - 1.5f, thumbY, 3.0f,
                                           fillH),
                 1.5f, fillCol);
    }

    const juce::Rectangle<float> cap(centerX - capW * 0.5f,
                                     thumbY - capH * 0.5f, capW, capH);
    g.setColour(juce::Colours::black.withAlpha(0.28f));
    g.fillRoundedRectangle(cap.translated(0.0f, 2.2f).expanded(0.8f, 0.0f), 2.5f);
    g.setColour(juce::Colours::black.withAlpha(0.42f));
    g.fillRoundedRectangle(cap.translated(0.0f, 1.2f), 2.5f);

    const auto faceTop = ThemeManager::get(Theme::Role::faceTop);
    const auto faceBot = ThemeManager::get(Theme::Role::faceBot);
    fillVertical(g, cap, Skin::lift(faceTop, 0.22f),
                 Skin::sink(faceBot, 0.18f), 2.5f);
    g.setColour(Skin::lift(faceTop, 0.70f)
                    .withAlpha(Skin::isDark() ? 0.22f : 0.75f));
    g.fillRect(cap.withHeight(1.0f).withY(cap.getY() + 1.0f)
                 .reduced(2.0f, 0.0f));
    g.setColour(ThemeManager::get(dragging || hovered ? Theme::Role::accent
                                                      : Theme::Role::borderStrong));
    g.drawRoundedRectangle(cap.reduced(0.5f), 2.5f, 1.0f);

    g.setColour(juce::Colours::black.withAlpha(0.35f));
    g.fillRect(juce::Rectangle<float>(cap.getX() + 2.0f, thumbY - 3.2f,
                                      cap.getWidth() - 4.0f, 1.0f));
    g.fillRect(juce::Rectangle<float>(cap.getX() + 2.0f, thumbY + 2.2f,
                                      cap.getWidth() - 4.0f, 1.0f));

    if (fineDragActive || dragging) {
      glowLine(g, cap.getX() + 2.0f, thumbY, cap.getRight() - 2.0f, thumbY,
               ThemeManager::get(Theme::Role::accent), 1.6f);
    } else {
      g.setColour(ThemeManager::get(Theme::Role::knobThumb));
      g.fillRect(juce::Rectangle<float>(cap.getX() + 2.0f, thumbY - 0.8f,
                                        cap.getWidth() - 4.0f, 1.6f));
    }
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
    if (trackBounds.isEmpty() && !labelArea.contains(e.getPosition()))
      return;

    rightButtonGesture = e.mods.isPopupMenu();
    rightButtonDragged = false;
    dragging = true;
    fineDragActive = isFineModifier(e.mods);
    anchorMouseY = e.position.y;
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
      anchorNorm = normValue;
      fineDragActive = nowFine;
      sink.handleParamTouchState(paramIdx, true, fineDragActive);
    }

    const float baseSpan = juce::jmax(
        (float)UiMetrics::kFaderMinVirtualSpanPx,
        (float)(trackBounds.getHeight() - 12));
    const float effectiveSpan =
        fineDragActive ? (baseSpan * UiMetrics::kFineTuneGearDivisor) : baseSpan;

    const float deltaNorm = (anchorMouseY - e.position.y) / effectiveSpan;
    float candidate = juce::jlimit(0.0f, 1.0f, anchorNorm + deltaNorm);

    const auto &d = paramDefs()[(size_t)paramIdx];
    if (d.bipolar && !fineDragActive &&
        std::abs(candidate - 0.5f) < UiMetrics::kBipolarDetentRadius) {
      candidate = 0.5f;
    }

    applyNormalizedFromUi(candidate);
  }

  void mouseUp(const juce::MouseEvent &e) override {
    const bool showPopup = rightButtonGesture && !rightButtonDragged;
    dragging = false;
    fineDragActive = false;
    rightButtonGesture = false;
    sink.handleParamTouchState(paramIdx, false, false);
    repaint();

    if (showPopup)
      sink.showParamContextMenu(paramIdx, this);
    (void)e;
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
      const float nextVal = clampAndSnapParam(d, proc.getParam(paramIdx) + dir * d.step);
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
  juce::String shortTitle;
  SynthValueReadout readout;

  juce::Rectangle<int> labelArea;
  juce::Rectangle<int> trackBounds;
  float normValue = 0.0f;
  float anchorMouseY = 0.0f;
  float anchorNorm = 0.0f;

  bool hovered = false;
  bool dragging = false;
  bool fineDragActive = false;
  bool rightButtonGesture = false;
  bool rightButtonDragged = false;
  bool isDimmed = false;
};

} // namespace FanfareSynth::Ui

