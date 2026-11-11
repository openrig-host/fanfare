#pragma once

#include <JuceHeader.h>
#include <cmath>
#include <functional>
#include "FmUiMetrics.h"

namespace FanfareFm::Ui {

class FmTelemetrySink {
public:
  virtual ~FmTelemetrySink() = default;
  virtual void handleTelemetryHover(const juce::String &text) = 0;
  virtual void handleTelemetryTouch(const juce::String &text, bool dragging,
                                    bool fineGear) = 0;
  virtual void handleTelemetryEdited(const juce::String &text) = 0;
};

class FmFader : public juce::Component, public juce::SettableTooltipClient {
public:
  FmFader(juce::String shortLabel, juce::String fullTitle, int maxValue,
          int defaultValue, bool isBipolar = false, int bipolarCenter = 0)
      : shortTitle(std::move(shortLabel)), fullName(std::move(fullTitle)),
        maxVal(juce::jmax(1, maxValue)),
        defVal(juce::jlimit(0, juce::jmax(1, maxValue), defaultValue)),
        bipolar(isBipolar), centerVal(bipolarCenter), currentVal(defVal) {
    setTooltip(fullName);
  }

  std::function<void(int)> onValueChanged;
  std::function<juce::String(int)> formatReadout;
  std::function<juce::String(int)> formatTelemetry;

  void setTelemetrySink(FmTelemetrySink *s) noexcept { sink = s; }

  void setAccentRole(Theme::Role activeRole, Theme::Role dimRole) {
    if (fillActiveRole == activeRole && fillDimRole == dimRole)
      return;
    fillActiveRole = activeRole;
    fillDimRole = dimRole;
    repaint();
  }

  void setValueSilent(int v) {
    const int clamped = juce::jlimit(0, maxVal, v);
    if (currentVal == clamped)
      return;
    currentVal = clamped;
    repaint();
  }

  int getValue() const noexcept { return currentVal; }
  bool isDraggingNow() const noexcept { return dragging; }

  void resized() override {
    auto b = getLocalBounds();
    labelArea = b.removeFromTop(Metrics::kControlLabelHeight);
    readoutArea = b.removeFromBottom(Metrics::kValueReadoutHeight).reduced(1, 0);
    trackBounds = b.reduced(1, 3);
  }

