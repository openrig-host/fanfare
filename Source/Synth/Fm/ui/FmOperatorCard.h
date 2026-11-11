#pragma once

#include <JuceHeader.h>
#include <array>
#include <cmath>
#include <functional>
#include <memory>
#include "../FmCore4.h"
#include "../FmSynthProcessor.h"
#include "FmControls.h"
#include "FmUiMetrics.h"

namespace FanfareFm::Ui {

class FmOpWaveButton : public juce::Component,
                       public juce::SettableTooltipClient {
public:
  FmOpWaveButton() { setMouseCursor(juce::MouseCursor::PointingHandCursor); }

  std::function<void(int)> onWaveSelected;

  void setStateSilent(int waveIdx, bool multiWaveAvailable) {
    const int w = waveIdx & 7;
    if (wave == w && enabledMode == multiWaveAvailable)
      return;
    wave = w;
    enabledMode = multiWaveAvailable;
    setTooltip(enabledMode
                   ? ("Operator Waveform: " + juce::String(txWaveName(wave)) +
                      " (Left-click to cycle, Right-click for list)")
                   : "Operator inactive in 4-operator architecture");
    repaint();
  }

  void paint(juce::Graphics &g) override {
    const auto b = getLocalBounds().toFloat().reduced(0.5f);
    g.setColour(ThemeManager::get(enabledMode ? Theme::Role::raised
                                              : Theme::Role::sunken));
    g.fillRoundedRectangle(b, 2.5f);
    g.setColour(enabledMode
                    ? ThemeManager::get(hovered ? Theme::Role::accent
                                                : Theme::Role::borderStrong)
                    : ThemeManager::get(Theme::Role::border));
    g.drawRoundedRectangle(b, 2.5f, 0.9f);

    if (!enabledMode) {
      g.setColour(ThemeManager::get(Theme::Role::textFaint));
      g.setFont(BoutiqueLookAndFeel::condensedFont(9.5f, 0.03f));
      g.drawFittedText("SIN", getLocalBounds(), juce::Justification::centred, 1);
      return;
    }

    auto inner = getLocalBounds().reduced(2, 2);
    auto waveBox = inner.removeFromLeft(juce::jmin(12, inner.getWidth() / 2))
                       .toFloat()
                       .reduced(0.5f, 2.0f);
    if (waveBox.getWidth() >= 6.0f) {
      juce::Path p;
      constexpr int kSamples = 16;
      for (int i = 0; i <= kSamples; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(kSamples);
        const float s = sampleTxWavePreview(wave, t);
        const float x = waveBox.getX() + t * waveBox.getWidth();
        const float y = waveBox.getCentreY() - s * (waveBox.getHeight() * 0.45f);
        if (i == 0)
          p.startNewSubPath(x, y);
        else
          p.lineTo(x, y);
      }
      g.setColour(ThemeManager::get(Theme::Role::accent));
      g.strokePath(p, juce::PathStrokeType(1.1f));
    }

    g.setColour(ThemeManager::get(Theme::Role::text));
    g.setFont(BoutiqueLookAndFeel::monoFont(9.2f, true));
    g.drawFittedText("W" + juce::String(wave + 1), inner,
                     juce::Justification::centred, 1);
  }

  void mouseEnter(const juce::MouseEvent &) override {
    hovered = true;
    repaint();
  }
  void mouseExit(const juce::MouseEvent &) override {
    hovered = false;
    repaint();
  }

  void mouseDown(const juce::MouseEvent &e) override {
    if (!enabledMode)
      return;
    if (e.mods.isPopupMenu()) {
      juce::PopupMenu m;
      m.addSectionHeader("TX81Z Operator Waveform");
      for (int i = 0; i < 8; ++i)
        m.addItem(i + 1, txWaveName(i), true, i == wave);
      juce::Component::SafePointer<FmOpWaveButton> safeThis(this);
      m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this),
                      [safeThis](int r) {
                        if (safeThis != nullptr && r >= 1 && r <= 8) {
                          safeThis->wave = r - 1;
                          if (safeThis->onWaveSelected)
                            safeThis->onWaveSelected(safeThis->wave);
                          safeThis->repaint();
                        }
                      });
      return;
    }
    wave = (wave + 1) & 7;
    setStateSilent(wave, enabledMode);
    if (onWaveSelected)
      onWaveSelected(wave);
    repaint();
  }

