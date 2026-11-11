#pragma once

#include <JuceHeader.h>
#include <functional>
#include "../../ThemeManager.h"

namespace FanfareSynth::Ui::Skin {

inline bool isDark() { return ThemeManager::getInstance().active().dark; }

inline juce::Colour lift(const juce::Colour &c, float amount) {
  return isDark() ? c.brighter(amount) : c.brighter(amount * 0.55f);
}

inline juce::Colour sink(const juce::Colour &c, float amount) {
  return isDark() ? c.darker(amount) : c.darker(amount * 0.5f);
}

inline juce::String themeSignature() {
  const auto &t = ThemeManager::getInstance().active();
  return t.id + juce::String(isDark() ? 1 : 0) +
         juce::String((int)ThemeManager::get(Theme::Role::panel).getARGB()) +
         juce::String((int)ThemeManager::get(Theme::Role::accent).getARGB()) +
         juce::String((int)ThemeManager::get(Theme::Role::sunken).getARGB());
}

inline void fillVertical(juce::Graphics &g, juce::Rectangle<float> r,
                         juce::Colour top, juce::Colour bottom, float corner) {
  juce::ColourGradient grad(top, 0.0f, r.getY(), bottom, 0.0f,
                            r.getBottom(), false);
  g.setGradientFill(grad);
  if (corner > 0.0f)
    g.fillRoundedRectangle(r, corner);
  else
    g.fillRect(r);
}

inline void innerTopShadow(juce::Graphics &g, juce::Rectangle<float> r,
                           float depth, float alpha) {
  juce::ColourGradient grad(juce::Colours::black.withAlpha(alpha), 0.0f,
                            r.getY(), juce::Colours::black.withAlpha(0.0f),
                            0.0f, r.getY() + depth, false);
  g.setGradientFill(grad);
  g.fillRect(r.withHeight(depth));
}

inline void drawPlate(juce::Graphics &g, juce::Rectangle<float> r,
                      float corner) {
  const auto face = ThemeManager::get(Theme::Role::panel);
  fillVertical(g, r, lift(face, 0.10f), sink(face, 0.26f), corner);
  g.setColour(lift(face, 0.55f).withAlpha(isDark() ? 0.14f : 0.60f));
  g.fillRect(r.withHeight(1.0f).withY(r.getY() + 1.0f)
               .reduced(corner * 0.6f, 0.0f));
  g.setColour(juce::Colours::black.withAlpha(isDark() ? 0.30f : 0.08f));
  g.fillRect(r.withHeight(1.0f).withY(r.getBottom() - 2.0f)
               .reduced(corner * 0.6f, 0.0f));
  g.setColour(ThemeManager::get(Theme::Role::border));
  g.drawRoundedRectangle(r.reduced(0.5f), corner, 1.0f);
}

inline void drawRecess(juce::Graphics &g, juce::Rectangle<float> r,
                       float corner) {
  const auto well = ThemeManager::get(Theme::Role::sunken);
  fillVertical(g, r, sink(well, 0.30f), lift(well, 0.10f), corner);
  innerTopShadow(g, r.withHeight(juce::jmin(4.0f, r.getHeight() * 0.5f)),
                 juce::jmin(4.0f, r.getHeight() * 0.5f),
                 isDark() ? 0.40f : 0.16f);
  g.setColour(ThemeManager::get(Theme::Role::border));
  g.drawRoundedRectangle(r.reduced(0.5f), corner, 1.0f);
}

inline void glowStroke(juce::Graphics &g, const juce::Path &path,
                       juce::Colour col, float coreWidth) {
  const juce::PathStrokeType wide(coreWidth * 3.2f,
                                  juce::PathStrokeType::curved,
                                  juce::PathStrokeType::rounded);
  const juce::PathStrokeType mid(coreWidth * 1.8f,
                                 juce::PathStrokeType::curved,
                                 juce::PathStrokeType::rounded);
  const juce::PathStrokeType core(coreWidth, juce::PathStrokeType::curved,
                                  juce::PathStrokeType::rounded);
  g.setColour(col.withAlpha(0.10f));
  g.strokePath(path, wide);
  g.setColour(col.withAlpha(0.22f));
  g.strokePath(path, mid);
  g.setColour(col);
  g.strokePath(path, core);
}

inline void glowLine(juce::Graphics &g, float x1, float y1, float x2,
                     float y2, juce::Colour col, float coreWidth) {
  g.setColour(col.withAlpha(0.12f));
  g.drawLine(x1, y1, x2, y2, coreWidth * 3.2f);
  g.setColour(col.withAlpha(0.25f));
  g.drawLine(x1, y1, x2, y2, coreWidth * 1.8f);
  g.setColour(col);
  g.drawLine(x1, y1, x2, y2, coreWidth);
}

inline void glowFill(juce::Graphics &g, juce::Rectangle<float> r, float corner,
                     juce::Colour col) {
  g.setColour(col.withAlpha(0.10f));
  g.fillRoundedRectangle(r.expanded(2.6f), corner + 2.6f);
  g.setColour(col.withAlpha(0.22f));
  g.fillRoundedRectangle(r.expanded(1.2f), corner + 1.2f);
  g.setColour(col);
  g.fillRoundedRectangle(r, corner);
}

inline void drawLed(juce::Graphics &g, juce::Rectangle<float> r,
                    juce::Colour onCol, bool on, bool glow = true) {
  if (on && glow) {
    const auto halo = r.expanded(r.getHeight() * 1.7f, r.getHeight() * 1.2f);
    juce::ColourGradient grad(onCol.withAlpha(0.34f), r.getCentreX(),
                              r.getCentreY(), onCol.withAlpha(0.0f),
                              r.getCentreX(),
                              r.getCentreY() + halo.getHeight() * 0.5f, true);
    g.setGradientFill(grad);
    g.fillRect(halo);
  }
  g.setColour(on ? onCol : ThemeManager::get(Theme::Role::bypassed));
  g.fillRoundedRectangle(r, r.getHeight() * 0.5f);
  g.setColour(juce::Colours::white.withAlpha(on ? 0.50f : 0.10f));
  g.fillRoundedRectangle(
      r.withHeight(r.getHeight() * 0.42f)
          .withY(r.getY() + r.getHeight() * 0.14f)
          .reduced(r.getHeight() * 0.16f, 0.0f),
      r.getHeight() * 0.2f);
}

inline void drawScrew(juce::Graphics &g, juce::Point<float> c, float radius) {
  const auto face = ThemeManager::get(Theme::Role::panelAlt);
  juce::ColourGradient grad(lift(face, 0.40f), c.x - radius, c.y - radius,
                            sink(face, 0.50f), c.x + radius, c.y + radius,
                            false);
  g.setGradientFill(grad);
  g.fillEllipse(c.x - radius, c.y - radius, radius * 2.0f, radius * 2.0f);
  g.setColour(juce::Colours::black.withAlpha(isDark() ? 0.55f : 0.35f));
  g.drawEllipse(c.x - radius, c.y - radius, radius * 2.0f, radius * 2.0f,
                1.0f);
  g.setColour(sink(face, 0.65f));
  g.drawLine(c.x - radius * 0.5f, c.y - radius * 0.5f, c.x + radius * 0.5f,
             c.y + radius * 0.5f, 1.2f);
}

inline void drawGlowText(juce::Graphics &g, const juce::String &text,
                         juce::Rectangle<int> area, juce::Font font,
                         juce::Colour col, juce::Justification just,
                         bool useEllipsis = false) {
  g.setFont(font);
  g.setColour(col.withAlpha(0.32f));
  g.drawText(text, area.expanded(1, 1), just, useEllipsis);
  g.setColour(col);
  g.drawText(text, area, just, useEllipsis);
}

inline void drawKeycap(juce::Graphics &g, juce::Rectangle<float> r, bool down,
                       bool on, juce::Colour onCol, float corner = 3.0f) {
  const auto top =
      down ? ThemeManager::get(Theme::Role::pressTop)
           : ThemeManager::get(Theme::Role::faceTop);
  const auto bot =
      down ? ThemeManager::get(Theme::Role::pressBot)
           : ThemeManager::get(Theme::Role::faceBot);
  if (on) {
    g.setColour(onCol.withAlpha(0.16f));
    g.fillRoundedRectangle(r.expanded(1.6f), corner + 1.6f);
  }
  fillVertical(g, r, down ? sink(top, 0.10f) : lift(top, 0.14f),
               down ? lift(bot, 0.06f) : sink(bot, 0.12f), corner);
  if (!down) {
    g.setColour(lift(top, 0.60f).withAlpha(isDark() ? 0.18f : 0.70f));
    g.fillRect(r.withHeight(1.0f).withY(r.getY() + 1.0f)
                 .reduced(corner * 0.6f, 0.0f));
  } else {
    innerTopShadow(g, r.withHeight(3.0f), 3.0f, isDark() ? 0.35f : 0.14f);
  }
  if (on) {
    g.setColour(onCol.withAlpha(0.14f));
    g.fillRoundedRectangle(r.reduced(1.0f), corner - 1.0f);
  }
  g.setColour(on ? onCol
                 : ThemeManager::get(down ? Theme::Role::border
                                          : Theme::Role::borderStrong));
  g.drawRoundedRectangle(r.reduced(0.5f), corner, 1.0f);
}

inline juce::Image makeScanlines(int w, int h) {
  juce::Image img(juce::Image::ARGB, juce::jmax(1, w), juce::jmax(1, h), true);
  juce::Graphics g(img);
  g.setColour(juce::Colours::black.withAlpha(isDark() ? 0.15f : 0.05f));
  for (int y = 0; y < h; y += 3)
    g.fillRect(0, y, w, 1);
  return img;
}

inline juce::Image makeNoise(int w, int h) {
  w = juce::jmax(1, w);
  h = juce::jmax(1, h);
  juce::Image img(juce::Image::ARGB, w, h, true);
  juce::Graphics g(img);
  juce::Random rng(0xB1EE2u);
  const float a = isDark() ? 0.045f : 0.035f;
  const int dots = (w * h) / 26;
  for (int i = 0; i < dots; ++i) {
    const int x = rng.nextInt(w);
    const int y = rng.nextInt(h);
    g.setColour(rng.nextBool() ? juce::Colours::white.withAlpha(a)
                               : juce::Colours::black.withAlpha(a));
    g.fillRect(x, y, 1, 1);
  }
  return img;
}

inline void drawVignette(juce::Graphics &g, juce::Rectangle<float> r) {
  const float depth =
      juce::jmin(70.0f, r.getWidth() * 0.06f, r.getHeight() * 0.09f);
  const float a = isDark() ? 0.30f : 0.09f;
  const auto edge = juce::Colours::black.withAlpha(a);
  const auto none = juce::Colours::black.withAlpha(0.0f);
  juce::ColourGradient top(edge, 0.0f, r.getY(), none, 0.0f,
                           r.getY() + depth, false);
  g.setGradientFill(top);
  g.fillRect(r.withHeight(depth));
  juce::ColourGradient bot(none, 0.0f, r.getBottom() - depth, edge, 0.0f,
                           r.getBottom(), false);
  g.setGradientFill(bot);
  g.fillRect(r.withY(r.getBottom() - depth).withHeight(depth));
  juce::ColourGradient left(edge, r.getX(), 0.0f, none, r.getX() + depth,
                            0.0f, false);
  g.setGradientFill(left);
  g.fillRect(r.withWidth(depth));
  juce::ColourGradient right(none, r.getRight() - depth, 0.0f, edge,
                             r.getRight(), 0.0f, false);
  g.setGradientFill(right);
  g.fillRect(r.withX(r.getRight() - depth).withWidth(depth));
}

class ChromeCache {
public:
  using Painter = std::function<void(juce::Graphics &, juce::Rectangle<int>)>;

  void render(int w, int h, const juce::String &sig, const Painter &paint) {
    if (w <= 0 || h <= 0) {
      image = juce::Image();
      signature.clear();
      return;
    }
    if (image.isValid() && image.getWidth() == w && image.getHeight() == h &&
        signature == sig)
      return;
    image = juce::Image(juce::Image::ARGB, w, h, true);
    juce::Graphics g(image);
    paint(g, juce::Rectangle<int>(0, 0, w, h));
    signature = sig;
  }

  void draw(juce::Graphics &g, int x, int y) const {
    if (image.isValid())
      g.drawImageAt(image, x, y);
  }

  bool ready() const { return image.isValid(); }

private:
  juce::Image image;
  juce::String signature;
};

} // namespace FanfareSynth::Ui::Skin