  void paint(juce::Graphics &g) override {
    g.setColour(ThemeManager::get(hovered || dragging ? Theme::Role::text
                                                      : Theme::Role::engrave));
    g.setFont(BoutiqueLookAndFeel::condensedFont(10.5f, 0.04f));
    g.drawFittedText(shortTitle, labelArea, juce::Justification::centred, 1,
                     0.78f);

    paintReadoutWell(g);

    if (trackBounds.getHeight() < 16)
      return;

    const float capH = 9.5f;
    const float capW = juce::jlimit(12.0f, 20.0f,
                                    static_cast<float>(trackBounds.getWidth()) * 0.62f);
    const float topY = static_cast<float>(trackBounds.getY()) + capH * 0.5f;
    const float botY = static_cast<float>(trackBounds.getBottom()) - capH * 0.5f;
    const float travel = juce::jmax(6.0f, botY - topY);
    const float centerX = static_cast<float>(trackBounds.getCentreX());

    if (trackBounds.getWidth() >= 20) {
      const juce::Colour majorCol =
          ThemeManager::get(Theme::Role::textFaint).withAlpha(0.68f);
      const juce::Colour minorCol =
          ThemeManager::get(Theme::Role::borderStrong).withAlpha(0.50f);
      const float tickInner = 3.8f;
      for (int step = 0; step <= 8; ++step) {
        const float t = static_cast<float>(step) / 8.0f;
        const float y = botY - t * travel;
        const bool major = (step == 0 || step == 4 || step == 8);
        const float len = major ? 4.2f : 2.4f;
        g.setColour(major ? majorCol : minorCol);
        g.drawHorizontalLine(static_cast<int>(std::lround(y)),
                             centerX - tickInner - len, centerX - tickInner);
        g.drawHorizontalLine(static_cast<int>(std::lround(y)),
                             centerX + tickInner, centerX + tickInner + len);
      }
    }

    const juce::Rectangle<float> groove(centerX - 1.8f, topY, 3.6f, travel);
    g.setColour(ThemeManager::get(Theme::Role::trackGroove));
    g.fillRoundedRectangle(groove, 1.8f);
    g.setColour(ThemeManager::get(Theme::Role::border));
    g.drawRoundedRectangle(groove, 1.8f, 0.7f);

    const float norm = static_cast<float>(currentVal) / static_cast<float>(maxVal);
    const float thumbY = botY - norm * travel;
    g.setColour(ThemeManager::get(dragging ? fillActiveRole : fillDimRole));
    if (bipolar) {
      const float centerNorm =
          static_cast<float>(centerVal) / static_cast<float>(maxVal);
      const float midY = botY - centerNorm * travel;
      const float fillTop = juce::jmin(midY, thumbY);
      const float fillH = std::abs(thumbY - midY);
      if (fillH > 0.5f)
        g.fillRoundedRectangle(
            juce::Rectangle<float>(centerX - 1.4f, fillTop, 2.8f, fillH), 1.4f);
    } else {
      const float fillH = botY - thumbY;
      if (fillH > 0.5f)
        g.fillRoundedRectangle(
            juce::Rectangle<float>(centerX - 1.4f, thumbY, 2.8f, fillH), 1.4f);
    }

    const juce::Rectangle<float> cap(centerX - capW * 0.5f, thumbY - capH * 0.5f,
                                     capW, capH);
    g.setColour(juce::Colours::black.withAlpha(0.32f));
    g.fillRoundedRectangle(cap.translated(0.0f, 1.0f), 1.8f);

    g.setColour(ThemeManager::get(Theme::Role::raised));
    g.fillRoundedRectangle(cap, 1.8f);
    g.setColour(ThemeManager::get(dragging || hovered ? fillActiveRole
                                                      : Theme::Role::borderStrong));
    g.drawRoundedRectangle(cap, 1.8f, 0.9f);

    g.setColour(ThemeManager::get(fineDrag ? fillActiveRole
                                           : Theme::Role::knobThumb));
    g.fillRect(juce::Rectangle<float>(cap.getX() + 2.0f, thumbY - 0.75f,
                                      cap.getWidth() - 4.0f, 1.5f));
  }

  void mouseEnter(const juce::MouseEvent &) override {
    hovered = true;
    if (sink != nullptr)
      sink->handleTelemetryHover(buildTelemetryString());
    repaint();
  }

  void mouseExit(const juce::MouseEvent &) override {
    hovered = false;
    repaint();
  }

  void mouseDown(const juce::MouseEvent &e) override {
    dragging = true;
    fineDrag = isFineModifier(e.mods);
    anchorMouseY = e.position.y;
    anchorNorm = static_cast<float>(currentVal) / static_cast<float>(maxVal);
    if (sink != nullptr)
      sink->handleTelemetryTouch(buildTelemetryString(), true, fineDrag);
    repaint();
  }

  void mouseDrag(const juce::MouseEvent &e) override {
    if (!dragging)
      return;
    const bool nowFine = isFineModifier(e.mods);
    if (nowFine != fineDrag) {
      anchorMouseY = e.position.y;
      anchorNorm = static_cast<float>(currentVal) / static_cast<float>(maxVal);
      fineDrag = nowFine;
      if (sink != nullptr)
        sink->handleTelemetryTouch(buildTelemetryString(), true, fineDrag);
    }

    const float baseSpan = juce::jmax(
        static_cast<float>(Metrics::kFaderMinVirtualSpanPx),
        static_cast<float>(trackBounds.getHeight() - 10));
    const float span =
        fineDrag ? (baseSpan * Metrics::kFineTuneGearDivisor) : baseSpan;
    float candidate =
        juce::jlimit(0.0f, 1.0f, anchorNorm + (anchorMouseY - e.position.y) / span);

    if (bipolar && !fineDrag) {
      const float cNorm =
          static_cast<float>(centerVal) / static_cast<float>(maxVal);
      if (std::abs(candidate - cNorm) < Metrics::kBipolarDetentRadius)
        candidate = cNorm;
    }

    commitFromUi(static_cast<int>(std::lround(candidate * static_cast<float>(maxVal))));
  }

