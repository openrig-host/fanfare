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

namespace FanfareSynth::Ui {

class SynthLedSwitch : public SynthParamWidget {
public:
  SynthLedSwitch(FanfareSynthProcessor &processor, int parameterIndex,
                 SynthTelemetrySink &telemetry, bool inlineLabelLeft = false,
                 const juce::String &overrideLabel = {},
                 bool showTitle = true)
      : SynthParamWidget(parameterIndex), proc(processor), sink(telemetry),
        inlineLeft(inlineLabelLeft) {
    const auto &d = paramDefs()[(size_t)paramIdx];
    if (showTitle) {
      titleText = overrideLabel.isNotEmpty() ? overrideLabel
                                             : juce::String(d.shortLabel);
    }
    options = juce::StringArray::fromTokens(
        d.choices != nullptr ? d.choices : "", ",", "");
    setMouseCursor(juce::MouseCursor::PointingHandCursor);
    syncFromProcessor();
  }

  void syncFromProcessor() override {
    const auto &d = paramDefs()[(size_t)paramIdx];
    selectedIndex = juce::jlimit(0, juce::jmax(0, options.size() - 1),
                                 (int)std::lround(proc.getParam(paramIdx)) -
                                     (int)std::lround(d.min));
    const int cc = proc.getCCForParamIndex(paramIdx);
    juce::String tip = juce::String(d.label) + ": " +
                       (selectedIndex < options.size() ? options[selectedIndex]
                                                       : juce::String());
    if (cc >= 0)
      tip += "  [CC " + juce::String(cc) + "]";
    setTooltip(tip);
    repaint();
  }

  void setControlDimmed(bool dimmed) override {
    if (isDimmed == dimmed)
      return;
    isDimmed = dimmed;
    setAlpha(dimmed ? UiMetrics::kDisabledControlAlpha : 1.0f);
    repaint();
  }

  void resized() override {
    auto b = getLocalBounds();
    if (titleText.isEmpty()) {
      titleArea = {};
      stripArea = b.reduced(1, 1);
    } else if (inlineLeft) {
      const int lw = juce::jlimit(28, 48, (int)(titleText.length() * 6 + 6));
      titleArea = b.removeFromLeft(lw);
      stripArea = b.reduced(1, 1);
    } else {
      titleArea = b.removeFromTop(12);
      stripArea = b.reduced(1, 1);
    }
  }

  void paint(juce::Graphics &g) override {
    if (!titleArea.isEmpty()) {
      g.setColour(ThemeManager::get(Theme::Role::engrave));
      g.setFont(BoutiqueLookAndFeel::condensedFont(10.5f, 0.04f));
      g.drawFittedText(titleText, titleArea,
                       inlineLeft ? juce::Justification::centredLeft
                                  : juce::Justification::centred,
                       1, 0.80f);
    }

    const int n = options.size();
    if (n <= 0 || stripArea.isEmpty())
      return;

    using namespace Skin;
    const auto outer = stripArea.toFloat();
    drawRecess(g, outer, 2.5f);

    const float segW = outer.getWidth() / (float)n;
    for (int i = 0; i < n; ++i) {
      const auto seg =
          juce::Rectangle<float>(outer.getX() + (float)i * segW, outer.getY(),
                                 segW, outer.getHeight())
              .reduced(1.4f, 1.4f);
      const bool on = (i == selectedIndex);
      const bool segHover = (hoveredIndex == i);
      const bool pressed = (pressedIndex == i);

      if (i > 0) {
        g.setColour(juce::Colours::black.withAlpha(isDark() ? 0.45f : 0.18f));
        g.drawVerticalLine((int)std::lround(outer.getX() + (float)i * segW),
                           outer.getY() + 2.0f, outer.getBottom() - 2.0f);
      }

      if (on) {
        const auto top = ThemeManager::get(Theme::Role::faceTop);
        const auto bot = ThemeManager::get(Theme::Role::faceBot);
        fillVertical(g, seg, Skin::lift(top, 0.28f), Skin::sink(bot, 0.05f),
                     2.0f);
        g.setColour(ThemeManager::get(Theme::Role::accent).withAlpha(0.14f));
        g.fillRoundedRectangle(seg.reduced(1.0f), 1.5f);
        g.setColour(ThemeManager::get(Theme::Role::accent).withAlpha(0.85f));
        g.drawRoundedRectangle(seg.reduced(0.5f), 2.0f, 1.0f);
      } else if (pressed) {
        fillVertical(g, seg,
                     ThemeManager::get(Theme::Role::pressTop),
                     ThemeManager::get(Theme::Role::pressBot), 2.0f);
      } else if (segHover) {
        g.setColour(ThemeManager::get(Theme::Role::panelAlt));
        g.fillRoundedRectangle(seg, 2.0f);
        g.setColour(juce::Colours::white.withAlpha(isDark() ? 0.05f : 0.25f));
        g.fillRoundedRectangle(seg.withHeight(seg.getHeight() * 0.5f), 2.0f);
      }

      const float ledW = juce::jlimit(6.0f, 14.0f, seg.getWidth() * 0.48f);
      const juce::Rectangle<float> led(seg.getCentreX() - ledW * 0.5f,
                                       seg.getY() + 2.2f, ledW, 2.4f);
      drawLed(g, led, ThemeManager::get(Theme::Role::accent), on);

      auto textRect = seg.withTrimmedTop(5.0f).toNearestInt();
      g.setColour(ThemeManager::get(on ? Theme::Role::text
                                       : Theme::Role::textDim));
      g.setFont(BoutiqueLookAndFeel::condensedFont(11.0f, 0.03f));
      g.drawFittedText(options[i], textRect, juce::Justification::centred, 1,
                       0.75f);
    }
  }