private:
  int wave = 0;
  bool enabledMode = false;
  bool hovered = false;
};

class FmOperatorCard : public juce::Component {
public:
  FmOperatorCard(FmSynthProcessor &processor, int displayOpOneBased,
                 FmTelemetrySink &telemetry, std::function<void()> onPatchCommit)
      : proc(processor), dispOp(juce::jlimit(1, 6, displayOpOneBased)),
        sink(telemetry), commitCallback(std::move(onPatchCommit)),
        onSwitch("ON", "OFF", Theme::Role::ok,
                 "Enable or mute Operator " + juce::String(dispOp)),
        fixSwitch("FIX", "RAT", Theme::Role::warn,
                  "Frequency Mode: Ratio (RAT) vs Fixed Hz (FIX)"),
        crsKnob("CRS", "OP" + juce::String(dispOp) + " Coarse", 1.0f / 31.0f,
                false, 31),
        fineKnob("FINE", "OP" + juce::String(dispOp) + " Fine", 0.0f, false, 99),
        detKnob("DET", "OP" + juce::String(dispOp) + " Detune", 0.5f, true, 14),
        outFader("OUT", "OP" + juce::String(dispOp) + " Output Level", 99,
                 dispOp == 1 ? 99 : 0),
        velFader("VEL", "OP" + juce::String(dispOp) + " Velocity Sens", 7, 0),
        keyFader("KEY", "OP" + juce::String(dispOp) + " Key Rate Scaling", 7, 0),
        modFader("MOD", "OP" + juce::String(dispOp) + " Amp Mod Sens", 3, 0) {
    const juce::String opPrefix = "OP" + juce::String(dispOp) + " ";

    onSwitch.onToggled = [this, opPrefix](bool state) {
      if (!isOpActiveInMode())
        return;
      auto &p = proc.engine().patch();
      const int eOp = engineOpIndex();
      if (state)
        p.d[155] |= static_cast<uint8_t>(1u << eOp);
      else
        p.d[155] &= static_cast<uint8_t>(~(1u << eOp));
      sink.handleTelemetryEdited(opPrefix + (state ? "ENABLED" : "MUTED"));
      commitCallback();
      repaint();
    };
    addAndMakeVisible(onSwitch);

    fixSwitch.onToggled = [this, opPrefix](bool fixedHz) {
      if (!isOpActiveInMode())
        return;
      auto &p = proc.engine().patch();
      const int off = patchByteOffset();
      p.d[off + 17] = fixedHz ? 1 : 0;
      commitCallback();
      sink.handleTelemetryEdited(
          opPrefix + (fixedHz ? "Fixed Mode: " : "Ratio Mode: ") +
          formatOpFrequency(p.d[off + 17], p.d[off + 18], p.d[off + 19]));
      repaint();
    };
    addAndMakeVisible(fixSwitch);

    waveBtn.onWaveSelected = [this, opPrefix](int w) {
      if (!isOpActiveInMode())
        return;
      auto &p = proc.engine().patch();
      p.d[156 + engineOpIndex()] = static_cast<uint8_t>(w & 7);
      sink.handleTelemetryEdited(opPrefix + "Waveform: " + txWaveName(w));
      commitCallback();
    };
    addAndMakeVisible(waveBtn);

    for (auto *k : {&crsKnob, &fineKnob, &detKnob}) {
      k->setTelemetrySink(&sink);
      addAndMakeVisible(*k);
    }

    crsKnob.formatReadout = [](float n) {
      return juce::String(static_cast<int>(std::lround(n * 31.0f)));
    };
    crsKnob.formatTelemetry = [this, opPrefix](float n) {
      const auto &p = proc.engine().patch();
      const int off = patchByteOffset();
      const int c = static_cast<int>(std::lround(n * 31.0f));
      return opPrefix + "Coarse: " + juce::String(c) + "  (" +
             formatOpFrequency(p.d[off + 17], c, p.d[off + 19]) + ")";
    };
    crsKnob.onNormChanged = [this](float n) {
      if (!isOpActiveInMode())
        return;
      proc.engine().patch().d[patchByteOffset() + 18] =
          static_cast<uint8_t>(juce::jlimit(0, 31, static_cast<int>(std::lround(n * 31.0f))));
      commitCallback();
      repaint(freqReadoutRect);
    };

    fineKnob.formatReadout = [](float n) {
      return juce::String(static_cast<int>(std::lround(n * 99.0f)))
          .paddedLeft('0', 2);
    };
    fineKnob.formatTelemetry = [this, opPrefix](float n) {
      const auto &p = proc.engine().patch();
      const int off = patchByteOffset();
      const int f = static_cast<int>(std::lround(n * 99.0f));
      return opPrefix + "Fine: " + juce::String(f) + "  (" +
             formatOpFrequency(p.d[off + 17], p.d[off + 18], f) + ")";
    };
    fineKnob.onNormChanged = [this](float n) {
      if (!isOpActiveInMode())
        return;
      proc.engine().patch().d[patchByteOffset() + 19] =
          static_cast<uint8_t>(juce::jlimit(0, 99, static_cast<int>(std::lround(n * 99.0f))));
      commitCallback();
      repaint(freqReadoutRect);
    };

    detKnob.formatReadout = [](float n) {
      return formatDetune(static_cast<int>(std::lround(n * 14.0f)));
    };
    detKnob.formatTelemetry = [opPrefix](float n) {
      return opPrefix + "Detune: " +
             formatDetune(static_cast<int>(std::lround(n * 14.0f)));
    };
    detKnob.onNormChanged = [this](float n) {
      if (!isOpActiveInMode())
        return;
      proc.engine().patch().d[patchByteOffset() + 20] =
          static_cast<uint8_t>(juce::jlimit(0, 14, static_cast<int>(std::lround(n * 14.0f))));
      commitCallback();
    };

    static constexpr const char *kRateShort[4] = {"R1", "R2", "R3", "R4"};
    static constexpr const char *kLevelShort[4] = {"L1", "L2", "L3", "L4"};
    static constexpr const int kDefLevels[4] = {99, 99, 99, 0};

    for (int i = 0; i < 4; ++i) {
      levelFaders[(size_t)i] = std::make_unique<FmFader>(
          kLevelShort[i], opPrefix + "EG Level " + juce::String(i + 1), 99,
          kDefLevels[i]);
      levelFaders[(size_t)i]->setTelemetrySink(&sink);
      levelFaders[(size_t)i]->onValueChanged = [this, i](int v) {
        if (!isOpActiveInMode())
          return;
        proc.engine().patch().d[patchByteOffset() + 4 + i] = static_cast<uint8_t>(v);
        commitCallback();
        repaint(envGraphRect);
      };
      addAndMakeVisible(*levelFaders[(size_t)i]);

      rateFaders[(size_t)i] = std::make_unique<FmFader>(
          kRateShort[i], opPrefix + "EG Rate " + juce::String(i + 1), 99, 99);
      rateFaders[(size_t)i]->setTelemetrySink(&sink);
      rateFaders[(size_t)i]->onValueChanged = [this, i](int v) {
        if (!isOpActiveInMode())
          return;
        proc.engine().patch().d[patchByteOffset() + i] = static_cast<uint8_t>(v);
        commitCallback();
        repaint(envGraphRect);
      };
      addAndMakeVisible(*rateFaders[(size_t)i]);
    }

    for (auto *f : {&outFader, &velFader, &keyFader, &modFader}) {
      f->setTelemetrySink(&sink);
      addAndMakeVisible(*f);
    }

    outFader.onValueChanged = [this](int v) {
      if (!isOpActiveInMode())
        return;
      proc.engine().patch().d[patchByteOffset() + 16] = static_cast<uint8_t>(v);
      commitCallback();
    };
    velFader.onValueChanged = [this](int v) {
      if (!isOpActiveInMode())
        return;
      proc.engine().patch().d[patchByteOffset() + 15] = static_cast<uint8_t>(v);
      commitCallback();
    };
    keyFader.onValueChanged = [this](int v) {
      if (!isOpActiveInMode())
        return;
      proc.engine().patch().d[patchByteOffset() + 13] = static_cast<uint8_t>(v);
      commitCallback();
    };
    modFader.onValueChanged = [this](int v) {
      if (!isOpActiveInMode())
        return;
      proc.engine().patch().d[patchByteOffset() + 14] = static_cast<uint8_t>(v);
      commitCallback();
    };
  }