  void mouseUp(const juce::MouseEvent &) override {
    dragging = false;
    fineDrag = false;
    if (sink != nullptr)
      sink->handleTelemetryTouch(buildTelemetryString(), false, false);
    repaint();
  }

  void mouseDoubleClick(const juce::MouseEvent &e) override {
    if (e.mods.isPopupMenu())
      return;
    commitFromUi(defVal);
  }

  void mouseWheelMove(const juce::MouseEvent &e,
                      const juce::MouseWheelDetails &wheel) override {
    if (std::abs(wheel.deltaY) <= 1e-5f)
      return;
    int step = (wheel.deltaY > 0.0f) ? 1 : -1;
    if (e.mods.isShiftDown() && maxVal >= 20)
      step *= 5;
    commitFromUi(currentVal + step);
  }

private:
  static bool isFineModifier(const juce::ModifierKeys &m) noexcept {
    return m.isShiftDown() || m.isCommandDown() || m.isCtrlDown() ||
           m.isRightButtonDown();
  }

  void paintReadoutWell(juce::Graphics &g) const {
    if (readoutArea.isEmpty())
      return;
    const auto rf = readoutArea.toFloat();
    g.setColour(ThemeManager::get(Theme::Role::sunken));
    g.fillRoundedRectangle(rf, 2.0f);
    g.setColour(ThemeManager::get(dragging ? fillActiveRole : Theme::Role::border));
    g.drawRoundedRectangle(rf, 2.0f, 0.7f);

    const juce::String txt =
        formatReadout ? formatReadout(currentVal) : juce::String(currentVal);
    g.setColour(ThemeManager::get(hovered || dragging ? Theme::Role::text
                                                      : Theme::Role::textDim));
    g.setFont(BoutiqueLookAndFeel::monoFont(9.5f, false));
    g.drawFittedText(txt, readoutArea.reduced(1, 0), juce::Justification::centred,
                     1, 0.75f);
  }

  juce::String buildTelemetryString() const {
    if (formatTelemetry)
      return formatTelemetry(currentVal);
    const juce::String valStr =
        formatReadout ? formatReadout(currentVal) : juce::String(currentVal);
    return fullName + ": " + valStr;
  }

  void commitFromUi(int nextVal) {
    const int clamped = juce::jlimit(0, maxVal, nextVal);
    if (clamped == currentVal)
      return;
    currentVal = clamped;
    if (onValueChanged)
      onValueChanged(currentVal);
    if (sink != nullptr)
      sink->handleTelemetryEdited(buildTelemetryString());
    repaint();
  }

  juce::String shortTitle;
  juce::String fullName;
  int maxVal = 99;
  int defVal = 0;
  bool bipolar = false;
  int centerVal = 0;
  int currentVal = 0;

  Theme::Role fillActiveRole = Theme::Role::accent;
  Theme::Role fillDimRole = Theme::Role::accentDim;
  FmTelemetrySink *sink = nullptr;

  juce::Rectangle<int> labelArea, trackBounds, readoutArea;
  float anchorMouseY = 0.0f;
  float anchorNorm = 0.0f;
  bool hovered = false;
  bool dragging = false;
  bool fineDrag = false;
};

class FmKnob : public juce::Component, public juce::SettableTooltipClient {
public:
  FmKnob(juce::String shortLabel, juce::String fullTitle, float defaultNorm = 0.0f,
         bool isBipolar = false, int discreteStepsMax = 0)
      : shortTitle(std::move(shortLabel)), fullName(std::move(fullTitle)),
        defNorm(juce::jlimit(0.0f, 1.0f, defaultNorm)), bipolar(isBipolar),
        maxDiscrete(discreteStepsMax), normValue(defNorm) {
    setTooltip(fullName);
  }

  std::function<void(float)> onNormChanged;
  std::function<juce::String(float)> formatReadout;
  std::function<juce::String(float)> formatTelemetry;

  void setTelemetrySink(FmTelemetrySink *s) noexcept { sink = s; }