  void mouseEnter(const juce::MouseEvent &e) override {
    sink.handleParamHovered(paramIdx);
    updateHoveredSegment(e.position);
  }

  void mouseMove(const juce::MouseEvent &e) override {
    updateHoveredSegment(e.position);
  }

  void mouseExit(const juce::MouseEvent &) override {
    hoveredIndex = -1;
    repaint();
  }

  void mouseDown(const juce::MouseEvent &e) override {
    if (e.mods.isPopupMenu()) {
      sink.showParamContextMenu(paramIdx, this);
      return;
    }
    const int n = options.size();
    if (n <= 0 || !stripArea.toFloat().contains(e.position))
      return;

    const float relX = e.position.x - (float)stripArea.getX();
    const int clicked = juce::jlimit(
        0, n - 1, (int)(relX / ((float)stripArea.getWidth() / (float)n)));
    pressedIndex = clicked;
    repaint();
    selectIndexFromUi(clicked);
  }

  void mouseUp(const juce::MouseEvent &) override {
    if (pressedIndex >= 0) {
      pressedIndex = -1;
      repaint();
    }
  }

  void mouseDoubleClick(const juce::MouseEvent &e) override {
    if (e.mods.isPopupMenu())
      return;
    const auto &d = paramDefs()[(size_t)paramIdx];
    proc.setParam(paramIdx, d.def, true);
    syncFromProcessor();
    sink.handleParamEdited(paramIdx);
  }

  void mouseWheelMove(const juce::MouseEvent &,
                      const juce::MouseWheelDetails &wheel) override {
    if (std::abs(wheel.deltaY) <= 1e-5f || options.isEmpty())
      return;
    const int dir = (wheel.deltaY > 0.0f) ? 1 : -1;
    const int next =
        juce::jlimit(0, options.size() - 1, selectedIndex + dir);
    if (next != selectedIndex)
      selectIndexFromUi(next);
  }

private:
  void updateHoveredSegment(juce::Point<float> p) {
    const int n = options.size();
    int nextHover = -1;
    if (n > 0 && stripArea.toFloat().contains(p)) {
      const float relX = p.x - (float)stripArea.getX();
      nextHover = juce::jlimit(
          0, n - 1, (int)(relX / ((float)stripArea.getWidth() / (float)n)));
    }
    if (nextHover != hoveredIndex) {
      hoveredIndex = nextHover;
      repaint();
    }
  }

  void selectIndexFromUi(int idxChoice) {
    const auto &d = paramDefs()[(size_t)paramIdx];
    proc.setParam(paramIdx, d.min + (float)idxChoice, true);
    syncFromProcessor();
    sink.handleParamEdited(paramIdx);
  }

  FanfareSynthProcessor &proc;
  SynthTelemetrySink &sink;
  bool inlineLeft = false;
  juce::String titleText;
  juce::StringArray options;
  juce::Rectangle<int> titleArea;
  juce::Rectangle<int> stripArea;
  int selectedIndex = 0;
  int hoveredIndex = -1;
  int pressedIndex = -1;
  bool isDimmed = false;
};

} // namespace FanfareSynth::Ui