  bool anyDragging() const noexcept {
    if (crsKnob.isDraggingNow() || fineKnob.isDraggingNow() ||
        detKnob.isDraggingNow() || outFader.isDraggingNow() ||
        velFader.isDraggingNow() || keyFader.isDraggingNow() ||
        modFader.isDraggingNow()) {
      return true;
    }
    for (int i = 0; i < 4; ++i) {
      if (levelFaders[(size_t)i]->isDraggingNow() ||
          rateFaders[(size_t)i]->isDraggingNow()) {
        return true;
      }
    }
    return false;
  }

  void syncFromProcessor() {
    const auto &e = proc.engine();
    const auto &p = e.patch();
    const bool active = isOpActiveInMode();
    const int eOp = engineOpIndex();
    const int off = patchByteOffset();

    isCarrierRole =
        active && ((e.numOps() == 6)
                       ? FmCore6W::isAudioCarrier(e.effectiveAlg(), eOp)
                       : FmCore4::isCarrier(e.effectiveAlg(), eOp));

    const Theme::Role roleCol =
        isCarrierRole ? Theme::Role::foh : Theme::Role::iem;
    outFader.setAccentRole(roleCol, roleCol);

    const bool opEnabled = active && (((p.d[155] >> eOp) & 1u) != 0);
    onSwitch.setStateSilent(opEnabled);
    onSwitch.setEnabled(active);

    fixSwitch.setStateSilent(p.d[off + 17] != 0);
    waveBtn.setStateSilent(active ? (p.d[156 + eOp] & 7) : 0, active);

    crsKnob.setNormSilent(static_cast<float>(p.d[off + 18]) / 31.0f);
    fineKnob.setNormSilent(static_cast<float>(p.d[off + 19]) / 99.0f);
    detKnob.setNormSilent(static_cast<float>(p.d[off + 20]) / 14.0f);

    for (int i = 0; i < 4; ++i) {
      rateFaders[(size_t)i]->setValueSilent(p.d[off + i]);
      levelFaders[(size_t)i]->setValueSilent(p.d[off + 4 + i]);
    }
    outFader.setValueSilent(p.d[off + 16]);
    velFader.setValueSilent(p.d[off + 15]);
    keyFader.setValueSilent(p.d[off + 13]);
    modFader.setValueSilent(p.d[off + 14]);

    const float contentAlpha =
        !active ? Metrics::kDisabledAlpha : (opEnabled ? 1.0f : 0.48f);
    for (auto *c : getChildren()) {
      if (c == &onSwitch) {
        c->setAlpha(active ? 1.0f : Metrics::kDisabledAlpha);
        continue;
      }
      c->setAlpha(contentAlpha);
      c->setInterceptsMouseClicks(active, active);
    }

    repaint();
  }