  void setNormSilent(float n) {
    const float clamped = snapNorm(n);
    if (std::abs(clamped - normValue) < 1e-5f)
      return;
    normValue = clamped;
    repaint();
  }

  float getNorm() const noexcept { return normValue; }
  int getDiscreteValue() const noexcept {
    return maxDiscrete > 0
               ? static_cast<int>(std::lround(normValue * static_cast<float>(maxDiscrete)))
               : 0;
  }
  bool isDraggingNow() const noexcept { return dragging; }

  void resized() override {
    auto b = getLocalBounds();
    labelArea = b.removeFromTop(Metrics::kControlLabelHeight);
    readoutArea = b.removeFromBottom(Metrics::kValueReadoutHeight).reduced(1, 0);
    const int dialSize =
        juce::jlimit(18, 42, juce::jmin(b.getWidth() - 2, b.getHeight() - 2));
    dialArea = b.withSizeKeepingCentre(dialSize, dialSize);
  }

  void paint(juce::Graphics &g) override {
    g.setColour(ThemeManager::get(hovered || dragging ? Theme::Role::text
                                                      : Theme::Role::engrave));
    g.setFont(BoutiqueLookAndFeel::condensedFont(10.5f, 0.04f));
    g.drawFittedText(shortTitle, labelArea, juce::Justification::centred, 1,
                     0.78f);

    if (!readoutArea.isEmpty()) {
      const auto rf = readoutArea.toFloat();
      g.setColour(ThemeManager::get(Theme::Role::sunken));
      g.fillRoundedRectangle(rf, 2.0f);
      g.setColour(ThemeManager::get(dragging ? Theme::Role::accent
                                             : Theme::Role::border));
      g.drawRoundedRectangle(rf, 2.0f, 0.7f);

      const juce::String txt = formatReadout
                                   ? formatReadout(normValue)
                                   : (maxDiscrete > 0
                                          ? juce::String(getDiscreteValue())
                                          : juce::String(static_cast<int>(std::lround(normValue * 100.0f))) + "%");
      g.setColour(ThemeManager::get(hovered || dragging ? Theme::Role::text
                                                        : Theme::Role::textDim));
      g.setFont(BoutiqueLookAndFeel::monoFont(9.5f, false));
      g.drawFittedText(txt, readoutArea.reduced(1, 0),
                       juce::Justification::centred, 1, 0.75f);
    }

    if (dialArea.isEmpty())
      return;

    const auto df = dialArea.toFloat();
    const float cx = df.getCentreX();
    const float cy = df.getCentreY();
    const float outerR = df.getWidth() * 0.5f - 1.2f;
    const float bodyR = juce::jmax(5.0f, outerR - 2.8f);

    constexpr float kStartAngle = -2.35619449f;
    constexpr float kEndAngle = 2.35619449f;
    const float valAngle = kStartAngle + normValue * (kEndAngle - kStartAngle);

    juce::Path bgArc;
    bgArc.addCentredArc(cx, cy, outerR, outerR, 0.0f, kStartAngle, kEndAngle,
                        true);
    g.setColour(ThemeManager::get(Theme::Role::trackGroove));
    g.strokePath(bgArc, juce::PathStrokeType(2.0f, juce::PathStrokeType::curved,
                                             juce::PathStrokeType::rounded));

    const float originAngle = bipolar ? 0.0f : kStartAngle;
    if (std::abs(valAngle - originAngle) > 0.02f) {
      juce::Path valArc;
      valArc.addCentredArc(cx, cy, outerR, outerR, 0.0f,
                           juce::jmin(originAngle, valAngle),
                           juce::jmax(originAngle, valAngle), true);
      g.setColour(ThemeManager::get(dragging ? Theme::Role::accent
                                             : Theme::Role::accentDim));
      g.strokePath(valArc,
                   juce::PathStrokeType(2.1f, juce::PathStrokeType::curved,
                                        juce::PathStrokeType::rounded));
    }

    const juce::Rectangle<float> body(cx - bodyR, cy - bodyR, bodyR * 2.0f,
                                      bodyR * 2.0f);
    g.setColour(ThemeManager::get(Theme::Role::knobFace));
    g.fillEllipse(body);
    g.setColour(ThemeManager::get(dragging || hovered ? Theme::Role::accent
                                                      : Theme::Role::borderStrong));
    g.drawEllipse(body, 1.0f);

    const float sinA = std::sin(valAngle);
    const float cosA = -std::cos(valAngle);
    g.setColour(ThemeManager::get(fineDrag ? Theme::Role::accent
                                           : Theme::Role::knobThumb));
    g.drawLine(cx + sinA * (bodyR * 0.25f), cy + cosA * (bodyR * 0.25f),
               cx + sinA * (bodyR - 1.2f), cy + cosA * (bodyR - 1.2f), 1.7f);
  }

