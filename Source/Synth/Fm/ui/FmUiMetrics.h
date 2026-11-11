#pragma once

#include <JuceHeader.h>
#include <array>
#include <cmath>
#include "../../../BoutiqueLookAndFeel.h"
#include "../../../ThemeManager.h"
#include "../FmEngine.h"

namespace FanfareFm::Ui {

namespace Metrics {
inline constexpr int kDefaultOverlayWidth = 1220;
inline constexpr int kDefaultOverlayHeight = 720;
inline constexpr int kMinOverlayWidth = 980;
inline constexpr int kMinOverlayHeight = 600;
inline constexpr int kMaxOverlayWidth = 1580;
inline constexpr int kMaxOverlayHeight = 960;
inline constexpr int kOverlayMarginFromHost = 32;

inline constexpr int kOuterPadding = 8;
inline constexpr int kHeaderHeight = 40;
inline constexpr int kModeBarHeight = 36;
inline constexpr int kSectionGap = 5;
inline constexpr int kSectionHeaderHeight = 19;
inline constexpr int kKeyboardStripHeight = 100;
inline constexpr int kWheelStripWidth = 34;

inline constexpr int kRightDeckMinWidth = 244;
inline constexpr int kRightDeckMaxWidth = 296;
inline constexpr float kRightDeckWidthRatio = 0.242f;

inline constexpr int kControlLabelHeight = 12;
inline constexpr int kValueReadoutHeight = 14;
inline constexpr int kFaderMinVirtualSpanPx = 150;
inline constexpr float kKnobVirtualSpanPx = 165.0f;
inline constexpr float kFineTuneGearDivisor = 10.0f;
inline constexpr float kBipolarDetentRadius = 0.025f;
inline constexpr float kDisabledAlpha = 0.34f;
inline constexpr int kTelemetryTimerHz = 24;
} // namespace Metrics

struct ModeSpec {
  FmMode mode;
  const char *title;
  const char *archBadge;
  const char *specSummary;
  const char *tooltip;
};

inline const std::array<ModeSpec, 5> &modeSpecs() {
  static const std::array<ModeSpec, 5> kSpecs = {{
      {FmMode::Dx7, "DX7", "6-OP", "32 ALG  |  16V",
       "Yamaha DX7 architecture: 6 sine operators, 32 algorithms, 16-voice polyphony"},
      {FmMode::Dx21, "DX21 / 100", "4-OP", "8 ALG  |  16V",
       "Yamaha DX21 / DX27 / DX100 architecture: 4 sine operators, 8 algorithms, 16 voices"},
      {FmMode::Dual4, "DUAL 2x4", "LAYER", "8 ALG  |  8V",
       "Dual detuned 4-operator stack (DX21/DX100 bi-timbral unison layer), 8-note polyphony"},
      {FmMode::Dual6, "DUAL 2x6", "LAYER", "32 ALG  |  8V",
       "DX7II-style dual detuned 6-operator unison layer, 32 algorithms, 8-note polyphony"},
      {FmMode::Tx81z, "TX81Z", "8-WAVE", "13 ALG  |  16V",
       "Yamaha TX81Z architecture: 4 operators, 13 algorithms, 8 selectable operator waveforms (W1-W8)"},
  }};
  return kSpecs;
}

inline juce::String formatOpFrequency(int mode, int coarse, int fine) {
  const int c = juce::jlimit(0, 31, coarse);
  const int f = juce::jlimit(0, 99, fine);
  if (mode == 0) {
    const double base = (c == 0) ? 0.5 : static_cast<double>(c);
    const double ratio = base * (1.0 + 0.01 * static_cast<double>(f));
    if (c == 0 && (f & 1) != 0)
      return juce::String(ratio, 3) + " x";
    return juce::String(ratio, 2) + " x";
  }

  const double exp10Arg =
      static_cast<double>((c & 3) * 100 + f) / 100.0;
  const double hz = std::pow(10.0, exp10Arg);
  if (hz >= 1000.0)
    return juce::String(hz / 1000.0, 2) + " kHz";
  if (hz >= 100.0)
    return juce::String(hz, 1) + " Hz";
  return juce::String(hz, 2) + " Hz";
}

inline juce::String formatDetune(int raw0to14) {
  const int d = juce::jlimit(0, 14, raw0to14) - 7;
  return (d > 0 ? "+" : "") + juce::String(d);
}

inline juce::String formatTranspose(int raw0to48) {
  const int st = juce::jlimit(0, 48, raw0to48) - 24;
  return (st > 0 ? "+" : "") + juce::String(st) + " st";
}

inline const char *txWaveName(int w) {
  static constexpr const char *kNames[8] = {
      "W1 Sine",        "W2ResSine",      "W3 HalfSine",   "W4 HalfRes",
      "W5 2xSine",      "W6 2xResSine",   "W7 Abs2xSine",  "W8 Abs2xRes"};
  return kNames[w & 7];
}

inline float sampleTxWavePreview(int wave, float phase01) noexcept {
  const double x = 2.0 * kFmPi * static_cast<double>(phase01);
  const double s = std::sin(x);
  const double s2 = std::sin(2.0 * x);
  const double sign = s < 0.0 ? -1.0 : 1.0;
  const double sign2 = s2 < 0.0 ? -1.0 : 1.0;
  switch (wave & 7) {
  case 0: return static_cast<float>(s);
  case 1: return static_cast<float>(s * s * s * sign);
  case 2: return static_cast<float>(s < 0.0 ? 0.0 : s);
  case 3: {
    const double v = s * s * s * sign;
    return static_cast<float>(v < 0.0 ? 0.0 : v);
  }
  case 4: return static_cast<float>(s2);
  case 5: return static_cast<float>(s2 * s2 * s2 * sign2);
  case 6: return static_cast<float>(std::fabs(s2));
  case 7: return static_cast<float>(std::fabs(s2 * s2 * s2 * sign2));
  default: return static_cast<float>(s);
  }
}

inline void paintHardwareCardFrame(juce::Graphics &g, juce::Rectangle<int> bounds,
                                   const juce::String &title,
                                   const juce::String &rightBadge = {},
                                   juce::Colour stripeColour = {},
                                   juce::Colour badgeColour = {}) {
  const auto b = bounds.toFloat();
  g.setColour(ThemeManager::get(Theme::Role::panel));
  g.fillRoundedRectangle(b, 3.0f);

  const auto headerRect = juce::Rectangle<float>(
      b.getX(), b.getY(), b.getWidth(), static_cast<float>(Metrics::kSectionHeaderHeight));
  g.setColour(ThemeManager::get(Theme::Role::panelAlt));
  g.fillRoundedRectangle(headerRect, 3.0f);

  const juce::Colour topStripe =
      stripeColour.isTransparent() ? ThemeManager::get(Theme::Role::accentDim)
                                   : stripeColour;
  g.setColour(topStripe);
  g.fillRect(juce::Rectangle<float>(b.getX() + 1.0f, b.getY() + 0.5f,
                                    b.getWidth() - 2.0f, 1.6f));

  g.setColour(ThemeManager::get(Theme::Role::border));
  g.drawRoundedRectangle(b.reduced(0.5f), 3.0f, 1.0f);
  g.drawHorizontalLine(bounds.getY() + Metrics::kSectionHeaderHeight,
                       b.getX() + 1.0f, b.getRight() - 1.0f);

  auto titleArea =
      bounds.removeFromTop(Metrics::kSectionHeaderHeight).reduced(6, 0);
  if (rightBadge.isNotEmpty()) {
    auto badgeArea = titleArea.removeFromRight(68);
    g.setColour(badgeColour.isTransparent()
                    ? ThemeManager::get(Theme::Role::accent)
                    : badgeColour);
    g.setFont(BoutiqueLookAndFeel::condensedFont(10.0f, 0.04f));
    g.drawFittedText(rightBadge, badgeArea, juce::Justification::centredRight, 1);
  }

  if (title.isNotEmpty()) {
    g.setColour(ThemeManager::get(Theme::Role::text));
    g.setFont(BoutiqueLookAndFeel::condensedFont(12.0f, 0.06f));
    g.drawFittedText(title, titleArea, juce::Justification::centredLeft, 1);
  }
}

} // namespace FanfareFm::Ui