  void resized() override {
    auto b = getLocalBounds();
    auto header = b.removeFromTop(Metrics::kSectionHeaderHeight);
    onSwitch.setBounds(header.removeFromRight(34).reduced(3, 2));
    roleBadgeRect = header.removeFromRight(32).reduced(2, 3);
    opTitleRect = header.reduced(5, 0);

    auto body = b.reduced(4, 4);
    if (body.isEmpty())
      return;

    auto freqRow = body.removeFromTop(19);
    waveBtn.setBounds(freqRow.removeFromRight(29));
    freqRow.removeFromRight(2);
    fixSwitch.setBounds(freqRow.removeFromRight(27));
    freqRow.removeFromRight(3);
    freqReadoutRect = freqRow;

    body.removeFromTop(3);
    const int knobH =
        juce::jlimit(50, 64, static_cast<int>(body.getHeight() * 0.165f));
    auto knobRow = body.removeFromTop(knobH);
    layoutEqualColumns(knobRow, {&crsKnob, &fineKnob, &detKnob}, 2);

    body.removeFromTop(3);
    const int envH =
        juce::jlimit(18, 28, static_cast<int>(body.getHeight() * 0.085f));
    envGraphRect = body.removeFromTop(envH);

    body.removeFromTop(4);
    divider1Y = -1;
    divider2Y = -1;

    const int gapH = 5;
    const int availFaderH = juce::jmax(60, body.getHeight() - 2 * gapH);
    const int egRowH = static_cast<int>(std::lround(availFaderH * 0.355f));

    auto lvlRow = body.removeFromTop(egRowH);
    layoutEqualColumns(lvlRow,
                       {levelFaders[0].get(), levelFaders[1].get(),
                        levelFaders[2].get(), levelFaders[3].get()},
                       2);

    divider1Y = body.getY() + 2;
    body.removeFromTop(gapH);

    auto rateRow = body.removeFromTop(egRowH);
    layoutEqualColumns(rateRow,
                       {rateFaders[0].get(), rateFaders[1].get(),
                        rateFaders[2].get(), rateFaders[3].get()},
                       2);

    divider2Y = body.getY() + 2;
    body.removeFromTop(gapH);

    layoutEqualColumns(body, {&outFader, &velFader, &keyFader, &modFader}, 2);
  }