  void mouseEnter(const juce::MouseEvent &) override {
    hovered = true;
    if (sink != nullptr)
      sink->handleTelemetryHover(buildTelemetryString());
    repaint();
  }

  void mouseExit(const juce::MouseEvent &) override {
    hovered = false;
    repaint();
  }

  void mouseDown(const juce::MouseEvent &e) override {
    dragging = true;
    fineDrag = isFineModifier(e.mods);
    anchorMouseY = e.position.y;
    anchorMouseX = e.position.x;
    anchorNorm = normValue;
    if (sink != nullptr)
      sink->handleTelemetryTouch(buildTelemetryString(), true, fineDrag);
    repaint();
  }

  void mouseDrag(const juce::MouseEvent &e) override {
    if (!dragging)
      return;
    const bool nowFine = isFineModifier(e.mods);
    if (nowFine != fineDrag) {
      anchorMouseY = e.position.y;
      anchorMouseX = e.position.x;
      anchorNorm = normValue;
      fineDrag = nowFine;
      if (sink != nullptr)
        sink->handleTelemetryTouch(buildTelemetryString(), true, fineDrag);
    }

    const float span = fineDrag
                           ? (Metrics::kKnobVirtualSpanPx * Metrics::kFineTuneGearDivisor)
                           : Metrics::kKnobVirtualSpanPx;
    const float deltaPx =
        (anchorMouseY - e.position.y) + 0.35f * (e.position.x - anchorMouseX);
    float candidate = juce::jlimit(0.0f, 1.0f, anchorNorm + deltaPx / span);
    if (bipolar && !fineDrag &&
        std::abs(candidate - 0.5f) < Metrics::kBipolarDetentRadius) {
      candidate = 0.5f;
    }
    commitFromUi(candidate);
  }

  void mouseUp(const juce::MouseEvent &) override {
    dragging = false;
    fineDrag = false;
    if (sink != nullptr)
      sink->handleTelemetryTouch(buildTelemetryString(), false, false);
    repaint();
  }

  void mouseDoubleClick(const juce::MouseEvent &e) override {
    if (e.mods.isPopupMenu())
      return;
    commitFromUi(defNorm);
  }

  void mouseWheelMove(const juce::MouseEvent &e,
                      const juce::MouseWheelDetails &wheel) override {
    if (std::abs(wheel.deltaY) <= 1e-5f)
      return;
    const float dir = (wheel.deltaY > 0.0f) ? 1.0f : -1.0f;
    if (maxDiscrete > 0) {
      const int next = juce::jlimit(0, maxDiscrete, getDiscreteValue() + static_cast<int>(dir));
      commitFromUi(static_cast<float>(next) / static_cast<float>(maxDiscrete));
      return;
    }
    const float step = (e.mods.isShiftDown() ? 0.005f : 0.025f) * dir;
    commitFromUi(normValue + step);
  }

private:
  static bool isFineModifier(const juce::ModifierKeys &m) noexcept {
    return m.isShiftDown() || m.isCommandDown() || m.isCtrlDown() ||
           m.isRightButtonDown();
  }

  float snapNorm(float n) const noexcept {
    const float clamped = juce::jlimit(0.0f, 1.0f, n);
    if (maxDiscrete > 0) {
      const float v = std::round(clamped * static_cast<float>(maxDiscrete));
      return v / static_cast<float>(maxDiscrete);
    }
    return clamped;
  }

