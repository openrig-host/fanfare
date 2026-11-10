#pragma once

#include <JuceHeader.h>
#include <vector>
#include "../../BoutiqueLookAndFeel.h"
#include "../../ThemeManager.h"
#include "../FanfareSynthProcessorImpl.h"
#include "../SynthConstants.h"
#include "SynthControlCallbacks.h"
#include "SynthFader.h"
#include "SynthKnob.h"
#include "SynthLayoutSpec.h"
#include "SynthLedSwitch.h"

namespace FanfareSynth::Ui {

class SynthSectionComponent : public juce::Component {
public:
  SynthSectionComponent(const SectionLayoutSpec &layoutSpec,
                        FanfareSynthProcessor &processor,
                        SynthTelemetrySink &telemetry)
      : spec(layoutSpec), proc(processor), sink(telemetry) {
    jassert(verifyLayoutCompleteness());

    for (const auto &ws : spec.primaryStrip)
      primaryWidgets.push_back(createAndOwnWidget(ws));

    deckWidgets.resize(spec.deckRows.size());
    for (size_t r = 0; r < spec.deckRows.size(); ++r) {
      for (const auto &ws : spec.deckRows[r].items)
        deckWidgets[r].push_back(createAndOwnWidget(ws));
    }
  }

  SectionId getSectionId() const noexcept { return spec.section; }
  float getWidthWeight() const noexcept { return spec.widthWeight; }

  void syncAllControls() {
    for (auto *w : ownedWidgets)
      w->syncFromProcessor();
    refreshDimmingAndBadges();
  }

  void refreshDimmingAndBadges() {
    for (auto *w : ownedWidgets) {
      const bool active = isParamEffectivelyActive(w->getParamIndex(), proc);
      w->setControlDimmed(!active);
    }

    juce::String nextBadge;
    if (spec.section == SecFltEnv &&
        (int)std::lround(proc.getParam(ParamId::filtEnvSrc)) == 1) {
      nextBadge = "AMP ENV";
    } else if (spec.section == SecAmpEnv &&
               (int)std::lround(proc.getParam(ParamId::vcaMode)) == 1 &&
               (int)std::lround(proc.getParam(ParamId::filtEnvSrc)) == 0) {
      nextBadge = "GATE";
    } else if (spec.section == SecChorus) {
      const int m = (int)std::lround(proc.getParam(ParamId::choMode));
      if (m == 1)
        nextBadge = "BBD I";
      else if (m == 2)
        nextBadge = "BBD II";
      else if (m == 3)
        nextBadge = "I+II";
    }

    if (nextBadge != statusBadge) {
      statusBadge = nextBadge;
      repaint();
    }
  }

  SynthParamWidget *findWidgetForParam(int paramIdx) const noexcept {
    for (auto *w : ownedWidgets)
      if (w->getParamIndex() == paramIdx)
        return w;
    return nullptr;
  }

  void paint(juce::Graphics &g) override {
    const auto b = getLocalBounds().toFloat();
    g.setColour(ThemeManager::get(Theme::Role::panel));
    g.fillRoundedRectangle(b, 3.0f);

    const auto headerRect =
        juce::Rectangle<float>(b.getX(), b.getY(), b.getWidth(),
                               (float)UiMetrics::kSectionHeaderHeight);
    g.setColour(ThemeManager::get(Theme::Role::panelAlt));
    g.fillRoundedRectangle(headerRect, 3.0f);

    // Top silk-screened Juno accent stripe
    g.setColour(ThemeManager::get(Theme::Role::accentDim));
    g.fillRect(juce::Rectangle<float>(b.getX() + 1.0f, b.getY() + 0.5f,
                                      b.getWidth() - 2.0f, 1.5f));

    g.setColour(ThemeManager::get(Theme::Role::border));
    g.drawRoundedRectangle(b.reduced(0.5f), 3.0f, 1.0f);
    g.drawHorizontalLine(UiMetrics::kSectionHeaderHeight, b.getX() + 1.0f,
                         b.getRight() - 1.0f);

    auto titleBox =
        getLocalBounds().removeFromTop(UiMetrics::kSectionHeaderHeight).reduced(6, 0);
    if (statusBadge.isNotEmpty()) {
      auto badgeArea = titleBox.removeFromRight(58);
      g.setColour(ThemeManager::get(Theme::Role::accent));
      g.setFont(BoutiqueLookAndFeel::condensedFont(10.5f, 0.04f));
      g.drawFittedText(statusBadge, badgeArea, juce::Justification::centredRight,
                       1);
    }

    g.setColour(ThemeManager::get(Theme::Role::text));
    g.setFont(BoutiqueLookAndFeel::condensedFont(12.5f, 0.08f));
    g.drawFittedText(spec.title, titleBox, juce::Justification::centredLeft, 1);

    if (deckDividerY > 0) {
      g.setColour(ThemeManager::get(Theme::Role::border));
      g.drawHorizontalLine(deckDividerY, b.getX() + 4.0f, b.getRight() - 4.0f);
    }
  }

