#pragma once

#include <JuceHeader.h>
#include <functional>
#include "FmUiMetrics.h"

namespace FanfareFm::Ui {

class FmModeBar : public juce::Component, public juce::TooltipClient {
public:
  FmModeBar() { setMouseCursor(juce::MouseCursor::PointingHandCursor); }

  std::function<void(int)> onModeSelected;

  void setSelectedMode(int modeIdx) {
    const int clamped = juce::jlimit(0, 4, modeIdx);
    if (selectedMode == clamped)
      return;
    selectedMode = clamped;
    repaint();
  }

  int getSelectedMode() const noexcept { return selectedMode; }

  juce::String getTooltip() override {
    const auto &specs = modeSpecs();
    if (hoveredCard >= 0 && hoveredCard < static_cast<int>(specs.size()))
      return specs[(size_t)hoveredCard].tooltip;
    return {};
  }

  void resized() override {
    const auto b = getLocalBounds();
    const int gap = Metrics::kSectionGap;
    const int availW = juce::jmax(50, b.getWidth() - 4 * gap);
    int curX = b.getX();
    for (int i = 0; i < 5; ++i) {
      const int w = (i == 4) ? (b.getRight() - curX) : (availW / 5);
      cardRects[(size_t)i] = juce::Rectangle<int>(curX, b.getY(), w, b.getHeight());
      curX += w + gap;
    }
  }

  void paint(juce::Graphics &g) override {
    const auto &specs = modeSpecs();
    for (int i = 0; i < 5; ++i) {
      const auto r = cardRects[(size_t)i].toFloat().reduced(0.5f);
      if (r.isEmpty())
        continue;

      const bool sel = (i == selectedMode);
      const bool hov = (i == hoveredCard);
      const auto &sp = specs[(size_t)i];

      g.setColour(ThemeManager::get(
          sel ? Theme::Role::raised
              : (hov ? Theme::Role::panelAlt : Theme::Role::panel)));
      g.fillRoundedRectangle(r, 3.0f);

      g.setColour(sel ? ThemeManager::get(Theme::Role::accent)
                      : ThemeManager::get(hov ? Theme::Role::borderStrong
                                              : Theme::Role::border));
      g.drawRoundedRectangle(r, 3.0f, sel ? 1.3f : 1.0f);

      const auto ledStrip =
          juce::Rectangle<float>(r.getX() + 5.0f, r.getY() + 2.0f, 18.0f, 2.2f);
      g.setColour(ThemeManager::get(sel ? Theme::Role::accent
                                        : Theme::Role::bypassed));
      g.fillRoundedRectangle(ledStrip, 1.0f);

      auto inner = cardRects[(size_t)i].reduced(7, 2);
      inner.removeFromTop(2);

      auto rightSide = inner.removeFromRight(juce::jmin(108, inner.getWidth() / 2 + 12));
      auto badgeRect = rightSide.removeFromRight(42).reduced(0, 3);

      g.setColour(sel ? ThemeManager::get(Theme::Role::accent).withAlpha(0.20f)
                      : ThemeManager::get(Theme::Role::sunken));
      g.fillRoundedRectangle(badgeRect.toFloat(), 2.0f);
      g.setColour(sel ? ThemeManager::get(Theme::Role::accent)
                      : ThemeManager::get(Theme::Role::border));
      g.drawRoundedRectangle(badgeRect.toFloat(), 2.0f, 0.8f);

      g.setColour(sel ? ThemeManager::get(Theme::Role::accent)
                      : ThemeManager::get(Theme::Role::textDim));
      g.setFont(BoutiqueLookAndFeel::condensedFont(9.5f, 0.04f));
      g.drawFittedText(sp.archBadge, badgeRect, juce::Justification::centred, 1);

      rightSide.removeFromRight(5);
      if (rightSide.getWidth() >= 34) {
        g.setColour(ThemeManager::get(sel ? Theme::Role::textDim
                                          : Theme::Role::textFaint));
        g.setFont(BoutiqueLookAndFeel::monoFont(9.2f, false));
        g.drawFittedText(sp.specSummary, rightSide,
                         juce::Justification::centredRight, 1, 0.80f);
      }

      g.setColour(ThemeManager::get(sel ? Theme::Role::text
                                        : Theme::Role::textDim));
      g.setFont(BoutiqueLookAndFeel::uiFont(12.0f, true));
      g.drawFittedText(sp.title, inner, juce::Justification::centredLeft, 1,
                       0.85f);
    }
  }

  void mouseMove(const juce::MouseEvent &e) override { updateHover(e.getPosition()); }
  void mouseEnter(const juce::MouseEvent &e) override { updateHover(e.getPosition()); }
  void mouseExit(const juce::MouseEvent &) override {
    hoveredCard = -1;
    repaint();
  }

  void mouseDown(const juce::MouseEvent &e) override {
    const int idx = hitCard(e.getPosition());
    if (idx >= 0 && idx != selectedMode) {
      selectedMode = idx;
      if (onModeSelected)
        onModeSelected(selectedMode);
      repaint();
    }
  }

private:
  int hitCard(juce::Point<int> pt) const noexcept {
    for (int i = 0; i < 5; ++i)
      if (cardRects[(size_t)i].contains(pt))
        return i;
    return -1;
  }

  void updateHover(juce::Point<int> pt) {
    const int next = hitCard(pt);
    if (next != hoveredCard) {
      hoveredCard = next;
      repaint();
    }
  }

  std::array<juce::Rectangle<int>, 5> cardRects{};
  int selectedMode = 0;
  int hoveredCard = -1;
};

} // namespace FanfareFm::Ui