  void paint(juce::Graphics &g) override {
    const bool active = isOpActiveInMode();
    const juce::Colour roleCol =
        !active ? ThemeManager::get(Theme::Role::borderStrong)
                : ThemeManager::get(isCarrierRole ? Theme::Role::foh
                                                  : Theme::Role::iem);

    paintHardwareCardFrame(g, getLocalBounds(), {}, {}, roleCol);

    g.setColour(active ? ThemeManager::get(Theme::Role::text)
                       : ThemeManager::get(Theme::Role::textFaint));
    g.setFont(BoutiqueLookAndFeel::condensedFont(12.5f, 0.06f));
    g.drawFittedText("OP" + juce::String(dispOp), opTitleRect,
                     juce::Justification::centredLeft, 1);

    if (!roleBadgeRect.isEmpty()) {
      const auto rf = roleBadgeRect.toFloat();
      g.setColour(active ? roleCol.withAlpha(0.20f)
                         : ThemeManager::get(Theme::Role::sunken));
      g.fillRoundedRectangle(rf, 2.0f);
      g.setColour(active ? roleCol.withAlpha(0.85f)
                         : ThemeManager::get(Theme::Role::border));
      g.drawRoundedRectangle(rf, 2.0f, 0.8f);

      g.setColour(active ? roleCol : ThemeManager::get(Theme::Role::textFaint));
      g.setFont(BoutiqueLookAndFeel::condensedFont(9.2f, 0.04f));
      g.drawFittedText(active ? (isCarrierRole ? "CAR" : "MOD") : "4-OP",
                       roleBadgeRect, juce::Justification::centred, 1);
    }

    if (!freqReadoutRect.isEmpty()) {
      const auto &p = proc.engine().patch();
      const int off = patchByteOffset();
      const juce::String freqTxt =
          formatOpFrequency(p.d[off + 17], p.d[off + 18], p.d[off + 19]);

      const auto ff = freqReadoutRect.toFloat();
      g.setColour(ThemeManager::get(Theme::Role::sunken));
      g.fillRoundedRectangle(ff, 2.5f);
      g.setColour(ThemeManager::get(Theme::Role::border));
      g.drawRoundedRectangle(ff, 2.5f, 0.8f);

      g.setColour(active ? roleCol : ThemeManager::get(Theme::Role::textFaint));
      g.setFont(BoutiqueLookAndFeel::monoFont(10.5f, true));
      g.drawFittedText(freqTxt, freqReadoutRect.reduced(4, 0),
                       juce::Justification::centredLeft, 1, 0.80f);
    }

    paintEnvelopePreview(g, roleCol, active);

    const float xL = 4.0f;
    const float xR = static_cast<float>(getWidth()) - 4.0f;
    g.setColour(ThemeManager::get(Theme::Role::border));
    if (divider1Y > 0)
      g.drawHorizontalLine(divider1Y, xL, xR);
    if (divider2Y > 0)
      g.drawHorizontalLine(divider2Y, xL, xR);
  }

private:
  bool isOpActiveInMode() const noexcept {
    return dispOp <= proc.engine().numOps();
  }

  int engineOpIndex() const noexcept {
    const int n = proc.engine().numOps();
    return (dispOp <= n) ? (n - dispOp) : (6 - dispOp);
  }

  int patchByteOffset() const noexcept {
    const int n = proc.engine().numOps();
    if (dispOp <= n)
      return FmVoice::opOffset(n - dispOp, n);
    return (6 - dispOp) * 21;
  }