  void resized() override {
    deckDividerY = -1;
    auto body = getLocalBounds()
                    .withTrimmedTop(UiMetrics::kSectionHeaderHeight)
                    .reduced(5, 4);
    if (body.isEmpty())
      return;

    if (spec.tier == 0) {
      layoutControlCheek(body);
      return;
    }

    const bool fxKnobSection =
        (spec.section == SecChorus || spec.section == SecDelay ||
         spec.section == SecReverb);

    if (fxKnobSection) {
      if (!spec.deckRows.empty()) {
        for (size_t r = 0; r < spec.deckRows.size(); ++r) {
          const int rh = juce::jmin(body.getHeight() / 3,
                                    spec.deckRows[r].preferredHeight);
          layoutWeightedRow(body.removeFromTop(rh), spec.deckRows[r].items,
                            deckWidgets[r]);
          body.removeFromTop(4);
        }
        deckDividerY = body.getY() - 2;
      }
      const int maxKnobBandH = juce::jmin(body.getHeight(), 96);
      auto knobBand =
          body.withSizeKeepingCentre(body.getWidth(), maxKnobBandH);
      layoutWeightedRow(knobBand, spec.primaryStrip, primaryWidgets);
      return;
    }

    // Voice & Envelope sections: deck rows anchored at bottom, faders fill top
    if (!spec.deckRows.empty()) {
      int totalDeckH = 0;
      for (const auto &dr : spec.deckRows)
        totalDeckH += dr.preferredHeight;
      totalDeckH += (int)(spec.deckRows.size() - 1) * 3;

      auto deckArea = body.removeFromBottom(juce::jmin(body.getHeight() / 2, totalDeckH));
      body.removeFromBottom(5);
      deckDividerY = body.getBottom() + 2;

      for (size_t r = 0; r < spec.deckRows.size(); ++r) {
        const int rh = (r + 1 == spec.deckRows.size())
                           ? deckArea.getHeight()
                           : spec.deckRows[r].preferredHeight;
        layoutWeightedRow(deckArea.removeFromTop(rh), spec.deckRows[r].items,
                          deckWidgets[r]);
        deckArea.removeFromTop(3);
      }
    }

    layoutWeightedRow(body, spec.primaryStrip, primaryWidgets);
  }

private:
  SynthParamWidget *createAndOwnWidget(const WidgetSpec &ws) {
    const juce::String customLbl(ws.customLabel != nullptr ? ws.customLabel
                                                           : "");
    SynthParamWidget *w = nullptr;
    switch (ws.kind) {
    case WidgetKind::Fader:
      w = new SynthFader(proc, ws.param, sink, customLbl);
      break;
    case WidgetKind::Knob:
      w = new SynthKnob(proc, ws.param, sink, false, customLbl);
      break;
    case WidgetKind::KnobCompact:
      w = new SynthKnob(proc, ws.param, sink, true, customLbl);
      break;
    case WidgetKind::SwitchStacked:
      w = new SynthLedSwitch(proc, ws.param, sink, false, customLbl, true);
      break;
    case WidgetKind::SwitchInline:
      w = new SynthLedSwitch(proc, ws.param, sink, true, customLbl, true);
      break;
    case WidgetKind::SwitchNoLabel:
      w = new SynthLedSwitch(proc, ws.param, sink, false, "", false);
      break;
    }
    addAndMakeVisible(w);
    ownedWidgets.add(w);
    return w;
  }

  static void layoutWeightedRow(juce::Rectangle<int> area,
                                const std::vector<WidgetSpec> &specs,
                                const std::vector<SynthParamWidget *> &widgets) {
    const size_t n = juce::jmin(specs.size(), widgets.size());
    if (n == 0 || area.isEmpty())
      return;

    float totalWeight = 0.0f;
    for (size_t i = 0; i < n; ++i)
      totalWeight += juce::jmax(0.1f, specs[i].widthWeight);

    const int gap = (n > 1) ? 3 : 0;
    const int availW = juce::jmax(1, area.getWidth() - (int)(n - 1) * gap);
    int curX = area.getX();
    for (size_t i = 0; i < n; ++i) {
      const int itemW =
          (i + 1 == n)
              ? (area.getRight() - curX)
              : (int)std::lround((float)availW *
                                 (specs[i].widthWeight / totalWeight));
      widgets[i]->setBounds(curX, area.getY(), juce::jmax(8, itemW),
                            area.getHeight());
      curX += itemW + gap;
    }
  }

  void layoutControlCheek(juce::Rectangle<int> body) {
    int totalDeckH = 0;
    for (const auto &dr : spec.deckRows)
      totalDeckH += dr.preferredHeight;
    totalDeckH += (int)spec.deckRows.size() * 4;

    auto deckArea =
        body.removeFromBottom(juce::jmin((int)(body.getHeight() * 0.62f), totalDeckH));
    body.removeFromBottom(6);
    deckDividerY = body.getBottom() + 3;

    // Top band: Master VOL fader on left, stacked BEND + DRIFT knobs on right
    if (primaryWidgets.size() >= 3) {
      const int leftW = juce::jlimit(52, 68, (int)(body.getWidth() * 0.42f));
      auto volCol = body.removeFromLeft(leftW);
      body.removeFromLeft(4);
      primaryWidgets[0]->setBounds(volCol);

      const int halfH = (body.getHeight() - 4) / 2;
      primaryWidgets[1]->setBounds(body.removeFromTop(halfH));
      body.removeFromTop(4);
      primaryWidgets[2]->setBounds(body);
    }

    for (size_t r = 0; r < spec.deckRows.size(); ++r) {
      const int rh = (r + 1 == spec.deckRows.size())
                         ? deckArea.getHeight()
                         : spec.deckRows[r].preferredHeight;
      layoutWeightedRow(deckArea.removeFromTop(rh), spec.deckRows[r].items,
                        deckWidgets[r]);
      deckArea.removeFromTop(4);
    }
  }

  SectionLayoutSpec spec;
  FanfareSynthProcessor &proc;
  SynthTelemetrySink &sink;
  juce::String statusBadge;
  int deckDividerY = -1;

  juce::OwnedArray<SynthParamWidget> ownedWidgets;
  std::vector<SynthParamWidget *> primaryWidgets;
  std::vector<std::vector<SynthParamWidget *>> deckWidgets;
};

} // namespace FanfareSynth::Ui

