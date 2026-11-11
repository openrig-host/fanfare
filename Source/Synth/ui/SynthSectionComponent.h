#pragma once

#include <JuceHeader.h>
#include <cmath>
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
#include "SynthSkin.h"

namespace FanfareSynth::Ui {

class SynthSectionComponent : public juce::Component, private juce::Timer {
public:
  SynthSectionComponent(const SectionLayoutSpec &layoutSpec,
                        FanfareSynthProcessor &processor,
                        SynthTelemetrySink &telemetry)
      : spec(layoutSpec), proc(processor), sink(telemetry) {
    jassert(verifyLayoutCompleteness());

    const auto &allSpecs = getJunoLayoutSpecs();
    for (size_t i = 0; i < allSpecs.size(); ++i)
      if (&allSpecs[i] == &layoutSpec) {
        sectionNumber = (int)i + 1;
        break;
      }

    for (const auto &ws : spec.primaryStrip)
      primaryWidgets.push_back(createAndOwnWidget(ws));

    deckWidgets.resize(spec.deckRows.size());
    for (size_t r = 0; r < spec.deckRows.size(); ++r) {
      for (const auto &ws : spec.deckRows[r].items)
        deckWidgets[r].push_back(createAndOwnWidget(ws));
    }

    if (spec.section == SecLfo || spec.section == SecAmpEnv ||
        spec.section == SecFltEnv)
      startTimerHz(24);
  }

  ~SynthSectionComponent() override { stopTimer(); }