  juce::String buildTelemetryString() const {
    if (formatTelemetry)
      return formatTelemetry(normValue);
    const juce::String valStr =
        formatReadout
            ? formatReadout(normValue)
            : (maxDiscrete > 0
                   ? juce::String(getDiscreteValue())
                   : juce::String(static_cast<int>(std::lround(normValue * 100.0f))) + "%");
    return fullName + ": " + valStr;
  }

  void commitFromUi(float nextNorm) {
    const float snapped = snapNorm(nextNorm);
    if (std::abs(snapped - normValue) < 1e-5f)
      return;
    normValue = snapped;
    if (onNormChanged)
      onNormChanged(normValue);
    if (sink != nullptr)
      sink->handleTelemetryEdited(buildTelemetryString());
    repaint();
  }

  juce::String shortTitle;
  juce::String fullName;
  float defNorm = 0.0f;
  bool bipolar = false;
  int maxDiscrete = 0;
  float normValue = 0.0f;

  FmTelemetrySink *sink = nullptr;
  juce::Rectangle<int> labelArea, dialArea, readoutArea;
  float anchorMouseY = 0.0f;
  float anchorMouseX = 0.0f;
  float anchorNorm = 0.0f;
  bool hovered = false;
  bool dragging = false;
  bool fineDrag = false;
};

class FmSegmentedSwitch : public juce::Component,
                          public juce::SettableTooltipClient {
public:
  FmSegmentedSwitch(juce::StringArray segmentLabels, juce::String tooltipText = {})
      : labels(std::move(segmentLabels)) {
    setMouseCursor(juce::MouseCursor::PointingHandCursor);
    if (tooltipText.isNotEmpty())
      setTooltip(tooltipText);
  }

  std::function<void(int)> onSelectionChanged;

  void setSelectedIndexSilent(int idx) {
    const int clamped =
        juce::jlimit(0, juce::jmax(0, labels.size() - 1), idx);
    if (selectedIdx == clamped)
      return;
    selectedIdx = clamped;
    repaint();
  }

  int getSelectedIndex() const noexcept { return selectedIdx; }

  void paint(juce::Graphics &g) override {
    const int n = labels.size();
    if (n <= 0)
      return;

    const auto outer = getLocalBounds().toFloat().reduced(0.5f);
    g.setColour(ThemeManager::get(Theme::Role::sunken));
    g.fillRoundedRectangle(outer, 2.5f);
    g.setColour(ThemeManager::get(Theme::Role::border));
    g.drawRoundedRectangle(outer, 2.5f, 1.0f);

    const float segW = outer.getWidth() / static_cast<float>(n);
    for (int i = 0; i < n; ++i) {
      const auto seg =
          juce::Rectangle<float>(outer.getX() + static_cast<float>(i) * segW,
                                 outer.getY(), segW, outer.getHeight())
              .reduced(1.2f, 1.2f);
      const bool on = (i == selectedIdx);
      const bool hov = (hoveredIdx == i);

      if (on) {
        g.setColour(ThemeManager::get(Theme::Role::raised));
        g.fillRoundedRectangle(seg, 2.0f);
        g.setColour(ThemeManager::get(Theme::Role::accent).withAlpha(0.80f));
        g.drawRoundedRectangle(seg, 2.0f, 1.0f);
      } else if (hov) {
        g.setColour(ThemeManager::get(Theme::Role::panelAlt));
        g.fillRoundedRectangle(seg, 2.0f);
      }

      const float ledW = juce::jlimit(6.0f, 16.0f, seg.getWidth() * 0.44f);
      const juce::Rectangle<float> led(seg.getCentreX() - ledW * 0.5f,
                                       seg.getY() + 1.8f, ledW, 2.0f);
      g.setColour(ThemeManager::get(on ? Theme::Role::accent
                                       : Theme::Role::bypassed));
      g.fillRoundedRectangle(led, 1.0f);

      const auto textRect = seg.withTrimmedTop(3.8f).toNearestInt();
      g.setColour(ThemeManager::get(on ? Theme::Role::text
                                       : Theme::Role::textDim));
      g.setFont(BoutiqueLookAndFeel::condensedFont(10.5f, 0.03f));
      g.drawFittedText(labels[i], textRect, juce::Justification::centred, 1,
                       0.72f);
    }
  }

  void mouseMove(const juce::MouseEvent &e) override { updateHover(e.position); }
  void mouseEnter(const juce::MouseEvent &e) override { updateHover(e.position); }
  void mouseExit(const juce::MouseEvent &) override {
    hoveredIdx = -1;
    repaint();
  }

  void mouseDown(const juce::MouseEvent &e) override {
    const int n = labels.size();
    if (n <= 0)
      return;
    const int clicked = juce::jlimit(
        0, n - 1,
        static_cast<int>(e.position.x / (static_cast<float>(getWidth()) / static_cast<float>(n))));
    if (clicked != selectedIdx) {
      selectedIdx = clicked;
      if (onSelectionChanged)
        onSelectionChanged(selectedIdx);
      repaint();
    }
  }

private:
  void updateHover(juce::Point<float> p) {
    const int n = labels.size();
    const int next =
        (n > 0 && getLocalBounds().toFloat().contains(p))
            ? juce::jlimit(0, n - 1,
                           static_cast<int>(p.x / (static_cast<float>(getWidth()) / static_cast<float>(n))))
            : -1;
    if (next != hoveredIdx) {
      hoveredIdx = next;
      repaint();
    }
  }

  juce::StringArray labels;
  int selectedIdx = 0;
  int hoveredIdx = -1;
};

class FmLedPillToggle : public juce::Component,
                        public juce::SettableTooltipClient {
public:
  FmLedPillToggle(juce::String onLabel, juce::String offLabel,
                  Theme::Role activeLedRole = Theme::Role::accent,
                  juce::String tooltipText = {})
      : textOn(std::move(onLabel)), textOff(std::move(offLabel)),
        ledRole(activeLedRole) {
    setMouseCursor(juce::MouseCursor::PointingHandCursor);
    if (tooltipText.isNotEmpty())
      setTooltip(tooltipText);
  }

  std::function<void(bool)> onToggled;

  void setStateSilent(bool state) {
    if (isOn == state)
      return;
    isOn = state;
    repaint();
  }

  bool getState() const noexcept { return isOn; }

  void paint(juce::Graphics &g) override {
    const auto b = getLocalBounds().toFloat().reduced(0.5f);
    g.setColour(ThemeManager::get(isOn ? Theme::Role::raised
                                       : Theme::Role::sunken));
    g.fillRoundedRectangle(b, 2.5f);

    const juce::Colour activeCol = ThemeManager::get(ledRole);
    g.setColour(isOn ? activeCol.withAlpha(0.82f)
                     : ThemeManager::get(hovered ? Theme::Role::borderStrong
                                                 : Theme::Role::border));
    g.drawRoundedRectangle(b, 2.5f, 1.0f);

    const float ledW = juce::jlimit(6.0f, 14.0f, b.getWidth() * 0.42f);
    g.setColour(isOn ? activeCol : ThemeManager::get(Theme::Role::bypassed));
    g.fillRoundedRectangle(
        juce::Rectangle<float>(b.getCentreX() - ledW * 0.5f, b.getY() + 1.6f,
                               ledW, 1.9f),
        1.0f);

    g.setColour(ThemeManager::get(isOn ? Theme::Role::text
                                       : Theme::Role::textDim));
    g.setFont(BoutiqueLookAndFeel::condensedFont(10.0f, 0.03f));
    g.drawFittedText(isOn ? textOn : textOff,
                     b.withTrimmedTop(3.2f).toNearestInt(),
                     juce::Justification::centred, 1, 0.72f);
  }

  void mouseEnter(const juce::MouseEvent &) override {
    hovered = true;
    repaint();
  }
  void mouseExit(const juce::MouseEvent &) override {
    hovered = false;
    repaint();
  }
  void mouseDown(const juce::MouseEvent &) override {
    isOn = !isOn;
    if (onToggled)
      onToggled(isOn);
    repaint();
  }

private:
  juce::String textOn, textOff;
  Theme::Role ledRole = Theme::Role::accent;
  bool isOn = false;
  bool hovered = false;
};

} // namespace FanfareFm::Ui