  static void layoutEqualColumns(juce::Rectangle<int> row,
                                 std::initializer_list<juce::Component *> items,
                                 int gap) {
    const int n = static_cast<int>(items.size());
    if (n <= 0 || row.isEmpty())
      return;
    const int availW = juce::jmax(n, row.getWidth() - (n - 1) * gap);
    int curX = row.getX();
    int idx = 0;
    for (auto *comp : items) {
      const int w = (idx == n - 1) ? (row.getRight() - curX) : (availW / n);
      comp->setBounds(curX, row.getY(), juce::jmax(10, w), row.getHeight());
      curX += w + gap;
      ++idx;
    }
  }

  void paintEnvelopePreview(juce::Graphics &g, juce::Colour strokeCol,
                            bool active) const {
    if (envGraphRect.isEmpty())
      return;
    const auto box = envGraphRect.toFloat();
    g.setColour(ThemeManager::get(Theme::Role::sunken));
    g.fillRoundedRectangle(box, 2.5f);
    g.setColour(ThemeManager::get(Theme::Role::border));
    g.drawRoundedRectangle(box, 2.5f, 0.8f);

    const auto &p = proc.engine().patch();
    const int off = patchByteOffset();
    const auto yForLevel = [&](int lvl) {
      const float norm = juce::jlimit(0.0f, 1.0f, static_cast<float>(lvl) / 99.0f);
      return box.getBottom() - 2.5f - norm * (box.getHeight() - 5.0f);
    };

    const float w1 = 0.10f + (1.0f - p.d[off + 0] / 99.0f) * 0.24f;
    const float w2 = 0.10f + (1.0f - p.d[off + 1] / 99.0f) * 0.22f;
    const float w3 = 0.10f + (1.0f - p.d[off + 2] / 99.0f) * 0.22f;
    const float wSus = 0.18f;
    const float w4 = 0.10f + (1.0f - p.d[off + 3] / 99.0f) * 0.24f;
    const float totalW = w1 + w2 + w3 + wSus + w4;

    const float x0 = box.getX() + 3.0f;
    const float spanX = juce::jmax(8.0f, box.getWidth() - 6.0f);
    const float x1 = x0 + spanX * (w1 / totalW);
    const float x2 = x1 + spanX * (w2 / totalW);
    const float x3 = x2 + spanX * (w3 / totalW);
    const float x4 = x3 + spanX * (wSus / totalW);
    const float x5 = x0 + spanX;

    const float yStart = yForLevel(p.d[off + 7]);
    const float y1 = yForLevel(p.d[off + 4]);
    const float y2 = yForLevel(p.d[off + 5]);
    const float y3 = yForLevel(p.d[off + 6]);

    juce::Path curve;
    curve.startNewSubPath(x0, yStart);
    curve.lineTo(x1, y1);
    curve.lineTo(x2, y2);
    curve.lineTo(x3, y3);
    curve.lineTo(x4, y3);
    curve.lineTo(x5, yStart);

    if (active) {
      juce::Path fill = curve;
      fill.lineTo(x5, box.getBottom() - 1.5f);
      fill.lineTo(x0, box.getBottom() - 1.5f);
      fill.closeSubPath();
      g.setColour(strokeCol.withAlpha(0.14f));
      g.fillPath(fill);
    }

    g.setColour(active ? strokeCol.withAlpha(0.90f)
                       : ThemeManager::get(Theme::Role::textFaint).withAlpha(0.45f));
    g.strokePath(curve, juce::PathStrokeType(1.3f, juce::PathStrokeType::curved,
                                             juce::PathStrokeType::rounded));
  }

  FmSynthProcessor &proc;
  int dispOp = 1;
  FmTelemetrySink &sink;
  std::function<void()> commitCallback;
  bool isCarrierRole = false;

  FmLedPillToggle onSwitch;
  FmLedPillToggle fixSwitch;
  FmOpWaveButton waveBtn;
  FmKnob crsKnob, fineKnob, detKnob;
  std::array<std::unique_ptr<FmFader>, 4> levelFaders;
  std::array<std::unique_ptr<FmFader>, 4> rateFaders;
  FmFader outFader, velFader, keyFader, modFader;

  juce::Rectangle<int> opTitleRect, roleBadgeRect, freqReadoutRect, envGraphRect;
  int divider1Y = -1;
  int divider2Y = -1;
};

} // namespace FanfareFm::Ui