  void timerCallback() override {
    if (!isVisible())
      return;
    if (spec.section == SecLfo) {
      repaint(ledArea.toNearestInt().expanded(5, 5));
      return;
    }
    const bool live = proc.getActiveVoiceCount() > 0;
    if (live != lastVoicesLive) {
      lastVoicesLive = live;
      repaint();
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

    const bool badgeChanged = (nextBadge != statusBadge);
    statusBadge = nextBadge;

    if (badgeChanged || spec.section == SecAmpEnv || spec.section == SecFltEnv)
      repaint();
  }

  SynthParamWidget *findWidgetForParam(int paramIdx) const noexcept {
    for (auto *w : ownedWidgets)
      if (w->getParamIndex() == paramIdx)
        return w;
    return nullptr;
  }

  void paint(juce::Graphics &g) override {
    rebuildChrome();
    chrome.draw(g, 0, 0);

    auto titleBox = getLocalBounds()
                        .removeFromTop(UiMetrics::kSectionHeaderHeight)
                        .reduced(6, 0);

    if (spec.section == SecLfo && !ledArea.isEmpty()) {
      const float rate = juce::jmax(0.05f, proc.getParam(ParamId::lfoRate));
      const float nowS =
          (float)(juce::Time::getMillisecondCounterHiRes() * 0.001);
      const float phase = std::fmod(nowS * rate, 1.0f);
      Skin::drawLed(g, ledArea, ThemeManager::get(Theme::Role::accent),
                    phase < 0.5f);
      titleBox.removeFromLeft(11);
    }

    g.setColour(ThemeManager::get(Theme::Role::textFaint).withAlpha(0.85f));
    g.setFont(BoutiqueLookAndFeel::monoFont(9.0f, true));
    g.drawText(juce::String(sectionNumber).paddedLeft('0', 2),
               titleBox.removeFromRight(20),
               juce::Justification::centredRight, false);

    if (statusBadge.isNotEmpty()) {
      auto badgeArea = titleBox.removeFromRight(58);
      Skin::drawGlowText(g, statusBadge, badgeArea,
                         BoutiqueLookAndFeel::condensedFont(10.5f, 0.04f),
                         ThemeManager::get(Theme::Role::accent),
                         juce::Justification::centredRight);
    }

    g.setColour(ThemeManager::get(Theme::Role::text));
    g.setFont(BoutiqueLookAndFeel::condensedFont(12.5f, 0.08f));
    g.drawFittedText(spec.title, titleBox, juce::Justification::centredLeft, 1);

    if (spec.section == SecAmpEnv || spec.section == SecFltEnv)
      drawEnvTrace(g, spec.section == SecFltEnv);
  }

  void resized() override {
    deckDividerY = -1;
    auto body = getLocalBounds()
                    .withTrimmedTop(UiMetrics::kSectionHeaderHeight)
                    .reduced(5, 4);

    ledArea = juce::Rectangle<float>(7.0f,
                                     (float)UiMetrics::kSectionHeaderHeight *
                                         0.5f - 2.5f,
                                     5.0f, 5.0f);
    traceArea = getLocalBounds()
                    .withTrimmedTop(UiMetrics::kSectionHeaderHeight + 6)
                    .reduced(9, 8)
                    .toFloat();

    if (!body.isEmpty()) {
      if (spec.tier == 0) {
        layoutControlCheek(body);
      } else {
        layoutBody(body);
      }
    }
    rebuildChrome();
  }

private:
  void layoutBody(juce::Rectangle<int> body) {
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
      const int maxKnobBandH = juce::jmin(body.getHeight(), 128);
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

  void rebuildChrome() {
    chrome.render(getWidth(), getHeight(),
                  Skin::themeSignature() + "|" + juce::String(deckDividerY),
                  [this](juce::Graphics &g, juce::Rectangle<int> b) {
                    const auto bf = b.toFloat();
                    Skin::drawPlate(g, bf, 3.0f);

                    const auto headerRect = juce::Rectangle<float>(
                        bf.getX(), bf.getY(), bf.getWidth(),
                        (float)UiMetrics::kSectionHeaderHeight);
                    const auto band =
                        ThemeManager::get(Theme::Role::panelAlt);
                    Skin::fillVertical(g, headerRect,
                                       Skin::lift(band, 0.16f),
                                       Skin::sink(band, 0.22f), 3.0f);
                    Skin::glowLine(g, bf.getX() + 2.0f, bf.getY() + 1.2f,
                                   bf.getRight() - 2.0f, bf.getY() + 1.2f,
                                   ThemeManager::get(Theme::Role::accentDim),
                                   1.2f);
                    g.setColour(ThemeManager::get(Theme::Role::border));
                    g.drawHorizontalLine(UiMetrics::kSectionHeaderHeight,
                                         bf.getX() + 1.0f, bf.getRight() - 1.0f);

                    if (deckDividerY > 0) {
                      g.setColour(juce::Colours::black.withAlpha(
                          Skin::isDark() ? 0.35f : 0.12f));
                      g.drawHorizontalLine(deckDividerY, bf.getX() + 4.0f,
                                           bf.getRight() - 4.0f);
                      g.setColour(juce::Colours::white.withAlpha(
                          Skin::isDark() ? 0.05f : 0.35f));
                      g.drawHorizontalLine(deckDividerY + 1, bf.getX() + 4.0f,
                                           bf.getRight() - 4.0f);
                    }
                  });
  }

  void drawEnvTrace(juce::Graphics &g, bool filt) const {
    if (primaryWidgets.size() < 4)
      return;

    auto *wA = primaryWidgets[0];
    auto *wD = primaryWidgets[1];
    auto *wS = primaryWidgets[2];
    auto *wR = primaryWidgets[3];
    if (wA == nullptr || wD == nullptr || wS == nullptr || wR == nullptr)
      return;

    const auto rA = wA->getBounds().toFloat();
    const auto rD = wD->getBounds().toFloat();
    const auto rS = wS->getBounds().toFloat();
    const auto rR = wR->getBounds().toFloat();
    if (rA.isEmpty() || rR.isEmpty())
      return;

    const float xA = rA.getCentreX();
    const float xD = rD.getCentreX();
    const float xS = rS.getCentreX();
    const float xR = rR.getCentreX();
    const float startX = rA.getX() + 6.0f;
    const float endX = rR.getRight() - 6.0f;

    // Match exact travel of SynthFader thumb
    const float trackTop = rA.getY() + (float)UiMetrics::kControlLabelHeight + 5.0f;
    const float trackBot = rA.getBottom() - (float)UiMetrics::kValueReadoutHeight - 5.0f;
    const float topY = trackTop + 6.5f;
    const float botY = trackBot - 6.5f;
    const float travel = juce::jmax(8.0f, botY - topY);

    const auto &dAtk = paramDefs()[(size_t)(filt ? ParamId::fatk : ParamId::atk)];
    const auto &dDec = paramDefs()[(size_t)(filt ? ParamId::fdec : ParamId::dec)];
    const auto &dRel = paramDefs()[(size_t)(filt ? ParamId::frel : ParamId::rel)];

    const float a = proc.getParam(filt ? ParamId::fatk : ParamId::atk);
    const float d = proc.getParam(filt ? ParamId::fdec : ParamId::dec);
    const float s = juce::jlimit(0.0f, 1.0f,
                                 proc.getParam(filt ? ParamId::fsus : ParamId::sus));
    const float r = proc.getParam(filt ? ParamId::frel : ParamId::rel);

    const float normA = valueToNormalized(dAtk, a);
    const float normD = valueToNormalized(dDec, d);
    const float normR = valueToNormalized(dRel, r);

    const float susY = botY - s * travel;

    const bool isGate = (!filt &&
                         (int)std::lround(proc.getParam(ParamId::vcaMode)) == 1 &&
                         (int)std::lround(proc.getParam(ParamId::filtEnvSrc)) == 0);

    juce::Path p;
    if (isGate) {
      p.startNewSubPath(startX, botY);
      p.lineTo(startX, topY);
      p.lineTo(endX, topY);
      p.lineTo(endX, botY);
    } else {
      p.startNewSubPath(startX, botY);

      // Attack: rises to (xA, topY)
      const float xAtk = startX + juce::jmax(2.0f, (xA - startX) * (0.08f + 0.92f * normA));
      if (xAtk < xA - 1.0f) {
        p.lineTo(xAtk, topY);
        p.lineTo(xA, topY);
      } else {
        p.quadraticTo(startX + (xA - startX) * 0.25f * (1.0f - normA), topY, xA, topY);
      }

      // Decay: falls from (xA, topY) to (xDecEnd, susY)
      const float decSpan = (xS - 4.0f) - xA;
      const float xDecEnd = xA + decSpan * (0.15f + 0.85f * normD);
      const float ctrlX = xA + (xDecEnd - xA) * 0.35f;
      const float ctrlY = susY + (topY - susY) * 0.20f;
      p.quadraticTo(ctrlX, ctrlY, xDecEnd, susY);

      // Sustain: flat plateau from xDecEnd across S fader
      const float xRelStart = juce::jmax(xDecEnd, xS + (xR - xS) * 0.30f);
      p.lineTo(xRelStart, susY);

      // Release: falls from (xRelStart, susY) down to botY
      const float relSpan = endX - xRelStart;
      const float xRelEnd = xRelStart + juce::jmax(4.0f, relSpan * (0.12f + 0.88f * normR));
      const float rCtrlX = xRelStart + (xRelEnd - xRelStart) * 0.35f;
      const float rCtrlY = botY + (susY - botY) * 0.20f;
      p.quadraticTo(rCtrlX, rCtrlY, xRelEnd, botY);
      if (xRelEnd < endX)
        p.lineTo(endX, botY);
    }

    const bool live = proc.getActiveVoiceCount() > 0;
    const float alpha = isGate ? 0.18f : (live ? 0.55f : 0.28f);
    Skin::glowStroke(g, p,
                     ThemeManager::get(Theme::Role::accent).withAlpha(alpha),
                     1.3f);
  }

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
  int sectionNumber = 0;
  bool lastVoicesLive = false;
  juce::Rectangle<float> ledArea;
  juce::Rectangle<float> traceArea;
  Skin::ChromeCache chrome;

  juce::OwnedArray<SynthParamWidget> ownedWidgets;
  std::vector<SynthParamWidget *> primaryWidgets;
  std::vector<std::vector<SynthParamWidget *>> deckWidgets;
};

} // namespace FanfareSynth::Ui

