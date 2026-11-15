#pragma once

#include <JuceHeader.h>
#include <array>
#include <cctype>
#include <cmath>
#include <functional>
#include <map>
#include <memory>
#include <utility>
#include <vector>

#include "../../../BoutiqueLookAndFeel.h"
#include "../../../ButtonRoles.h"
#include "../../../RackSlot.h"
#include "../../../SynthMidiSetupDialog.h"
#include "../../../ThemeManager.h"
#include "../B3OrganProcessor.h"

namespace FanfareB3::Ui {

// ---- Shared Vintage Hardware Drawing Helpers ----
inline void drawBrassScrew(juce::Graphics &g, float cx, float cy, float r = 2.6f,
                           float angle = 0.68f) {
  g.setColour(juce::Colour(0xCC0D0703));
  g.fillEllipse(cx - r - 0.5f, cy - r - 0.2f, r * 2.0f + 1.0f, r * 2.0f + 1.0f);
  juce::ColourGradient grad(juce::Colour(0xFFE5C481), cx - r * 0.5f, cy - r * 0.5f,
                            juce::Colour(0xFF694B20), cx + r * 0.7f, cy + r * 0.7f, true);
  g.setGradientFill(grad);
  g.fillEllipse(cx - r, cy - r, r * 2.0f, r * 2.0f);
  g.setColour(juce::Colour(0x55FFF3D1));
  g.drawEllipse(cx - r + 0.3f, cy - r + 0.3f, r * 2.0f - 0.6f, r * 2.0f - 0.6f, 0.5f);
  const float dx = std::cos(angle) * (r * 0.68f);
  const float dy = std::sin(angle) * (r * 0.68f);
  g.setColour(juce::Colour(0xFF2B1B0A));
  g.drawLine(cx - dx, cy - dy, cx + dx, cy + dy, 0.95f);
}

inline void drawWalnutWood(juce::Graphics &g, juce::Rectangle<float> area,
                           float cornerRadius, bool horizontalGrain) {
  if (area.isEmpty())
    return;
  juce::ColourGradient baseGrad(
      juce::Colour(0xFF563119), area.getX(), area.getY(),
      juce::Colour(0xFF2B1408),
      horizontalGrain ? area.getX() : area.getRight(),
      horizontalGrain ? area.getBottom() : area.getY(), false);
  baseGrad.addColour(0.45, juce::Colour(0xFF422310));
  g.setGradientFill(baseGrad);
  g.fillRoundedRectangle(area, cornerRadius);

  g.saveState();
  juce::Path clip;
  clip.addRoundedRectangle(area, cornerRadius);
  g.reduceClipRegion(clip);

  if (horizontalGrain) {
    const int y0 = (int)area.getY();
    const int y1 = (int)std::ceil(area.getBottom());
    for (int y = y0; y < y1; y += 2) {
      const float fy = (float)y;
      const float w = std::sin(fy * 0.14f) * 0.6f + std::cos(fy * 0.33f) * 0.4f;
      if (std::abs(w) > 0.22f) {
        g.setColour(w > 0.0f ? juce::Colour(0x1C8B5028) : juce::Colour(0x26120602));
        g.drawLine(area.getX(), fy, area.getRight(), fy + w * 4.5f,
                   w > 0.0f ? 1.1f : 1.5f);
      }
    }
  } else {
    const int x0 = (int)area.getX();
    const int x1 = (int)std::ceil(area.getRight());
    for (int x = x0; x < x1; x += 3) {
      const float fx = (float)x;
      const float w = std::sin(fx * 0.17f) + 0.45f * std::cos(fx * 0.41f);
      const bool light = w > 0.20f;
      g.setColour(light ? juce::Colour(0x1792562D) : juce::Colour(0x1E120703));
      g.drawLine(fx, area.getY(), fx + w * 2.5f, area.getBottom(), light ? 1.1f : 1.5f);
    }
  }
  g.restoreState();
}

inline void drawBrushedBronzePanel(juce::Graphics &g, juce::Rectangle<float> area,
                                   const juce::String &heading, bool withScrews = true) {
  if (area.isEmpty())
    return;
  g.setColour(juce::Colour(0x88080402));
  g.fillRoundedRectangle(area.translated(0.0f, 1.5f), 4.0f);

  juce::ColourGradient face(juce::Colour(0xFF28221D), area.getX(), area.getY(),
                            juce::Colour(0xFF181410), area.getX(), area.getBottom(), false);
  g.setGradientFill(face);
  g.fillRoundedRectangle(area, 4.0f);

  g.saveState();
  juce::Path p;
  p.addRoundedRectangle(area, 4.0f);
  g.reduceClipRegion(p);
  for (int y = (int)area.getY() + 2; y < (int)area.getBottom(); y += 3) {
    g.setColour((y % 6 == 0) ? juce::Colour(0x0BFFFFFF) : juce::Colour(0x10000000));
    g.drawHorizontalLine(y, area.getX(), area.getRight());
  }
  g.restoreState();

  g.setColour(juce::Colour(0x666E5A44));
  g.drawRoundedRectangle(area.reduced(0.5f), 4.0f, 1.0f);
  g.setColour(juce::Colour(0x30FFF2D6));
  g.drawLine(area.getX() + 4.0f, area.getY() + 1.0f,
             area.getRight() - 4.0f, area.getY() + 1.0f, 0.8f);

  if (withScrews) {
    const float inset = 6.5f;
    drawBrassScrew(g, area.getX() + inset, area.getY() + inset, 2.2f, 0.5f);
    drawBrassScrew(g, area.getRight() - inset, area.getY() + inset, 2.2f, 1.1f);
    drawBrassScrew(g, area.getX() + inset, area.getBottom() - inset, 2.2f, -0.7f);
    drawBrassScrew(g, area.getRight() - inset, area.getBottom() - inset, 2.2f, 0.2f);
  }

  if (heading.isNotEmpty()) {
    g.setColour(juce::Colour(0xFFE5D1AA));
    g.setFont(BoutiqueLookAndFeel::condensedFont(10.5f, 0.08f));
    g.drawText(heading, (int)area.getX() + 14, (int)area.getY() + 4,
               (int)area.getWidth() - 28, 15, juce::Justification::centredLeft, false);
  }
}

// ---- Custom Hardware LookAndFeel matching Vintage Walnut B3 Mockup ----
class B3VintageLookAndFeel : public BoutiqueLookAndFeel {
public:
  B3VintageLookAndFeel() {
    setColour(juce::ComboBox::backgroundColourId, juce::Colour(0xFF221B16));
    setColour(juce::ComboBox::textColourId, juce::Colour(0xFFEBDDC5));
    setColour(juce::ComboBox::outlineColourId, juce::Colour(0xFF6B5842));
    setColour(juce::ComboBox::arrowColourId, juce::Colour(0xFFD0A96A));
    setColour(juce::PopupMenu::backgroundColourId, juce::Colour(0xFF1D1713));
    setColour(juce::PopupMenu::textColourId, juce::Colour(0xFFE8DAC2));
    setColour(juce::PopupMenu::highlightedBackgroundColourId, juce::Colour(0xFF8B4D20));
    setColour(juce::PopupMenu::highlightedTextColourId, juce::Colours::white);
    setColour(juce::TextButton::buttonColourId, juce::Colour(0xFF28201A));
    setColour(juce::TextButton::buttonOnColourId, juce::Colour(0xFF7D431A));
    setColour(juce::TextButton::textColourOffId, juce::Colour(0xFFE6D7BE));
    setColour(juce::TextButton::textColourOnId, juce::Colour(0xFFFFF4E0));
    setColour(juce::ToggleButton::textColourId, juce::Colour(0xFFE2D4BC));
    setColour(juce::ToggleButton::tickColourId, juce::Colour(0xFFE07C34));
  }

  void drawButtonBackground(juce::Graphics &g, juce::Button &b,
                            const juce::Colour &bg, bool highlighted, bool down) override {
    juce::ignoreUnused(bg);
    const auto r = b.getLocalBounds().toFloat().reduced(0.5f);
    const bool on = b.getToggleState();
    auto topC = on ? juce::Colour(0xFF73421D) : juce::Colour(0xFF2C241E);
    auto botC = on ? juce::Colour(0xFF472710) : juce::Colour(0xFF191410);
    if (highlighted && b.isEnabled())
      topC = topC.brighter(0.14f);
    if (down)
      std::swap(topC, botC);

    juce::ColourGradient grad(topC, r.getX(), r.getY(), botC, r.getX(), r.getBottom(), false);
    g.setGradientFill(grad);
    g.fillRoundedRectangle(r, 3.0f);
    g.setColour(on ? juce::Colour(0xFFD59B56) : juce::Colour(0x886E5942));
    g.drawRoundedRectangle(r, 3.0f, 0.9f);
  }

  void drawComboBox(juce::Graphics &g, int width, int height, bool down,
                    int buttonX, int buttonY, int buttonW, int buttonH,
                    juce::ComboBox &box) override {
    juce::ignoreUnused(down);
    const auto r = juce::Rectangle<float>(0.5f, 0.5f, (float)width - 1.0f, (float)height - 1.0f);
    juce::ColourGradient grad(juce::Colour(0xFF28201B), 0.0f, 0.0f,
                              juce::Colour(0xFF17120E), 0.0f, (float)height, false);
    g.setGradientFill(grad);
    g.fillRoundedRectangle(r, 3.0f);
    g.setColour(box.hasKeyboardFocus(false) ? juce::Colour(0xFFD69F58)
                                            : juce::Colour(0x886E5942));
    g.drawRoundedRectangle(r, 3.0f, 1.0f);

    const float ax = (float)buttonX + (float)buttonW * 0.5f;
    const float ay = (float)buttonY + (float)buttonH * 0.5f;
    juce::Path arrow;
    arrow.addTriangle(ax - 3.5f, ay - 2.0f, ax + 3.5f, ay - 2.0f, ax, ay + 2.8f);
    g.setColour(juce::Colour(0xFFD2AD6F));
    g.fillPath(arrow);
  }

  void drawRotarySlider(juce::Graphics &g, int x, int y, int width, int height,
                        float sliderPos, float rotaryStartAngle,
                        float rotaryEndAngle, juce::Slider &s) override {
    juce::ignoreUnused(s);
    const auto b = juce::Rectangle<float>((float)x, (float)y, (float)width, (float)height).reduced(2.0f);
    const float rad = juce::jmax(8.0f, juce::jmin(b.getWidth(), b.getHeight()) * 0.45f);
    const float cx = b.getCentreX();
    const float cy = b.getCentreY();
    const float angle = rotaryStartAngle + sliderPos * (rotaryEndAngle - rotaryStartAngle);

    // Radial tick dots around sweep
    const int numTicks = 9;
    for (int i = 0; i < numTicks; ++i) {
      const float t = (float)i / (float)(numTicks - 1);
      const float ta = rotaryStartAngle + t * (rotaryEndAngle - rotaryStartAngle);
      const float tx = cx + std::sin(ta) * (rad + 2.6f);
      const float ty = cy - std::cos(ta) * (rad + 2.6f);
      const bool lit = (t <= sliderPos + 0.01f);
      g.setColour(lit ? juce::Colour(0xFFE0974A) : juce::Colour(0x55756654));
      g.fillEllipse(tx - 1.1f, ty - 1.1f, 2.2f, 2.2f);
    }

    // Cast drop shadow
    g.setColour(juce::Colour(0xAA060301));
    g.fillEllipse(cx - rad + 0.5f, cy - rad + 2.2f, rad * 2.0f, rad * 2.0f);

    // Fluted dark bakelite outer skirt
    const auto skirt = juce::Rectangle<float>(cx - rad, cy - rad, rad * 2.0f, rad * 2.0f);
    juce::ColourGradient skirtG(juce::Colour(0xFF3E291B), cx - rad * 0.6f, cy - rad * 0.6f,
                                juce::Colour(0xFF140C07), cx + rad * 0.6f, cy + rad * 0.6f, true);
    g.setGradientFill(skirtG);
    g.fillEllipse(skirt);
    g.setColour(juce::Colour(0x55A07D53));
    g.drawEllipse(skirt, 0.8f);

    // Knurl flutes around perimeter
    const int flutes = 12;
    for (int i = 0; i < flutes; ++i) {
      const float fa = angle + (float)i * (juce::MathConstants<float>::twoPi / (float)flutes);
      const float r0 = rad * 0.72f;
      const float r1 = rad * 0.96f;
      g.setColour(juce::Colour(0x40050201));
      g.drawLine(cx + std::sin(fa) * r0, cy - std::cos(fa) * r0,
                 cx + std::sin(fa) * r1, cy - std::cos(fa) * r1, 1.2f);
    }

    // Conical brushed brass center cap
    const float capR = rad * 0.64f;
    const auto cap = juce::Rectangle<float>(cx - capR, cy - capR, capR * 2.0f, capR * 2.0f);
    juce::ColourGradient capG(juce::Colour(0xFFDFBC7A), cx - capR * 0.6f, cy - capR * 0.7f,
                              juce::Colour(0xFF5D421D), cx + capR * 0.7f, cy + capR * 0.7f, true);
    g.setGradientFill(capG);
    g.fillEllipse(cap);
    g.setColour(juce::Colour(0x77FFF0CE));
    g.drawEllipse(cap.reduced(0.4f), 0.7f);

    // Crisp ivory indicator pointer
    const float px0 = cx + std::sin(angle) * (capR * 0.25f);
    const float py0 = cy - std::cos(angle) * (capR * 0.25f);
    const float px1 = cx + std::sin(angle) * (rad * 0.93f);
    const float py1 = cy - std::cos(angle) * (rad * 0.93f);
    g.setColour(juce::Colour(0xFFF6EBD4));
    g.drawLine(px0, py0, px1, py1, juce::jlimit(1.6f, 2.6f, rad * 0.13f));
  }

  void drawLinearSlider(juce::Graphics &g, int x, int y, int width, int height,
                        float sliderPos, float minSliderPos, float maxSliderPos,
                        juce::Slider::SliderStyle style, juce::Slider &s) override {
    juce::ignoreUnused(minSliderPos, maxSliderPos, s);
    if (style != juce::Slider::LinearHorizontal) {
      BoutiqueLookAndFeel::drawLinearSlider(g, x, y, width, height, sliderPos,
                                            minSliderPos, maxSliderPos, style, s);
      return;
    }

    const float cy = (float)y + (float)height * 0.5f;
    const auto track = juce::Rectangle<float>((float)x + 5.0f, cy - 2.2f,
                                              juce::jmax(8.0f, (float)width - 10.0f), 4.4f);

    // Subtle engraved tick notches along track
    const int ticks = 9;
    for (int i = 0; i < ticks; ++i) {
      const float tx = track.getX() + ((float)i / (float)(ticks - 1)) * track.getWidth();
      g.setColour(juce::Colour(0x44C2A67D));
      g.drawVerticalLine((int)std::round(tx), cy - 5.5f, cy - 3.2f);
      g.drawVerticalLine((int)std::round(tx), cy + 3.2f, cy + 5.5f);
    }

    g.setColour(juce::Colour(0xFF0A0806));
    g.fillRoundedRectangle(track, 2.0f);
    g.setColour(juce::Colour(0x5575634E));
    g.drawRoundedRectangle(track, 2.0f, 0.7f);

    // Warm filled portion up to thumb
    const float fillW = juce::jlimit(0.0f, track.getWidth(), sliderPos - track.getX());
    if (fillW > 1.0f) {
      g.setColour(juce::Colour(0x88BA622B));
      g.fillRoundedRectangle(track.withWidth(fillW), 2.0f);
    }

    // Machined brushed steel/pewter fader cap
    const float tw = 11.0f;
    const float th = juce::jlimit(12.0f, 16.0f, (float)height - 2.0f);
    const auto thumb = juce::Rectangle<float>(
        juce::jlimit(track.getX() - tw * 0.3f, track.getRight() - tw * 0.7f, sliderPos - tw * 0.5f),
        cy - th * 0.5f, tw, th);

    g.setColour(juce::Colour(0x99060301));
    g.fillRoundedRectangle(thumb.translated(0.5f, 1.5f), 2.0f);
    juce::ColourGradient capG(juce::Colour(0xFFC2BDB3), thumb.getX(), thumb.getY(),
                              juce::Colour(0xFF565148), thumb.getX(), thumb.getBottom(), false);
    g.setGradientFill(capG);
    g.fillRoundedRectangle(thumb, 2.0f);
    g.setColour(juce::Colour(0x88FFF6E4));
    g.drawRoundedRectangle(thumb, 2.0f, 0.7f);

    g.setColour(juce::Colour(0xFF1F1A15));
    g.drawVerticalLine((int)std::round(thumb.getCentreX()), thumb.getY() + 2.0f, thumb.getBottom() - 2.0f);
  }

  void drawToggleButton(juce::Graphics &g, juce::ToggleButton &b,
                        bool highlighted, bool down) override {
    juce::ignoreUnused(down);
    const auto id = b.getComponentID();
    const auto bounds = b.getLocalBounds().toFloat();
    const bool on = b.getToggleState();

    if (id == "rocker") {
      // Classic Hammond cream/ivory tilting rocker tablet
      const auto r = bounds.reduced(1.5f, 1.0f);
      g.setColour(juce::Colour(0xFF0E0A07));
      g.fillRoundedRectangle(r, 3.0f);
      const auto face = r.reduced(1.8f);
      auto topC = on ? juce::Colour(0xFFF3E8CF) : juce::Colour(0xFF9F9078);
      auto botC = on ? juce::Colour(0xFFD5C4A0) : juce::Colour(0xFF756752);
      if (highlighted)
        topC = topC.brighter(0.08f);
      juce::ColourGradient gTab(topC, face.getX(), face.getY(),
                                botC, face.getX(), face.getBottom(), false);
      g.setGradientFill(gTab);
      g.fillRoundedRectangle(face, 2.5f);

      // Tilted top/bottom shadow crease to convey physical rocker angle
      g.setColour(on ? juce::Colour(0x453A2610) : juce::Colour(0x55FFF5DF));
      if (on)
        g.fillRect(face.withY(face.getBottom() - 3.5f).withHeight(3.5f));
      else
        g.fillRect(face.withHeight(3.0f));

      g.setColour(juce::Colour(0xFF2E2216));
      g.drawRoundedRectangle(face, 2.5f, 0.9f);

      g.setColour(on ? juce::Colour(0xFF281B0E) : juce::Colour(0xFF1F150B));
      g.setFont(condensedFont(11.0f, 0.03f));
      g.drawText(b.getButtonText(), face.reduced(2.0f), juce::Justification::centred, false);
      return;
    }

    if (id == "bat") {
      // Vintage chrome bat-handle toggle switch + etched text
      const float cy = bounds.getCentreY();
      const float sx = bounds.getX() + 11.0f;
      // Hex mounting bushing
      g.setColour(juce::Colour(0xFF140F0B));
      g.fillEllipse(sx - 6.0f, cy - 6.0f, 12.0f, 12.0f);
      g.setColour(juce::Colour(0xFFA8A399));
      g.fillEllipse(sx - 4.5f, cy - 4.5f, 9.0f, 9.0f);
      g.setColour(juce::Colour(0xFF3E3A34));
      g.fillEllipse(sx - 2.2f, cy - 2.2f, 4.4f, 4.4f);

      // Chrome bat handle leaning up (ON) or down (OFF)
      const float tipY = on ? (cy - 7.5f) : (cy + 7.5f);
      const float tipX = sx + 1.5f;
      g.setColour(juce::Colour(0x99050301));
      g.drawLine(sx + 1.0f, cy + 1.0f, tipX + 1.0f, tipY + 1.0f, 3.2f);
      g.setColour(juce::Colour(0xFFD6D8DA));
      g.drawLine(sx, cy, tipX, tipY, 2.6f);
      g.fillEllipse(tipX - 2.6f, tipY - 2.6f, 5.2f, 5.2f);
      g.setColour(juce::Colours::white.withAlpha(0.75f));
      g.fillEllipse(tipX - 1.3f, tipY - 1.6f, 2.0f, 2.0f);

      g.setColour(on ? juce::Colour(0xFFF3E4C8) : juce::Colour(0xFFCBBBA2));
      g.setFont(condensedFont(11.0f, 0.03f));
      g.drawText(b.getButtonText(),
                 bounds.withTrimmedLeft(21.0f), juce::Justification::centredLeft, false);
      return;
    }

    // Default vintage dark-bronze square checkbox with warm amber check
    const float boxSz = juce::jlimit(10.0f, 13.0f, bounds.getHeight() - 4.0f);
    const auto boxR = juce::Rectangle<float>(bounds.getX() + 2.0f,
                                             bounds.getCentreY() - boxSz * 0.5f,
                                             boxSz, boxSz);
    g.setColour(on ? juce::Colour(0xFF8A3E15) : juce::Colour(0xFF17120E));
    g.fillRoundedRectangle(boxR, 2.0f);
    g.setColour(on ? juce::Colour(0xFFE07D36)
                   : (highlighted ? juce::Colour(0xFF9C8466) : juce::Colour(0xFF635240)));
    g.drawRoundedRectangle(boxR, 2.0f, 1.0f);

    if (on) {
      juce::Path tick;
      tick.startNewSubPath(boxR.getX() + boxSz * 0.22f, boxR.getCentreY() + boxSz * 0.02f);
      tick.lineTo(boxR.getX() + boxSz * 0.44f, boxR.getBottom() - boxSz * 0.22f);
      tick.lineTo(boxR.getRight() - boxSz * 0.20f, boxR.getY() + boxSz * 0.24f);
      g.setColour(juce::Colour(0xFFFFEAD0));
      g.strokePath(tick, juce::PathStrokeType(1.7f, juce::PathStrokeType::curved,
                                              juce::PathStrokeType::rounded));
    }

    g.setColour(on ? juce::Colour(0xFFF4E7CE) : juce::Colour(0xFFC4B59E));
    g.setFont(condensedFont(11.0f, 0.02f));
    g.drawText(b.getButtonText(), bounds.withTrimmedLeft(boxSz + 6.0f),
               juce::Justification::centredLeft, false);
  }
};

// Tactile 7-position rotary Scanner Vibrato/Chorus selector (OFF, V1..C3)
class B3ScannerDial : public juce::Component {
public:
  explicit B3ScannerDial(B3OrganProcessor &processor) : proc(processor) {}
  std::function<void()> onModeChanged;

  void paint(juce::Graphics &g) override {
    const auto b = getLocalBounds().toFloat();
    if (b.isEmpty())
      return;
    const float rad = juce::jlimit(10.0f, 22.0f, juce::jmin(b.getWidth(), b.getHeight()) * 0.36f);
    const float cx = b.getCentreX();
    const float cy = b.getCentreY() + 2.0f;
    const int mode = juce::jlimit(0, 6, proc.scannerMode.load(std::memory_order_relaxed));

    constexpr float startA = -2.10f;
    constexpr float endA = 2.10f;
    for (int i = 0; i < 7; ++i) {
      const float t = (float)i / 6.0f;
      const float a = startA + t * (endA - startA);
      const float tx = cx + std::sin(a) * (rad + 4.0f);
      const float ty = cy - std::cos(a) * (rad + 4.0f);
      g.setColour(i == mode ? juce::Colour(0xFFE58238) : juce::Colour(0x887C6A54));
      g.fillEllipse(tx - (i == mode ? 1.8f : 1.1f),
                    ty - (i == mode ? 1.8f : 1.1f),
                    i == mode ? 3.6f : 2.2f,
                    i == mode ? 3.6f : 2.2f);
    }

    g.setColour(juce::Colour(0xAA070402));
    g.fillEllipse(cx - rad + 0.5f, cy - rad + 2.0f, rad * 2.0f, rad * 2.0f);

    const auto knob = juce::Rectangle<float>(cx - rad, cy - rad, rad * 2.0f, rad * 2.0f);
    juce::ColourGradient kg(juce::Colour(0xFF4F2B16), cx - rad * 0.5f, cy - rad * 0.5f,
                            juce::Colour(0xFF1C0E06), cx + rad * 0.6f, cy + rad * 0.6f, true);
    g.setGradientFill(kg);
    g.fillEllipse(knob);
    g.setColour(juce::Colour(0x77BA8D54));
    g.drawEllipse(knob, 0.9f);

    const float selA = startA + ((float)mode / 6.0f) * (endA - startA);
    g.setColour(juce::Colour(0xFFF3E4C9));
    g.drawLine(cx + std::sin(selA) * (rad * 0.18f), cy - std::cos(selA) * (rad * 0.18f),
               cx + std::sin(selA) * (rad * 0.88f), cy - std::cos(selA) * (rad * 0.88f), 2.1f);
  }

  void mouseDown(const juce::MouseEvent &e) override {
    dragStartX = e.position.x;
    dragStartMode = proc.scannerMode.load(std::memory_order_relaxed);
  }

  void mouseDrag(const juce::MouseEvent &e) override {
    const int delta = (int)std::lround((e.position.x - dragStartX) / 14.0f);
    const int next = juce::jlimit(0, 6, dragStartMode + delta);
    if (next != proc.scannerMode.load(std::memory_order_relaxed)) {
      proc.setScannerMode(next);
      repaint();
      if (onModeChanged)
        onModeChanged();
    }
  }

  void mouseUp(const juce::MouseEvent &e) override {
    if (e.getDistanceFromDragStart() < 4) {
      const int cur = proc.scannerMode.load(std::memory_order_relaxed);
      const int next = (cur + 1) % 7;
      proc.setScannerMode(next);
      repaint();
      if (onModeChanged)
        onModeChanged();
    }
  }

  void mouseWheelMove(const juce::MouseEvent &, const juce::MouseWheelDetails &w) override {
    const int step = w.deltaY > 0.0f ? 1 : -1;
    const int next = juce::jlimit(0, 6, proc.scannerMode.load(std::memory_order_relaxed) + step);
    proc.setScannerMode(next);
    repaint();
    if (onModeChanged)
      onModeChanged();
  }

private:
  B3OrganProcessor &proc;
  float dragStartX = 0.0f;
  int dragStartMode = 4;
};

// Custom-drawn tactile pull-down Hammond B3 drawbars:
// 0 = pushed in at top, 8 = pulled down toward player.
// Supports vertical detent drag (Shift = fine continuous), horizontal smear
// across all 9 drawbars, mouse wheel stepping, and double-click reset to 0.
class HammondDrawbarStrip : public juce::Component {
public:
  explicit HammondDrawbarStrip(B3OrganProcessor &processor) : proc(processor) {
    setRepaintsOnMouseActivity(true);
  }

  std::function<void()> onAnyChanged;
  bool isUserDragging() const noexcept { return activeCol >= 0; }

  void paint(juce::Graphics &g) override {
    const auto bounds = getLocalBounds().toFloat();
    if (bounds.isEmpty())
      return;

    const int iw = getWidth();
    const int ih = getHeight();
    if (!cachedBackdrop.isValid() || cachedBackdrop.getWidth() != iw ||
        cachedBackdrop.getHeight() != ih) {
      cachedBackdrop = juce::Image(juce::Image::ARGB, iw, ih, true);
      juce::Graphics bg(cachedBackdrop);
      drawBackdrop(bg, bounds);
    }
    g.drawImageAt(cachedBackdrop, 0, 0);

    static const juce::Colour kBarColors[9] = {
        juce::Colour(0xFF834420), // 0: 16' Brown
        juce::Colour(0xFF834420), // 1: 5 1/3' Brown
        juce::Colour(0xFFF0E6D2), // 2: 8' Ivory White
        juce::Colour(0xFFF0E6D2), // 3: 4' Ivory White
        juce::Colour(0xFF211F1E), // 4: 2 2/3' Ebony Black
        juce::Colour(0xFFF0E6D2), // 5: 2' Ivory White
        juce::Colour(0xFF211F1E), // 6: 1 3/5' Ebony Black
        juce::Colour(0xFF211F1E), // 7: 1 1/3' Ebony Black
        juce::Colour(0xFFF0E6D2)  // 8: 1' Ivory White
    };

    const bool percCancelActive = proc.percOn.load(std::memory_order_relaxed);
    const float colW = bounds.getWidth() / 9.0f;

    for (int i = 0; i < 9; ++i) {
      const auto col = juce::Rectangle<float>(
          bounds.getX() + (float)i * colW, bounds.getY(), colW, bounds.getHeight());
      const auto well = trackRectForCol(col);
      const float cx = well.getCentreX();
      const float capH = 26.0f;
      const float travel = juce::jmax(10.0f, well.getHeight() - capH);

      // Drawbar pull value (0 = pushed in at top, 8 = pulled out at bottom)
      const float val = proc.getDrawbar(i);
      const int q = juce::jlimit(0, 8, (int)std::lround(val * 8.0f));
      const bool dimmedPerc = (i == 8 && percCancelActive);

      const float capW = juce::jlimit(28.0f, 46.0f, colW * 0.56f);
      const float capY = well.getY() + val * travel;

      // Sculpted 3D Fluted Hammond Drawbar Cap
      const auto capR = juce::Rectangle<float>(cx - capW * 0.5f, capY, capW, capH);
      juce::Colour baseCol = kBarColors[i];
      if (dimmedPerc)
        baseCol = baseCol.withMultipliedSaturation(0.55f).withAlpha(0.65f);
      if (i == activeCol || i == hoverCol)
        baseCol = baseCol.brighter(i == 4 || i == 6 || i == 7 ? 0.16f : 0.06f);

      g.setColour(juce::Colours::black.withAlpha(0.58f));
      g.fillRoundedRectangle(capR.translated(0.0f, 2.8f), 3.5f);

      juce::ColourGradient capGrad(baseCol.brighter(0.16f), capR.getX(), capR.getY(),
                                   baseCol.darker(0.32f), capR.getX(), capR.getBottom(), false);
      g.setGradientFill(capGrad);
      g.fillRoundedRectangle(capR, 3.5f);

      const bool isLightCap = (i == 2 || i == 3 || i == 5 || i == 8);
      // Horizontal finger-grip flute grooves along upper/lower thirds of cap
      for (float fy : {capR.getY() + 4.5f, capR.getBottom() - 5.0f}) {
        g.setColour(isLightCap ? juce::Colour(0x2E000000) : juce::Colour(0x28FFFFFF));
        g.drawLine(capR.getX() + 4.0f, fy, capR.getRight() - 4.0f, fy, 0.9f);
      }
      g.setColour(isLightCap ? juce::Colour(0x553A2A18) : juce::Colour(0x38FFF1D8));
      g.drawRoundedRectangle(capR, 3.5f, 1.0f);

      // Step number stamped on cap
      g.setColour(isLightCap ? juce::Colour(0xFF201810) : juce::Colour(0xFFF3ECDC));
      g.setFont(BoutiqueLookAndFeel::monoFont(11.0f, true));
      g.drawText(juce::String(q), capR.reduced(1.0f), juce::Justification::centred, false);

      // Compact brass/ebony live value badge at bottom of walnut well
      auto botArea = juce::Rectangle<float>(col.getX() + 6.0f,
                                            bounds.getBottom() - 20.0f,
                                            col.getWidth() - 12.0f, 15.0f);
      if (dimmedPerc) {
        g.setColour(juce::Colour(0x99352110));
        g.fillRoundedRectangle(botArea, 2.5f);
        g.setColour(juce::Colour(0xFFE0974A));
        g.setFont(BoutiqueLookAndFeel::monoFont(8.5f, true));
        g.drawText("PERC", botArea, juce::Justification::centred, false);
      } else if (q > 0) {
        g.setColour(juce::Colour(0x992C190D));
        g.fillRoundedRectangle(botArea, 2.5f);
        g.setColour(juce::Colour(0xFFE7AD63));
        g.setFont(BoutiqueLookAndFeel::monoFont(10.0f, true));
        g.drawText(juce::String(q), botArea, juce::Justification::centred, false);
      }
    }
  }

  void drawBackdrop(juce::Graphics &g, juce::Rectangle<float> bounds) const {
    static constexpr const char *kFootages[9] = {
        "16'", "5 1/3'", "8'", "4'", "2 2/3'", "2'", "1 3/5'", "1 1/3'", "1'"};
    static constexpr const char *kHarmonicNames[9] = {
        "SUB", "5TH", "FUND", "OCT", "12TH", "15TH", "17TH", "19TH", "22ND"};

    // Walnut wood plank backdrop inside the drawbar bay
    drawWalnutWood(g, bounds, 4.0f, true);

    // Top brushed dark-bronze header bezel strip for footages
    const auto topRail = bounds.withHeight(28.0f);
    juce::ColourGradient railG(juce::Colour(0xFF282019), topRail.getX(), topRail.getY(),
                               juce::Colour(0xFF17120E), topRail.getX(), topRail.getBottom(), false);
    g.setGradientFill(railG);
    g.fillRoundedRectangle(topRail, 4.0f);
    g.setColour(juce::Colour(0x887C6449));
    g.drawLine(topRail.getX(), topRail.getBottom(), topRail.getRight(), topRail.getBottom(), 1.2f);
    g.setColour(juce::Colour(0xAA070301));
    g.drawLine(topRail.getX(), topRail.getBottom() + 1.5f, topRail.getRight(), topRail.getBottom() + 1.5f, 2.0f);

    const float colW = bounds.getWidth() / 9.0f;
    for (int i = 0; i < 9; ++i) {
      const auto col = juce::Rectangle<float>(
          bounds.getX() + (float)i * colW, bounds.getY(), colW, bounds.getHeight());
      const auto well = trackRectForCol(col);

      // Top footage & harmonic subtitle
      auto topArea = juce::Rectangle<float>(col.getX(), col.getY() + 2.0f, col.getWidth(), 24.0f);
      g.setColour(juce::Colour(0xFFF0DFBF));
      g.setFont(BoutiqueLookAndFeel::condensedFont(12.5f, 0.03f));
      g.drawText(kFootages[i], topArea.removeFromTop(13.5f), juce::Justification::centred, false);

      g.setColour(juce::Colour(0xFFB39C7B));
      g.setFont(BoutiqueLookAndFeel::monoFont(8.2f, false));
      g.drawText(kHarmonicNames[i], topArea, juce::Justification::centred, false);

      // Brushed steel drawbar track plate with top/bottom brass screws
      const float cx = well.getCentreX();
      const float plateW = juce::jlimit(16.0f, 24.0f, colW * 0.24f);
      const auto plateR = juce::Rectangle<float>(
          cx - plateW * 0.5f, well.getY() - 5.0f, plateW, well.getHeight() + 10.0f);

      g.setColour(juce::Colour(0x99080402));
      g.fillRoundedRectangle(plateR.translated(0.8f, 1.4f), 2.5f);

      juce::ColourGradient plateG(juce::Colour(0xFFABA69C), plateR.getX(), plateR.getY(),
                                  juce::Colour(0xFF5D5850), plateR.getRight(), plateR.getY(), false);
      plateG.addColour(0.5, juce::Colour(0xFF8E897F));
      g.setGradientFill(plateG);
      g.fillRoundedRectangle(plateR, 2.5f);
      g.setColour(juce::Colour(0x66FFFFFF));
      g.drawRoundedRectangle(plateR, 2.5f, 0.8f);

      // Dark center mechanical slot
      const float slotW = juce::jlimit(4.0f, 7.0f, plateW * 0.30f);
      const auto slotR = juce::Rectangle<float>(
          cx - slotW * 0.5f, well.getY() + 5.0f, slotW, juce::jmax(10.0f, well.getHeight() - 10.0f));
      g.setColour(juce::Colour(0xFF0B0907));
      g.fillRoundedRectangle(slotR, 1.5f);

      // Etched detent tick lines & 1..8 numbers along steel track
      const float capH = 26.0f;
      const float travel = juce::jmax(10.0f, well.getHeight() - capH);
      g.setFont(BoutiqueLookAndFeel::monoFont(7.8f, true));
      for (int step = 0; step <= 8; ++step) {
        const float ty = well.getY() + capH * 0.5f + ((float)step / 8.0f) * travel;
        g.setColour(juce::Colour(step == 0 || step == 8 ? 0xCC241C14 : 0x88352D24));
        g.drawHorizontalLine((int)std::round(ty), plateR.getX() + 1.5f, slotR.getX() - 1.0f);
        g.drawHorizontalLine((int)std::round(ty), slotR.getRight() + 1.0f, plateR.getRight() - 1.5f);
        if (step >= 2 && step <= 8) {
          g.setColour(juce::Colour(0xCC221A12));
          g.drawText(juce::String(step),
                     juce::Rectangle<float>(cx - 16.0f, ty - 5.0f, 8.0f, 10.0f),
                     juce::Justification::centredRight, false);
        }
      }

      // Slotted brass screws at top and bottom of steel channel
      drawBrassScrew(g, cx, plateR.getY() + 5.5f, 2.3f, 0.5f);
      drawBrassScrew(g, cx, plateR.getBottom() - 5.5f, 2.3f, -0.7f);
    }

    g.setColour(juce::Colour(0x777E613E));
    g.drawRoundedRectangle(bounds.reduced(0.5f), 4.0f, 1.1f);
  }

  void mouseMove(const juce::MouseEvent &e) override {
    const int c = colFromX(e.position.x);
    if (c != hoverCol) {
      hoverCol = c;
      repaint();
    }
  }

  void mouseExit(const juce::MouseEvent &) override {
    if (hoverCol >= 0) {
      hoverCol = -1;
      repaint();
    }
  }

  void mouseDown(const juce::MouseEvent &e) override {
    activeCol = colFromX(e.position.x);
    applyDragAt(e.position, e.mods.isShiftDown());
  }

  void mouseDrag(const juce::MouseEvent &e) override {
    const int col = colFromX(e.position.x);
    if (col >= 0)
      activeCol = col;
    applyDragAt(e.position, e.mods.isShiftDown());
  }

  void mouseUp(const juce::MouseEvent &) override {
    activeCol = -1;
    repaint();
  }

  void mouseDoubleClick(const juce::MouseEvent &e) override {
    const int col = colFromX(e.position.x);
    if (col < 0 || col >= 9)
      return;
    proc.setDrawbar(col, 0.0f);
    if (onAnyChanged)
      onAnyChanged();
    repaint();
  }

  void mouseWheelMove(const juce::MouseEvent &e,
                      const juce::MouseWheelDetails &wheel) override {
    const int col = colFromX(e.position.x);
    if (col < 0 || col >= 9)
      return;
    const int deltaSteps = wheel.deltaY > 0.0f ? -1 : (wheel.deltaY < 0.0f ? 1 : 0);
    if (deltaSteps == 0)
      return;
    const int cur = juce::jlimit(0, 8, (int)std::lround(proc.getDrawbar(col) * 8.0f));
    const int next = juce::jlimit(0, 8, cur + deltaSteps);
    proc.setDrawbar(col, (float)next / 8.0f);
    if (onAnyChanged)
      onAnyChanged();
    repaint();
  }

private:
  B3OrganProcessor &proc;
  int activeCol = -1;
  int hoverCol = -1;
  juce::Image cachedBackdrop;

  juce::Rectangle<float> trackRectForCol(juce::Rectangle<float> col) const {
    return juce::Rectangle<float>(col.getX(), col.getY() + 30.0f,
                                  col.getWidth(),
                                  juce::jmax(30.0f, col.getHeight() - 56.0f));
  }

  int colFromX(float x) const {
    const float w = (float)getWidth();
    if (w <= 0.0f)
      return -1;
    return juce::jlimit(0, 8, (int)(x / (w / 9.0f)));
  }

  void applyDragAt(juce::Point<float> p, bool fineContinuous) {
    if (activeCol < 0 || activeCol >= 9)
      return;
    const auto well = trackRectForCol(getLocalBounds().toFloat());
    constexpr float kCapH = 26.0f;
    const float travel = juce::jmax(10.0f, well.getHeight() - kCapH);
    const float rel = (p.y - (well.getY() + kCapH * 0.5f)) / travel;
    float norm = juce::jlimit(0.0f, 1.0f, rel);
    if (!fineContinuous)
      norm = std::round(norm * 8.0f) / 8.0f;
    proc.setDrawbar(activeCol, norm);
    if (onAnyChanged)
      onAnyChanged();
    repaint();
  }

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(HammondDrawbarStrip)
};

// Animated dual-rotor Leslie 122 display (Treble Horn + Bass Scoop Drum)
class LeslieRotorDisplay : public juce::Component {
public:
  explicit LeslieRotorDisplay(B3OrganProcessor &processor) : proc(processor) {}
  std::function<void()> onToggleSpeed;

  void paint(juce::Graphics &g) override {
    const auto b = getLocalBounds().toFloat();
    if (b.isEmpty())
      return;

    // Brushed dark bronze bezel around rotor cabinet cutout
    juce::ColourGradient cab(juce::Colour(0xFF2B211A), b.getX(), b.getY(),
                             juce::Colour(0xFF17110C), b.getRight(), b.getBottom(), false);
    g.setGradientFill(cab);
    g.fillRoundedRectangle(b, 4.0f);
    g.setColour(juce::Colour(0x777E6549));
    g.drawRoundedRectangle(b.reduced(0.5f), 4.0f, 1.0f);

    const bool bypassed = proc.leslieBypass.load(std::memory_order_relaxed);
    const int mode = proc.leslieMode.load(std::memory_order_relaxed);

    auto inner = b.reduced(5.0f);
    auto topLabel = inner.removeFromBottom(15.0f);

    const float radius = juce::jmax(13.0f, juce::jmin(inner.getWidth() * 0.44f,
                                                      inner.getHeight() * 0.25f));
    const float hornCy = inner.getY() + radius + 3.0f;
    const float drumCy = inner.getBottom() - radius - 3.0f;
    const float cx = inner.getCentreX();

    drawRotor(g, cx, hornCy, radius, proc.getHornAngle(),
              proc.getHornAngleB(), true, true, bypassed, mode);
    drawRotor(g, cx, drumCy, radius, -proc.getDrumAngle(),
              -proc.getDrumAngleB(), true, false, bypassed, mode);

    g.setFont(BoutiqueLookAndFeel::monoFont(9.5f, true));
    g.setColour(bypassed
                    ? juce::Colour(0xFF6B5D4D)
                    : (mode == 2 ? juce::Colour(0xFFF07B3A)
                                 : (mode == 1 ? juce::Colour(0xFFB39E84)
                                              : juce::Colour(0xFFE07D36))));
    const char *txt = bypassed
                          ? "BYPASS"
                          : (mode == 2 ? "2x TREM" : (mode == 1 ? "2x BRAKE" : "2x CHOR"));
    g.drawText(txt, topLabel, juce::Justification::centred, false);
  }

  void mouseDown(const juce::MouseEvent &) override {
    if (onToggleSpeed)
      onToggleSpeed();
  }

private:
  B3OrganProcessor &proc;

  static void drawRotor(juce::Graphics &g, float cx, float cy, float r,
                        float angle, float angleB, bool dual, bool isHorn,
                        bool bypassed, int mode) {
    const auto ring = juce::Rectangle<float>(cx - r, cy - r, r * 2.0f, r * 2.0f);
    // Deep recessed cylindrical chamber
    g.setColour(juce::Colour(0xFF100C09));
    g.fillEllipse(ring.expanded(1.5f));

    if (isHorn) {
      // Metallic brushed silver-aluminum conical treble horn chamber
      juce::ColourGradient coneG(juce::Colour(0xFFD5D7DA), cx - r * 0.45f, cy - r * 0.55f,
                                 juce::Colour(0xFF42464B), cx + r * 0.65f, cy + r * 0.65f, true);
      coneG.addColour(0.55, juce::Colour(0xFF8C9096));
      g.setGradientFill(coneG);
      g.fillEllipse(ring);
      g.setColour(juce::Colour(0x8835393E));
      g.drawEllipse(ring.reduced(r * 0.16f), 0.9f);
      g.setColour(juce::Colour(0x66FFFFFF));
      g.drawEllipse(ring, 0.9f);
    } else {
      // Warm varnished cherry/walnut wooden lower rotor drum
      juce::ColourGradient woodG(juce::Colour(0xFFA05B2B), cx - r * 0.45f, cy - r * 0.55f,
                                 juce::Colour(0xFF3E1D0A), cx + r * 0.7f, cy + r * 0.7f, true);
      woodG.addColour(0.55, juce::Colour(0xFF753F1B));
      g.setGradientFill(woodG);
      g.fillEllipse(ring);
      g.setColour(juce::Colour(0x66D69D63));
      g.drawEllipse(ring, 1.0f);
    }

    const float cosA = std::cos(angle);
    const float sinA = std::sin(angle);
    const juce::Colour accentCol = bypassed
                                        ? juce::Colour(0xFF6B5D4D)
                                        : (mode == 2 ? juce::Colour(0xFFF07B3A)
                                                     : juce::Colour(0xFFE6A962));

    if (dual && !bypassed) {
      const float cosB = std::cos(angleB);
      const float sinB = std::sin(angleB);
      const float bx = cx + cosB * (r * 0.66f);
      const float by = cy + sinB * (r * 0.66f);
      g.setColour(accentCol.withAlpha(0.32f));
      g.drawLine(cx, cy, bx, by, 1.8f);
      g.fillEllipse(bx - 2.6f, by - 2.6f, 5.2f, 5.2f);
    }

    if (isHorn) {
      // Dual chrome horn bells spinning across metallic dish
      const float hx = cx + cosA * (r * 0.72f);
      const float hy = cy + sinA * (r * 0.72f);
      const float tx = cx - cosA * (r * 0.62f);
      const float ty = cy - sinA * (r * 0.62f);
      g.setColour(juce::Colour(0xCC1E1F22));
      g.drawLine(tx + 0.8f, ty + 1.0f, hx + 0.8f, hy + 1.0f, 3.0f);
      g.setColour(juce::Colour(0xFFE0E2E5));
      g.drawLine(tx, ty, hx, hy, 2.2f);
      g.setColour(accentCol);
      g.fillEllipse(hx - 3.8f, hy - 3.8f, 7.6f, 7.6f);
      g.setColour(juce::Colour(0xFF24272A));
      g.fillEllipse(tx - 2.6f, ty - 2.6f, 5.2f, 5.2f);
    } else {
      // Rotating drum scoop pocket & brass indicator arm
      const float sx = cx + cosA * (r * 0.66f);
      const float sy = cy + sinA * (r * 0.66f);
      g.setColour(juce::Colour(0x55120601));
      g.fillEllipse(ring.reduced(r * 0.24f));
      g.setColour(accentCol);
      g.drawLine(cx, cy, sx, sy, 3.2f);
      g.fillEllipse(sx - 3.2f, sy - 3.2f, 6.4f, 6.4f);
    }

    g.setColour(juce::Colour(0xFFD5B06B));
    g.fillEllipse(cx - 2.4f, cy - 2.4f, 4.8f, 4.8f);
  }

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(LeslieRotorDisplay)
};

// 61-key Waterfall Organ Keyboard (C2..C7) with palm-smear glissando & QWERTY
class B3WaterfallKeyboard : public juce::Component, private juce::Timer {
public:
  explicit B3WaterfallKeyboard(B3OrganProcessor &processor) : proc(processor) {
    setWantsKeyboardFocus(true);

    int offset = 0;
    for (const char *row : {kRowLower, kRowUpper}) {
      for (int i = 0; row[i] != 0; ++i) {
        keyMap.push_back(
            {static_cast<int>(std::toupper(static_cast<unsigned char>(row[i]))),
             offset + i});
      }
      offset += 12;
    }

    octDownBtn.setButtonText("OCT -");
    octDownBtn.setTooltip("Shift QWERTY keyboard down an octave");
    octDownBtn.onClick = [this] { shiftOctave(-12); };
    addAndMakeVisible(octDownBtn);

    octUpBtn.setButtonText("OCT +");
    octUpBtn.setTooltip("Shift QWERTY keyboard up an octave");
    octUpBtn.onClick = [this] { shiftOctave(12); };
    addAndMakeVisible(octUpBtn);

    panicBtn.setButtonText("ALL OFF");
    Fanfare::setButtonRole(panicBtn, Fanfare::ButtonRole::Danger);
    panicBtn.setTooltip("Silence all held organ notes");
    panicBtn.onClick = [this] {
      releaseAll();
      proc.allNotesOff();
      repaint();
    };
    addAndMakeVisible(panicBtn);
  }

  ~B3WaterfallKeyboard() override {
    stopTimer();
    releaseAll();
  }

  void setKeyboardEnabled(bool enabled) {
    keyboardActive = enabled;
    if (enabled) {
      startTimerHz(20);
    } else {
      stopTimer();
      releaseAll();
    }
  }

  void shiftOctave(int semis) {
    releaseAll();
    qwertyBase = juce::jlimit(36, 72, qwertyBase + semis);
    repaint();
  }

  void releaseAllNotes() { releaseAll(); }

  bool handleKeyPressed(const juce::KeyPress &key) {
    if (!keyboardActive || isTextEditorFocused())
      return false;
    if (key.isKeyCode(juce::KeyPress::leftKey)) {
      shiftOctave(-12);
      return true;
    }
    if (key.isKeyCode(juce::KeyPress::rightKey)) {
      shiftOctave(12);
      return true;
    }
    const int code = normalizeCode(key.getKeyCode());
    for (const auto &km : keyMap) {
      if (km.first != code)
        continue;
      if (keyHeldByCode.find(km.first) == keyHeldByCode.end()) {
        const int n = juce::jlimit(kMinNote, kMaxNote, qwertyBase + km.second);
        keyHeldByCode[km.first] = n;
        proc.injectNote(n, true, 0.90f);
        repaint();
      }
      return true;
    }
    return false;
  }

  bool handleKeyStateChange() {
    if (!keyboardActive || isTextEditorFocused())
      return false;
    for (const auto &km : keyMap) {
      const bool down = juce::KeyPress::isKeyCurrentlyDown(km.first);
      auto it = keyHeldByCode.find(km.first);
      if (down && it == keyHeldByCode.end()) {
        const int n = juce::jlimit(kMinNote, kMaxNote, qwertyBase + km.second);
        keyHeldByCode[km.first] = n;
        proc.injectNote(n, true, 0.90f);
        repaint();
      } else if (!down && it != keyHeldByCode.end()) {
        proc.injectNote(it->second, false, 0.0f);
        keyHeldByCode.erase(it);
        repaint();
      }
    }
    return !keyHeldByCode.empty();
  }

  void paint(juce::Graphics &g) override {
    const auto b = getLocalBounds().toFloat();
    g.setColour(ThemeManager::get(Theme::Role::panel));
    g.fillRoundedRectangle(b, 4.0f);
    g.setColour(ThemeManager::get(Theme::Role::border));
    g.drawRoundedRectangle(b.reduced(0.5f), 4.0f, 1.0f);

    g.setColour(ThemeManager::get(Theme::Role::textDim));
    g.setFont(BoutiqueLookAndFeel::monoFont(10.0f, false));
    g.drawText("61-KEY WATERFALL MANUAL (C2 - C7)  |  QWERTY [" +
                   noteName(qwertyBase) + "-" + noteName(qwertyBase + 24) +
                   "]  |  Drag to Glissando",
               headerRect, juce::Justification::centredLeft, true);

    const auto keys = keysRect.toFloat();
    if (keys.isEmpty())
      return;

    // C2..C7 = 5 octaves + 1 top C = 36 white keys
    constexpr int kNumWhites = 36;
    const float wW = keys.getWidth() / (float)kNumWhites;

    // White waterfall keys
    for (int n = kMinNote; n <= kMaxNote; ++n) {
      if (isBlackPc(n % 12))
        continue;
      const int wi = whiteIndexOf(n - kMinNote);
      const auto r = juce::Rectangle<float>(
          keys.getX() + (float)wi * wW, keys.getY(), wW - 0.8f, keys.getHeight());
      const bool on = isNoteOn(n);
      g.setColour(on ? ThemeManager::get(Theme::Role::accent)
                     : juce::Colour(0xFFEBE5D8));
      g.fillRoundedRectangle(r, 1.8f);
      g.setColour(ThemeManager::get(Theme::Role::borderStrong));
      g.drawRoundedRectangle(r, 1.8f, 0.7f);

      if (n % 12 == 0) {
        g.setColour(on ? ThemeManager::get(Theme::Role::textOnAccent)
                       : juce::Colour(0xFF5A5448));
        g.setFont(BoutiqueLookAndFeel::monoFont(8.5f, true));
        g.drawText(noteName(n), r.toNearestInt().removeFromBottom(12),
                   juce::Justification::centred, false);
      }
      const char ch = charForOffset(n - qwertyBase);
      if (ch != 0) {
        g.setColour(on ? ThemeManager::get(Theme::Role::textOnAccent)
                       : juce::Colour(0xFF8A7A60));
        g.setFont(BoutiqueLookAndFeel::monoFont(8.5f, false));
        g.drawText(juce::String::charToString((juce::juce_wchar)std::toupper((unsigned char)ch)),
                   r.toNearestInt().removeFromBottom(23).removeFromTop(11),
                   juce::Justification::centred, false);
      }
    }

    // Black keys
    for (int n = kMinNote; n <= kMaxNote; ++n) {
      if (!isBlackPc(n % 12))
        continue;
      const int wi = whiteIndexOf(n - kMinNote);
      const float bx = keys.getX() + (float)wi * wW - wW * 0.30f;
      const auto r = juce::Rectangle<float>(bx, keys.getY(), wW * 0.60f,
                                            keys.getHeight() * 0.58f);
      const bool on = isNoteOn(n);
      g.setColour(on ? ThemeManager::get(Theme::Role::warn)
                     : juce::Colour(0xFF17191C));
      g.fillRoundedRectangle(r, 1.5f);
      g.setColour(juce::Colours::black);
      g.drawRoundedRectangle(r, 1.5f, 0.7f);
    }
  }

  void resized() override {
    auto b = getLocalBounds().reduced(6, 4);
    auto topBar = b.removeFromTop(18);
    panicBtn.setBounds(topBar.removeFromRight(62).reduced(1, 0));
    topBar.removeFromRight(4);
    octUpBtn.setBounds(topBar.removeFromRight(48).reduced(1, 0));
    topBar.removeFromRight(2);
    octDownBtn.setBounds(topBar.removeFromRight(48).reduced(1, 0));
    topBar.removeFromRight(6);
    headerRect = topBar;
    b.removeFromTop(3);
    keysRect = b;
  }

  void focusLost(juce::Component::FocusChangeType) override { releaseAll(); }
  bool keyPressed(const juce::KeyPress &key) override {
    return handleKeyPressed(key);
  }
  bool keyStateChanged(bool) override { return handleKeyStateChange(); }
  void timerCallback() override { handleKeyStateChange(); }

  void mouseDown(const juce::MouseEvent &e) override {
    grabKeyboardFocus();
    mouseNote = noteFromPos(e.position);
    if (mouseNote >= 0)
      proc.injectNote(mouseNote, true, 0.90f);
    repaint();
  }

  void mouseDrag(const juce::MouseEvent &e) override {
    const int n = noteFromPos(e.position);
    if (n != mouseNote) {
      if (mouseNote >= 0)
        proc.injectNote(mouseNote, false, 0.0f);
      mouseNote = n;
      if (n >= 0)
        proc.injectNote(n, true, 0.90f);
      repaint();
    }
  }

  void mouseUp(const juce::MouseEvent &) override {
    if (mouseNote >= 0) {
      proc.injectNote(mouseNote, false, 0.0f);
      mouseNote = -1;
      repaint();
    }
  }

private:
  static constexpr int kMinNote = 36; // C2
  static constexpr int kMaxNote = 96; // C7

  static bool isTextEditorFocused() {
    return dynamic_cast<juce::TextEditor *>(
               juce::Component::getCurrentlyFocusedComponent()) != nullptr;
  }
  static bool isBlackPc(int pc) noexcept {
    return pc == 1 || pc == 3 || pc == 6 || pc == 8 || pc == 10;
  }
  static int normalizeCode(int code) noexcept {
    return (code >= 'a' && code <= 'z') ? code - 32 : code;
  }
  static char charForOffset(int s) noexcept {
    if (s >= 0 && s < 12) return kRowLower[s];
    if (s >= 12 && s < 24) return kRowUpper[s - 12];
    return 0;
  }
  static int whiteIndexOf(int offset) noexcept {
    int w = 0;
    for (int i = 0; i < offset; ++i)
      if (!isBlackPc(i % 12))
        ++w;
    return w;
  }
  static juce::String noteName(int midi) {
    static constexpr const char *kNames[] = {
        "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    return juce::String(kNames[(midi % 12 + 12) % 12]) + juce::String(midi / 12 - 1);
  }

  bool isNoteOn(int n) const {
    if (n == mouseNote)
      return true;
    for (const auto &kv : keyHeldByCode)
      if (kv.second == n)
        return true;
    return false;
  }

  int noteFromPos(juce::Point<float> p) const {
    const auto keys = keysRect.toFloat();
    if (!keys.contains(p))
      return -1;
    constexpr int kNumWhites = 36;
    const float wW = keys.getWidth() / (float)kNumWhites;
    if (p.y < keys.getY() + keys.getHeight() * 0.58f) {
      for (int n = kMinNote; n <= kMaxNote; ++n) {
        if (!isBlackPc(n % 12))
          continue;
        const float bx = keys.getX() + (float)whiteIndexOf(n - kMinNote) * wW - wW * 0.30f;
        if (p.x >= bx && p.x < bx + wW * 0.60f)
          return n;
      }
    }
    const int wi = juce::jlimit(0, kNumWhites - 1, (int)((p.x - keys.getX()) / wW));
    int w = 0;
    for (int n = kMinNote; n <= kMaxNote; ++n) {
      if (!isBlackPc(n % 12)) {
        if (w == wi)
          return n;
        ++w;
      }
    }
    return -1;
  }

  void releaseAll() {
    for (const auto &kv : keyHeldByCode)
      proc.injectNote(kv.second, false, 0.0f);
    keyHeldByCode.clear();
    if (mouseNote >= 0) {
      proc.injectNote(mouseNote, false, 0.0f);
      mouseNote = -1;
    }
    repaint();
  }

  static inline constexpr const char *kRowLower = "zsxdcvgbhnjm";
  static inline constexpr const char *kRowUpper = "q2w3er5t6y7ui";

  B3OrganProcessor &proc;
  int qwertyBase = 48; // C3
  int mouseNote = -1;
  std::vector<std::pair<int, int>> keyMap;
  std::map<int, int> keyHeldByCode;
  juce::TextButton octDownBtn, octUpBtn, panicBtn;
  juce::Rectangle<int> headerRect, keysRect;
  bool keyboardActive = false;

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(B3WaterfallKeyboard)
};

class B3DrawbarMapDialog : public juce::Component, private juce::Timer {
public:
  B3DrawbarMapDialog(B3OrganProcessor &p, std::function<void()> onChanged)
      : proc(p), onMapsChanged(std::move(onChanged)) {
    static constexpr const char *kNames[9] = {
        "16'", "5 1/3'", "8'", "4'", "2 2/3'", "2'", "1 3/5'", "1 1/3'", "1'"};

    titleLbl.setText("HARDWARE DRAWBAR CC MAP", juce::dontSendNotification);
    titleLbl.setFont(BoutiqueLookAndFeel::condensedFont(14.0f, 0.06f));
    titleLbl.setColour(juce::Label::textColourId, juce::Colour(0xFFE9C37A));
    addAndMakeVisible(titleLbl);

    nameLbl.setText("Map Name:", juce::dontSendNotification);
    nameLbl.setFont(BoutiqueLookAndFeel::condensedFont(11.5f, 0.03f));
    addAndMakeVisible(nameLbl);

    nameEditor.setTextToShowWhenEmpty("e.g. Keylab 88", juce::Colours::grey);
    addAndMakeVisible(nameEditor);

    learnAllBtn.setButtonText("LEARN ALL (16' -> 1')");
    learnAllBtn.setClickingTogglesState(true);
    learnAllBtn.setTooltip("Move your 9 hardware faders in order from 16' to 1'");
    learnAllBtn.onClick = [this] {
      if (learnAllBtn.getToggleState()) {
        autoAdvance = true;
        startListening(0);
      } else {
        stopListening();
      }
    };
    addAndMakeVisible(learnAllBtn);

    invertToggle.setButtonText("INV (Pull = 8)");
    invertToggle.setTooltip("Invert faders for this controller (saved inside map)");
    addAndMakeVisible(invertToggle);

    for (int i = 0; i < 9; ++i) {
      rowLabels[(size_t)i].setText(kNames[i], juce::dontSendNotification);
      rowLabels[(size_t)i].setFont(BoutiqueLookAndFeel::monoFont(12.0f, true));
      addAndMakeVisible(rowLabels[(size_t)i]);

      ccReadouts[(size_t)i].setJustificationType(juce::Justification::centred);
      ccReadouts[(size_t)i].setFont(BoutiqueLookAndFeel::monoFont(11.5f, false));
      ccReadouts[(size_t)i].setColour(juce::Label::backgroundColourId,
                                      ThemeManager::get(Theme::Role::sunken));
      addAndMakeVisible(ccReadouts[(size_t)i]);

      learnBtns[(size_t)i].setButtonText("LEARN");
      learnBtns[(size_t)i].onClick = [this, i] {
        autoAdvance = false;
        learnAllBtn.setToggleState(false, juce::dontSendNotification);
        if (listeningIdx == i)
          stopListening();
        else
          startListening(i);
      };
      addAndMakeVisible(learnBtns[(size_t)i]);

      clrBtns[(size_t)i].setButtonText("X");
      clrBtns[(size_t)i].onClick = [this, i] {
        working.ccs[(size_t)i] = -1;
        refreshRows();
      };
      addAndMakeVisible(clrBtns[(size_t)i]);
    }

    saveBtn.setButtonText("SAVE & USE");
    Fanfare::setButtonRole(saveBtn, Fanfare::ButtonRole::Primary);
    saveBtn.onClick = [this] {
      working.name = nameEditor.getText().trim();
      if (working.name.isEmpty())
        working.name = "Custom Map";
      working.invert = invertToggle.getToggleState();
      if (proc.saveOrUpdateCustomCcMap(working)) {
        if (onMapsChanged)
          onMapsChanged();
        if (auto *box = findParentComponentOfClass<juce::CallOutBox>())
          box->dismiss();
      }
    };
    addAndMakeVisible(saveBtn);

    delBtn.setButtonText("DELETE");
    delBtn.onClick = [this] {
      const auto n = nameEditor.getText().trim();
      if (n.isNotEmpty() && proc.deleteCustomCcMap(n)) {
        if (onMapsChanged)
          onMapsChanged();
        if (auto *box = findParentComponentOfClass<juce::CallOutBox>())
          box->dismiss();
      }
    };
    addAndMakeVisible(delBtn);

    // Preload active custom map if selected
    if (proc.ccProfile.load() == B3OrganProcessor::CcCustom &&
        proc.getCustomCcMapName().isNotEmpty()) {
      for (const auto &m : proc.getCustomCcMaps()) {
        if (m.name.equalsIgnoreCase(proc.getCustomCcMapName())) {
          working = m;
          break;
        }
      }
    } else {
      working.invert = proc.invertDrawbars.load();
    }
    nameEditor.setText(working.name, juce::dontSendNotification);
    invertToggle.setToggleState(working.invert, juce::dontSendNotification);
    delBtn.setEnabled(working.name.isNotEmpty());
    refreshRows();

    lastSeq = proc.getLastCcSeq();
    startTimerHz(25);
    setSize(340, 386);
  }

  ~B3DrawbarMapDialog() override { stopTimer(); }

  void paint(juce::Graphics &g) override {
    g.fillAll(juce::Colour(0xFF1E140E));
  }

  void resized() override {
    auto r = getLocalBounds().reduced(10);
    titleLbl.setBounds(r.removeFromTop(22));
    r.removeFromTop(6);

    auto nameRow = r.removeFromTop(24);
    nameLbl.setBounds(nameRow.removeFromLeft(72));
    nameEditor.setBounds(nameRow);
    r.removeFromTop(6);

    auto topCtrl = r.removeFromTop(24);
    learnAllBtn.setBounds(topCtrl.removeFromLeft(175));
    topCtrl.removeFromLeft(8);
    invertToggle.setBounds(topCtrl);
    r.removeFromTop(8);

    for (int i = 0; i < 9; ++i) {
      auto row = r.removeFromTop(24);
      rowLabels[(size_t)i].setBounds(row.removeFromLeft(56));
      ccReadouts[(size_t)i].setBounds(row.removeFromLeft(88).reduced(2, 1));
      row.removeFromLeft(6);
      clrBtns[(size_t)i].setBounds(row.removeFromRight(28).reduced(1, 1));
      learnBtns[(size_t)i].setBounds(row.reduced(2, 1));
      r.removeFromTop(3);
    }

    r.removeFromTop(6);
    auto bot = r.removeFromTop(26);
    delBtn.setBounds(bot.removeFromLeft(80));
    saveBtn.setBounds(bot.removeFromRight(110));
  }

private:
  void startListening(int idx) {
    listeningIdx = idx;
    lastSeq = proc.getLastCcSeq();
    lastAutoCc = -1;
    lastAutoCaptureMs = 0;
    refreshRows();
  }

  void stopListening() {
    listeningIdx = -1;
    autoAdvance = false;
    learnAllBtn.setToggleState(false, juce::dontSendNotification);
    refreshRows();
  }

  void refreshRows() {
    for (int i = 0; i < 9; ++i) {
      const int cc = working.ccs[(size_t)i];
      ccReadouts[(size_t)i].setText(
          cc >= 0 ? ("CC " + juce::String(cc)) : "--",
          juce::dontSendNotification);
      const bool active = (listeningIdx == i);
      learnBtns[(size_t)i].setButtonText(active ? "MOVE FADER..." : "LEARN");
      learnBtns[(size_t)i].setColour(
          juce::TextButton::buttonColourId,
          active ? ThemeManager::get(Theme::Role::warn)
                 : ThemeManager::get(Theme::Role::panel));
    }
  }

  void timerCallback() override {
    if (listeningIdx < 0 || listeningIdx >= 9)
      return;
    const uint32_t seq = proc.getLastCcSeq();
    if (seq == lastSeq)
      return;
    lastSeq = seq;
    const int cc = proc.getLastCcNum();
    if (cc < 0 || cc > 127)
      return;

    const juce::uint32 now = juce::Time::getMillisecondCounter();
    if (autoAdvance && cc == lastAutoCc && (now - lastAutoCaptureMs) < 900)
      return; // Ignore continuous stream from the fader we just captured

    working.ccs[(size_t)listeningIdx] = cc;
    lastAutoCc = cc;
    lastAutoCaptureMs = now;

    if (autoAdvance) {
      if (listeningIdx < 8)
        ++listeningIdx;
      else
        stopListening();
    } else {
      stopListening();
    }
    refreshRows();
  }

  B3OrganProcessor &proc;
  std::function<void()> onMapsChanged;
  B3OrganProcessor::DrawbarCcMap working;
  int listeningIdx = -1;
  bool autoAdvance = false;
  uint32_t lastSeq = 0;
  int lastAutoCc = -1;
  juce::uint32 lastAutoCaptureMs = 0;

  juce::Label titleLbl, nameLbl;
  juce::TextEditor nameEditor;
  juce::TextButton learnAllBtn, saveBtn, delBtn;
  juce::ToggleButton invertToggle;
  std::array<juce::Label, 9> rowLabels, ccReadouts;
  std::array<juce::TextButton, 9> learnBtns, clrBtns;
};

} // namespace FanfareB3::Ui

class B3OrganComponent : public juce::Component,
                         private juce::Timer {
public:
  // ---- Centered controls: the 12 o'clock / midpoint position equals the voiced
  // default value; the upper half extends beyond it for extra range. ----
  static constexpr double kDriveC = 0.86, kDriveMax = 1.72;
  static constexpr double kToneC = 0.88, kClickC = 0.90;
  static constexpr double kLeakC = 0.033, kLeakMax = 0.066;
  static constexpr double kVolC = 0.60;
  static constexpr double kRoomC = 0.84, kSpreadC = 0.85;
  static constexpr double kChorusC = 0.40, kWarmC = 0.80;
  static constexpr double kDelayC = 0.22, kDelayMax = 0.80;

  static double centeredToValue(double p, double c, double mx) {
    p = juce::jlimit(0.0, 1.0, p);
    return p <= 0.5 ? p * 2.0 * c : c + (p - 0.5) * 2.0 * (mx - c);
  }
  static double valueToCentered(double v, double c, double mx) {
    if (v <= c)
      return c > 1.0e-9 ? 0.5 * juce::jmax(0.0, v) / c : 0.0;
    return juce::jmin(1.0, 0.5 + 0.5 * (v - c) / juce::jmax(1.0e-9, mx - c));
  }
  static void setCenteredControl(juce::Slider &s, double c, double mx) {
    s.setRange(0.0, 1.0, 0.005);
    s.setDoubleClickReturnValue(true, 0.5);
    s.textFromValueFunction = [c, mx](double p) {
      return juce::String(centeredToValue(p, c, mx), 2);
    };
  }

  static juce::Rectangle<int>
  computeOverlayBoundsForHost(juce::Rectangle<int> host) noexcept {
    const int availW = juce::jmax(360, host.getWidth() - 20);
    const int availH = juce::jmax(340, host.getHeight() - 20);
    const int w = juce::jmin(availW, juce::jlimit(840, 1220, host.getWidth() * 94 / 100));
    const int h = juce::jmin(availH, juce::jlimit(540, 760, host.getHeight() * 90 / 100));
    return host.withSizeKeepingCentre(w, h);
  }

  void updateForHostBounds(juce::Rectangle<int> host) {
    if (userDraggedCorner && getWidth() > 0 && getHeight() > 0) {
      const int maxW = juce::jmax(360, host.getWidth() - 16);
      const int maxH = juce::jmax(340, host.getHeight() - 16);
      applyingHostBounds = true;
      setBounds(host.withSizeKeepingCentre(juce::jmin(getWidth(), maxW),
                                           juce::jmin(getHeight(), maxH)));
      applyingHostBounds = false;
      return;
    }
    applyingHostBounds = true;
    setBounds(computeOverlayBoundsForHost(host));
    applyingHostBounds = false;
  }

  std::function<void()> onOpenNoteRange;
  std::function<void()> onOpenCCDialog;
  std::function<void()> onOpenMidiEffects;
  std::function<void(int extSlot, const juce::Rectangle<int>& anchor)> onPickExtPlugin;
  std::function<void(int extSlot)> onOpenExtEditor;

  void refreshExtPluginLabels() {
    for (int e = 0; e < B3OrganProcessor::kNumExtSlots; ++e) {
      const juce::String name = proc.getExtPluginName(e);
      const bool hasPlug = name.isNotEmpty();
      extPickBtn[e].setButtonText(hasPlug ? name : "+ Load VST3...");
      extEditBtn[e].setEnabled(hasPlug);
      extBypBtn[e].setEnabled(hasPlug);
      extBypBtn[e].setToggleState(proc.isExtPluginBypassed(e),
                                  juce::dontSendNotification);
    }
  }

  juce::Rectangle<int> getMidiButtonScreenBounds() const {
    return midiBtn.getScreenBounds();
  }

  B3OrganComponent(RackSlot &slot, std::function<void()> onClose,
                   int chainIdx = 0)
      : slotRef(slot), chainIndex(juce::jlimit(0, 2, chainIdx)),
        proc(slot.getB3Organ(chainIndex)), closeCallback(std::move(onClose)),
        drawbarStrip(proc), scannerDial(proc), leslieDisplay(proc), waterfallKeys(proc) {
    setLookAndFeel(&vintageLaf);
    setWantsKeyboardFocus(true);

    // ---- Header row ----
    title.setText("FANFARE B3 ORGAN", juce::dontSendNotification);
    title.setFont(BoutiqueLookAndFeel::condensedFont(16.0f, 0.08f));
    title.setColour(juce::Label::textColourId, juce::Colour(0xFFE9C37A));
    addAndMakeVisible(title);

    regBadge.setJustificationType(juce::Justification::centred);
    regBadge.setFont(BoutiqueLookAndFeel::monoFont(12.0f, true));
    regBadge.setColour(juce::Label::backgroundColourId,
                       ThemeManager::get(Theme::Role::sunken));
    regBadge.setColour(juce::Label::textColourId,
                       ThemeManager::get(Theme::Role::warn));
    addAndMakeVisible(regBadge);

    prevPresetBtn.setButtonText("<");
    prevPresetBtn.onClick = [this] { stepPreset(-1); };
    addAndMakeVisible(prevPresetBtn);

    presetBox.setTextWhenNothingSelected("Select Registration...");
    presetBox.onChange = [this] {
      const int idx = presetBox.getSelectedId() - 1;
      proc.loadPresetAtIndex(idx);
      syncFromProcessor();
    };
    addAndMakeVisible(presetBox);

    nextPresetBtn.setButtonText(">");
    nextPresetBtn.onClick = [this] { stepPreset(1); };
    addAndMakeVisible(nextPresetBtn);

    saveBtn.setButtonText("SAVE");
    Fanfare::setButtonRole(saveBtn, Fanfare::ButtonRole::Standard);
    saveBtn.onClick = [this] { promptSavePreset(); };
    addAndMakeVisible(saveBtn);

    initBtn.setButtonText("Init");
    Fanfare::setButtonRole(initBtn, Fanfare::ButtonRole::Standard);
    initBtn.onClick = [this] {
      proc.loadPresetByName("vibe coda");
      syncFromProcessor();
    };
    addAndMakeVisible(initBtn);

    ccProfileBox.setTooltip("Hardware drawbar MIDI CC bank mapping (select or learn custom controller map)");
    ccProfileBox.onChange = [this] {
      const int id = ccProfileBox.getSelectedId();
      if (id == 999) {
        syncCcProfileBoxSelection();
        showDrawbarMapDialog();
      } else if (id >= 100) {
        const auto &maps = proc.getCustomCcMaps();
        const int idx = id - 100;
        if (idx >= 0 && idx < (int)maps.size()) {
          proc.selectCustomCcMap(maps[(size_t)idx].name);
          invertToggle.setToggleState(proc.invertDrawbars.load(),
                                      juce::dontSendNotification);
        }
      } else if (id >= 1 && id <= 5) {
        proc.setCcProfile(id - 1);
      }
    };
    rebuildCcProfileBox();
    addAndMakeVisible(ccProfileBox);

    invertToggle.setButtonText("INV CC");
    invertToggle.setTooltip("Invert incoming hardware drawbar CCs (pull down = 8)");
    invertToggle.onClick = [this] {
      proc.setInvertDrawbars(invertToggle.getToggleState());
    };
    addAndMakeVisible(invertToggle);

    octaveUpToggle.setButtonText("+12");
    octaveUpToggle.setTooltip("Transpose incoming MIDI & keyboard +1 octave (+12 semitones)");
    octaveUpToggle.onClick = [this] {
      proc.setOctaveUp(octaveUpToggle.getToggleState());
    };
    addAndMakeVisible(octaveUpToggle);

    midiBtn.setButtonText("MIDI");
    Fanfare::setButtonRole(midiBtn, Fanfare::ButtonRole::Standard);
    midiBtn.setTooltip("Note ranges, MIDI CC learn & MIDI Transformer");
    midiBtn.onClick = [this] { showMidiRoutingDialog(); };
    addAndMakeVisible(midiBtn);

    keysToggle.setButtonText("KEYS");
    keysToggle.setToggleState(false, juce::dontSendNotification);
    keysToggle.setTooltip("Show/hide onscreen 61-key waterfall manual");
    keysToggle.onClick = [this] {
      showKeyboard = keysToggle.getToggleState();
      waterfallKeys.setKeyboardEnabled(showKeyboard);
      waterfallKeys.setVisible(showKeyboard);
      resized();
      repaint();
    };
    addAndMakeVisible(keysToggle);

    closeBtn.setButtonText("X");
    Fanfare::setButtonRole(closeBtn, Fanfare::ButtonRole::Standard);
    closeBtn.onClick = [this] {
      if (closeCallback)
        closeCallback();
    };
    addAndMakeVisible(closeBtn);

    // ---- Drawbars ----
    drawbarStrip.onAnyChanged = [this] { refreshRegistrationBadge(); };
    addAndMakeVisible(drawbarStrip);

    // ---- Percussion & Scanner ----
    percOnToggle.setButtonText("Perc  ON");
    percOnToggle.setComponentID("rocker");
    percOnToggle.onClick = [this] { proc.setPercOn(percOnToggle.getToggleState()); drawbarStrip.repaint(); };
    addAndMakeVisible(percOnToggle);

    percSoftToggle.setButtonText("Soft");
    percSoftToggle.setComponentID("rocker");
    percSoftToggle.setTooltip("Soft vs Normal percussion volume level");
    percSoftToggle.onClick = [this] { proc.setPercSoft(percSoftToggle.getToggleState()); };
    addAndMakeVisible(percSoftToggle);

    percFastToggle.setButtonText("Fast decay");
    percFastToggle.setComponentID("rocker");
    percFastToggle.setTooltip("Fast (~200 ms) vs Slow (~550 ms) percussion decay");
    percFastToggle.onClick = [this] { proc.setPercFast(percFastToggle.getToggleState()); };
    addAndMakeVisible(percFastToggle);

    percThirdToggle.setButtonText("3RD harm");
    percThirdToggle.setComponentID("bat");
    percThirdToggle.setTooltip("3rd harmonic (2-2/3') vs 2nd harmonic (4')");
    percThirdToggle.onClick = [this] { proc.setPercThird(percThirdToggle.getToggleState()); };
    addAndMakeVisible(percThirdToggle);

    for (int i = 0; i < 7; ++i) {
      scannerButtons[(size_t)i] = std::make_unique<juce::ToggleButton>(
          i == 0 ? "OFF"
                 : (i <= 3 ? juce::String("V") + juce::String(i)
                           : juce::String("C") + juce::String(i - 3)));
      scannerButtons[(size_t)i]->setRadioGroupId(1001);
      scannerButtons[(size_t)i]->onClick = [this, i] {
        proc.setScannerMode(i);
        scannerDial.repaint();
      };
      addAndMakeVisible(*scannerButtons[(size_t)i]);
    }

    scannerDial.onModeChanged = [this] {
      const int sm = proc.scannerMode.load(std::memory_order_relaxed);
      for (int i = 0; i < 7; ++i)
        scannerButtons[(size_t)i]->setToggleState(sm == i, juce::dontSendNotification);
    };
    addAndMakeVisible(scannerDial);

    scannerDepthLabel.setText("V/C WARBLE", juce::dontSendNotification);
    scannerDepthLabel.setFont(BoutiqueLookAndFeel::condensedFont(11.0f, 0.04f));
    scannerDepthLabel.setColour(juce::Label::textColourId, juce::Colour(0xFFD5C4A8));
    addAndMakeVisible(scannerDepthLabel);

    scannerDepthSlider.setSliderStyle(juce::Slider::LinearHorizontal);
    scannerDepthSlider.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    scannerDepthSlider.setRange(0.0, 1.0, 0.01);
    scannerDepthSlider.setDoubleClickReturnValue(true, 0.28);
    scannerDepthSlider.setPopupDisplayEnabled(true, true, this);
    scannerDepthSlider.setTooltip("Scanner Vibrato/Chorus modulation depth (0 = static phase richness, 1 = full sweep)");
    scannerDepthSlider.onValueChange = [this] {
      proc.setScannerDepth((float)scannerDepthSlider.getValue());
    };
    addAndMakeVisible(scannerDepthSlider);

    // ---- Preamp knobs ----
    auto setupKnob = [this](juce::Slider &k, juce::Label &lbl, const juce::String &name,
                            double mn, double mx, double step) {
      k.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
      k.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
      k.setRange(mn, mx, step);
      k.setPopupDisplayEnabled(true, true, this);
      addAndMakeVisible(k);

      lbl.setText(name, juce::dontSendNotification);
      lbl.setJustificationType(juce::Justification::centred);
      lbl.setFont(BoutiqueLookAndFeel::condensedFont(11.0f, 0.04f));
      lbl.setColour(juce::Label::textColourId, juce::Colour(0xFFD8C9AE));
      addAndMakeVisible(lbl);
    };

    setupKnob(driveKnob, driveLabel, "DRIVE", 0.0, 1.0, 0.005);
    setCenteredControl(driveKnob, kDriveC, kDriveMax);
    driveKnob.onValueChange = [this] {
      proc.setDrive((float)centeredToValue(driveKnob.getValue(), kDriveC, kDriveMax));
    };

    setupKnob(toneKnob, toneLabel, "TONE", 0.0, 1.0, 0.005);
    setCenteredControl(toneKnob, kToneC, 1.0);
    toneKnob.onValueChange = [this] {
      proc.setTone((float)centeredToValue(toneKnob.getValue(), kToneC, 1.0));
    };

    setupKnob(clickKnob, clickLabel, "CLICK", 0.0, 1.0, 0.005);
    setCenteredControl(clickKnob, kClickC, 1.0);
    clickKnob.onValueChange = [this] {
      proc.setClickDepth((float)centeredToValue(clickKnob.getValue(), kClickC, 1.0));
    };

    setupKnob(leakKnob, leakLabel, "LEAK", 0.0, 1.0, 0.005);
    setCenteredControl(leakKnob, kLeakC, kLeakMax);
    leakKnob.onValueChange = [this] {
      proc.setLeakage((float)centeredToValue(leakKnob.getValue(), kLeakC, kLeakMax));
    };

    setupKnob(volKnob, volLabel, "VOLUME", 0.0, 1.0, 0.005);
    setCenteredControl(volKnob, kVolC, 1.0);
    volKnob.onValueChange = [this] {
      proc.setVolume((float)centeredToValue(volKnob.getValue(), kVolC, 1.0));
    };

    swellLabel.setText("SWELL", juce::dontSendNotification);
    swellLabel.setFont(BoutiqueLookAndFeel::condensedFont(11.0f, 0.04f));
    swellLabel.setColour(juce::Label::textColourId, juce::Colour(0xFFD8C9AE));
    addAndMakeVisible(swellLabel);

    swellSlider.setSliderStyle(juce::Slider::LinearHorizontal);
    swellSlider.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    swellSlider.setRange(0.0, 1.0, 0.01);
    swellSlider.setPopupDisplayEnabled(true, true, this);
    swellSlider.onValueChange = [this] { proc.setSwell((float)swellSlider.getValue()); };
    addAndMakeVisible(swellSlider);

    // ---- Leslie 122 ----
    leslieDisplay.onToggleSpeed = [this] {
      const int nextMode = (proc.leslieMode.load() == 2) ? 0 : 2;
      proc.setLeslieMode(nextMode);
      syncLeslieButtons();
    };
    addAndMakeVisible(leslieDisplay);

    leslieSlowBtn.setButtonText("Slow");
    leslieSlowBtn.setRadioGroupId(2001);
    leslieSlowBtn.onClick = [this] { proc.setLeslieMode(0); };
    addAndMakeVisible(leslieSlowBtn);

    leslieBrakeBtn.setButtonText("Brake");
    leslieBrakeBtn.setComponentID("bat");
    leslieBrakeBtn.setRadioGroupId(2001);
    leslieBrakeBtn.onClick = [this] { proc.setLeslieMode(1); };
    addAndMakeVisible(leslieBrakeBtn);

    leslieFastBtn.setButtonText("Fast");
    leslieFastBtn.setComponentID("bat");
    leslieFastBtn.setRadioGroupId(2001);
    leslieFastBtn.onClick = [this] { proc.setLeslieMode(2); };
    addAndMakeVisible(leslieFastBtn);

    leslieBypassToggle.setButtonText("Byp");
    leslieBypassToggle.setTooltip("Bypass Dual Leslie 122 rotary cabinets");
    leslieBypassToggle.onClick = [this] {
      proc.setLeslieBypass(leslieBypassToggle.getToggleState());
    };
    addAndMakeVisible(leslieBypassToggle);

    cc64LeslieToggle.setButtonText("CC64->FAST");
    cc64LeslieToggle.setTooltip("When ON, Sustain Pedal (CC64) switches Leslie Slow/Fast instead of sustaining notes");
    cc64LeslieToggle.onClick = [this] {
      proc.setSustainControlsLeslie(cc64LeslieToggle.getToggleState());
    };
    addAndMakeVisible(cc64LeslieToggle);

    // Leslie cabinet is fixed (147 Closed voicing); no cabinet selector.

    roomMixLabel.setText("ROOM", juce::dontSendNotification);
    roomMixLabel.setFont(BoutiqueLookAndFeel::condensedFont(11.0f, 0.04f));
    roomMixLabel.setColour(juce::Label::textColourId, juce::Colour(0xFFD8C9AE));
    addAndMakeVisible(roomMixLabel);

    roomMixSlider.setSliderStyle(juce::Slider::LinearHorizontal);
    roomMixSlider.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    roomMixSlider.setRange(0.0, 1.0, 0.01);
    roomMixSlider.setDoubleClickReturnValue(true, 0.32);
    roomMixSlider.setPopupDisplayEnabled(true, true, this);
    roomMixSlider.setTooltip("Tight wood room reflections vs close Leslie cabinet mics");
    roomMixSlider.onValueChange = [this] {
      proc.setRoomMix((float)roomMixSlider.getValue());
    };
    addAndMakeVisible(roomMixSlider);

    micSpreadLabel.setText("SPREAD", juce::dontSendNotification);
    micSpreadLabel.setFont(BoutiqueLookAndFeel::condensedFont(11.0f, 0.04f));
    micSpreadLabel.setColour(juce::Label::textColourId, juce::Colour(0xFFD8C9AE));
    addAndMakeVisible(micSpreadLabel);

    micSpreadSlider.setSliderStyle(juce::Slider::LinearHorizontal);
    micSpreadSlider.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    micSpreadSlider.setRange(0.0, 1.0, 0.01);
    micSpreadSlider.setPopupDisplayEnabled(true, true, this);
    micSpreadSlider.onValueChange = [this] {
      proc.setMicSpread((float)micSpreadSlider.getValue());
    };
    addAndMakeVisible(micSpreadSlider);

    auto initLeslieSlider = [this](juce::Slider &s, juce::Label &l, const char *txt,
                                   double def, const char *tip,
                                   std::function<void(float)> fn) {
      l.setText(txt, juce::dontSendNotification);
      l.setFont(BoutiqueLookAndFeel::condensedFont(11.0f, 0.04f));
      l.setColour(juce::Label::textColourId, juce::Colour(0xFFD8C9AE));
      addAndMakeVisible(l);
      s.setSliderStyle(juce::Slider::LinearHorizontal);
      s.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
      s.setRange(0.0, 1.0, 0.01);
      s.setDoubleClickReturnValue(true, def);
      s.setPopupDisplayEnabled(true, true, this);
      s.setTooltip(tip);
      s.onValueChange = [&s, fn] { fn((float)s.getValue()); };
      addAndMakeVisible(s);
    };
    initLeslieSlider(leslieChorusSlider, leslieChorusLabel, "CHORUS", 0.35,
                     "Gentle Dimension-style stereo chorus for thickness and width",
                     [this](float v) { proc.setLeslieChorus(v); });
    initLeslieSlider(leslieWarmthSlider, leslieWarmthLabel, "WARMTH", 0.55,
                     "Low-mid body & woody warmth",
                     [this](float v) { proc.setLeslieWarmth(v); });

    // Midpoint = voiced default; upper half adds extra range
    setCenteredControl(roomMixSlider, kRoomC, 1.0);
    roomMixSlider.onValueChange = [this] {
      proc.setRoomMix((float)centeredToValue(roomMixSlider.getValue(), kRoomC, 1.0));
    };
    setCenteredControl(micSpreadSlider, kSpreadC, 1.0);
    micSpreadSlider.onValueChange = [this] {
      proc.setMicSpread((float)centeredToValue(micSpreadSlider.getValue(), kSpreadC, 1.0));
    };
    setCenteredControl(leslieChorusSlider, kChorusC, 1.0);
    leslieChorusSlider.onValueChange = [this] {
      proc.setLeslieChorus((float)centeredToValue(leslieChorusSlider.getValue(), kChorusC, 1.0));
    };
    setCenteredControl(leslieWarmthSlider, kWarmC, 1.0);
    leslieWarmthSlider.onValueChange = [this] {
      proc.setLeslieWarmth((float)centeredToValue(leslieWarmthSlider.getValue(), kWarmC, 1.0));
    };

    // ---- Studio FX (3-Band EQ, Opto Comp, Reverb) ----
    auto initFxKnob = [&](juce::Slider &s, juce::Label &l, const char *txt,
                          double minV, double maxV, double defV, double stepV,
                          std::function<void(float)> onChange) {
      s.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
      s.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
      s.setRange(minV, maxV, stepV);
      s.setDoubleClickReturnValue(true, defV);
      s.setPopupDisplayEnabled(true, true, this);
      s.onValueChange = [this, &s, onChange] { onChange((float)s.getValue()); };
      addAndMakeVisible(s);

      l.setText(txt, juce::dontSendNotification);
      l.setJustificationType(juce::Justification::centred);
      l.setFont(BoutiqueLookAndFeel::condensedFont(10.0f, 0.04f));
      l.setColour(juce::Label::textColourId, juce::Colour(0xFFD8C9AE));
      addAndMakeVisible(l);
    };
    initFxKnob(eqLowKnob,  eqLowLabel,  "EQ LOW",  -9.0, 9.0,  1.5, 0.1,
               [this](float v) { proc.setEqLowDb(v); });
    initFxKnob(eqMidKnob,  eqMidLabel,  "EQ MID",  -9.0, 9.0,  0.0, 0.1,
               [this](float v) { proc.setEqMidDb(v); });
    initFxKnob(eqHighKnob, eqHighLabel, "EQ HIGH", -9.0, 9.0, -0.5, 0.1,
               [this](float v) { proc.setEqHighDb(v); });
    initFxKnob(compKnob,   compLabel,   "COMP",     0.0, 1.0,  0.28, 0.01,
               [this](float v) { proc.setCompAmount(v); });
    initFxKnob(revKnob,    revLabel,    "REVERB",   0.0, 1.0,  0.20, 0.01,
               [this](float v) { proc.setReverbMix(v); });
    initFxKnob(delayKnob,  delayLabel,  "DELAY",    0.0, 1.0,  0.50, 0.005,
               [this](float v) { proc.setDelayMix((float)centeredToValue(v, kDelayC, kDelayMax)); });
    setCenteredControl(delayKnob, kDelayC, kDelayMax);
    delayKnob.setTooltip("Ping-Pong Delay (301 ms, 26% feedback, 600 Hz LPF) — 12 o'clock = 22%");

    // ---- External VST3 Insert Slots (Saturation/Comp + External Leslie) ----
    for (int e = 0; e < B3OrganProcessor::kNumExtSlots; ++e) {
      extSlotLabel[e].setText(e == 0 ? "SAT VST" : "LESLIE VST",
                              juce::dontSendNotification);
      extSlotLabel[e].setFont(BoutiqueLookAndFeel::condensedFont(10.5f, 0.04f));
      extSlotLabel[e].setColour(juce::Label::textColourId,
                                juce::Colour(0xFFD8C9AE));
      addAndMakeVisible(extSlotLabel[e]);

      extPickBtn[e].setTooltip(e == 0
          ? "Load external Saturation / Compressor VST3 (pre-Leslie)"
          : "Load external Rotary Speaker VST3 (replaces native Leslie when active)");
      extPickBtn[e].onClick = [this, e] {
        if (onPickExtPlugin)
          onPickExtPlugin(e, extPickBtn[e].getScreenBounds());
      };
      addAndMakeVisible(extPickBtn[e]);

      extEditBtn[e].setButtonText("EDIT");
      extEditBtn[e].onClick = [this, e] {
        if (onOpenExtEditor)
          onOpenExtEditor(e);
      };
      addAndMakeVisible(extEditBtn[e]);

      extBypBtn[e].setButtonText("BYP");
      extBypBtn[e].onClick = [this, e] {
        proc.setExtPluginBypassed(e, extBypBtn[e].getToggleState());
      };
      addAndMakeVisible(extBypBtn[e]);
    }

    // ---- Onscreen 61-key manual ----
    waterfallKeys.setKeyboardEnabled(false);
    addChildComponent(waterfallKeys);

    resizeLimits.setSizeLimits(760, 480, 1400, 900);
    resizer = std::make_unique<juce::ResizableCornerComponent>(this, &resizeLimits);
    addAndMakeVisible(*resizer);

    proc.refreshPresetCatalog();
    rebuildPresetList();
    syncFromProcessor();
    startTimerHz(30);
  }

  ~B3OrganComponent() override {
    stopTimer();
    waterfallKeys.releaseAllNotes();
    setLookAndFeel(nullptr);
  }

  void visibilityChanged() override {
    if (isVisible() && showKeyboard)
      waterfallKeys.grabKeyboardFocus();
  }

  bool keyPressed(const juce::KeyPress &key) override {
    return showKeyboard ? waterfallKeys.handleKeyPressed(key) : false;
  }

  bool keyStateChanged(bool) override {
    return showKeyboard ? waterfallKeys.handleKeyStateChange() : false;
  }

  void paint(juce::Graphics &g) override {
    const auto bounds = getLocalBounds().toFloat();
    const int w = getWidth();
    const int h = getHeight();
    if (w <= 0 || h <= 0)
      return;

    if (!cachedBgImage.isValid() || cachedBgImage.getWidth() != w ||
        cachedBgImage.getHeight() != h || cachedPreampRect != preampCardRect ||
        cachedShowKb != showKeyboard) {
      cachedBgImage = juce::Image(juce::Image::ARGB, w, h, true);
      cachedPreampRect = preampCardRect;
      cachedShowKb = showKeyboard;
      juce::Graphics bg(cachedBgImage);

      // Rich Vintage Walnut Cabinet Frame
      FanfareB3::Ui::drawWalnutWood(bg, bounds, 7.0f, false);

      // Recessed console well
      const auto deck = bounds.reduced(11.0f, 6.0f);
      FanfareB3::Ui::drawWalnutWood(bg, deck, 4.0f, true);
      bg.setColour(juce::Colour(0x77D4A15C));
      bg.drawRoundedRectangle(deck, 4.0f, 1.0f);

      // Top bronze header rail strip
      const auto hdr = juce::Rectangle<float>(deck.getX() + 4.0f, deck.getY() + 3.0f,
                                              deck.getWidth() - 8.0f, 30.0f);
      juce::ColourGradient hdrG(juce::Colour(0xFF28201A), hdr.getX(), hdr.getY(),
                                juce::Colour(0xFF17120E), hdr.getX(), hdr.getBottom(), false);
      bg.setGradientFill(hdrG);
      bg.fillRoundedRectangle(hdr, 3.0f);
      bg.setColour(juce::Colour(0x55A78355));
      bg.drawRoundedRectangle(hdr, 3.0f, 0.9f);

      // Outer cabinet shadow & brass highlight
      bg.setColour(juce::Colour(0x44D69D5E));
      bg.drawRoundedRectangle(bounds.reduced(1.2f), 6.5f, 1.0f);
      bg.setColour(juce::Colour(0xFF100703));
      bg.drawRoundedRectangle(bounds.reduced(0.5f), 7.0f, 1.2f);

      // Brushed Dark-Bronze Hardware Module Panels with Slotted Brass Screws
      FanfareB3::Ui::drawBrushedBronzePanel(
          bg, percScanCardRect.toFloat(),
          "HARMONIC PERCUSSION  &  SCANNER VIBRATO / CHORUS");
      FanfareB3::Ui::drawBrushedBronzePanel(
          bg, preampCardRect.toFloat(),
          "AO-28 TUBE PREAMP  &  SWELL EXPRESSION");
      FanfareB3::Ui::drawBrushedBronzePanel(
          bg, leslieCardRect.toFloat(),
          "LESLIE 122 ROTARY SPEAKER");
      FanfareB3::Ui::drawBrushedBronzePanel(
          bg, studioCardRect.toFloat(),
          "ONBOARD STUDIO FX");
      FanfareB3::Ui::drawBrushedBronzePanel(
          bg, extCardRect.toFloat(),
          "EXTERNAL VST3 INSERTS (PRE-LESLIE SAT  /  EXT LESLIE)");
    }

    g.drawImageAt(cachedBgImage, 0, 0);

    // Warm Amber Jewel Pilot LEDs centered above each of the 5 AO-28 Preamp knobs
    for (const auto *kp : {&driveKnob, &toneKnob, &clickKnob, &leakKnob, &volKnob}) {
      if (kp->getWidth() > 0) {
        const auto kb = kp->getBounds().toFloat();
        const float lx = kb.getCentreX();
        const float ly = kb.getY() - 5.5f;
        g.setColour(juce::Colour(0x48FF9933));
        g.fillEllipse(lx - 5.5f, ly - 5.5f, 11.0f, 11.0f);
        g.setColour(juce::Colour(0xFF3E240E));
        g.fillEllipse(lx - 2.6f, ly - 2.6f, 5.2f, 5.2f);
        g.setColour(juce::Colour(0xFFFFB247));
        g.fillEllipse(lx - 2.0f, ly - 2.0f, 4.0f, 4.0f);
        g.setColour(juce::Colour(0xFFFFF1C7));
        g.fillEllipse(lx - 0.9f, ly - 1.1f, 1.5f, 1.5f);
      }
    }

    // Live stereo peak meter in header
    if (!meterRect.isEmpty()) {
      BoutiqueLookAndFeel::drawStereoSegmentedMeter(
          g, meterRect.toFloat(),
          juce::jlimit(0.0f, 1.0f, proc.getOutputPeakL()),
          juce::jlimit(0.0f, 1.0f, proc.getOutputPeakR()),
          12,
          ThemeManager::get(Theme::Role::meterLow),
          ThemeManager::get(Theme::Role::meterMid),
          ThemeManager::get(Theme::Role::meterPeak));
    }
  }

  void resized() override {
    if (!applyingHostBounds && resizer != nullptr && resizer->isMouseButtonDown())
      userDraggedCorner = true;

    auto area = getLocalBounds().reduced(18, 10);

    // Header (30 px)
    auto header = area.removeFromTop(30);
    title.setBounds(header.removeFromLeft(142));
    regBadge.setBounds(header.removeFromLeft(94).reduced(2, 3));
    header.removeFromLeft(4);
    prevPresetBtn.setBounds(header.removeFromLeft(24).reduced(1, 3));
    presetBox.setBounds(header.removeFromLeft(182).reduced(2, 2));
    nextPresetBtn.setBounds(header.removeFromLeft(24).reduced(1, 3));
    saveBtn.setBounds(header.removeFromLeft(48).reduced(2, 2));
    initBtn.setBounds(header.removeFromLeft(44).reduced(2, 2));

    closeBtn.setBounds(header.removeFromRight(28).reduced(2, 3));
    meterRect = header.removeFromRight(36).reduced(3, 4);
    keysToggle.setBounds(header.removeFromRight(52).reduced(2, 3));
    midiBtn.setBounds(header.removeFromRight(52).reduced(2, 2));
    invertToggle.setBounds(header.removeFromRight(66).reduced(2, 3));
    octaveUpToggle.setBounds(header.removeFromRight(42).reduced(2, 3));
    ccProfileBox.setBounds(header.removeFromRight(juce::jmin(146, juce::jmax(90, header.getWidth() - 4))).reduced(2, 2));

    area.removeFromTop(6);

    // Bottom keyboard if active
    if (showKeyboard) {
      waterfallKeys.setBounds(area.removeFromBottom(82));
      area.removeFromBottom(5);
    }

    // Studio FX + External VST3 bar (72 px)
    auto bottomBar = area.removeFromBottom(72);
    area.removeFromBottom(5);
    const int gap = 6;
    const int studioW = (bottomBar.getWidth() - gap) * 48 / 100;
    studioCardRect = bottomBar.removeFromLeft(studioW);
    bottomBar.removeFromLeft(gap);
    extCardRect = bottomBar;

    // Layout Studio FX Card (6 knobs: EQ Low, Mid, High, Comp, Reverb, Delay)
    {
      auto c = studioCardRect.reduced(8);
      c.removeFromTop(18);
      const int kw = c.getWidth() / 6;
      auto placeFx = [&](juce::Slider &k, juce::Label &l, int idx) {
        auto cell = c.withX(c.getX() + idx * kw).withWidth(kw);
        l.setBounds(cell.removeFromTop(12));
        k.setBounds(cell.reduced(1));
      };
      placeFx(eqLowKnob,  eqLowLabel,  0);
      placeFx(eqMidKnob,  eqMidLabel,  1);
      placeFx(eqHighKnob, eqHighLabel, 2);
      placeFx(compKnob,   compLabel,   3);
      placeFx(revKnob,    revLabel,    4);
      placeFx(delayKnob,  delayLabel,  5);
    }

    // Layout External VST3 Card (2 insert rows: SAT VST, LESLIE VST)
    {
      auto c = extCardRect.reduced(8);
      c.removeFromTop(18);
      const int rowH = c.getHeight() / 2;
      for (int e = 0; e < B3OrganProcessor::kNumExtSlots; ++e) {
        auto row = c.removeFromTop(rowH).reduced(0, 1);
        extSlotLabel[e].setBounds(row.removeFromLeft(66));
        extBypBtn[e].setBounds(row.removeFromRight(38).reduced(1, 0));
        extEditBtn[e].setBounds(row.removeFromRight(42).reduced(1, 0));
        extPickBtn[e].setBounds(row.reduced(2, 0));
      }
    }

    // Drawbars (top ~48% of remaining console deck)
    const int dbHeight = juce::jlimit(148, 230, area.getHeight() * 48 / 100);
    drawbarStrip.setBounds(area.removeFromTop(dbHeight));
    area.removeFromTop(5);

    // Lower deck split into 3 side-by-side cards
    const int w = area.getWidth();
    const int card1W = (w - gap * 2) * 34 / 100;
    const int card2W = (w - gap * 2) * 34 / 100;

    percScanCardRect = area.removeFromLeft(card1W);
    area.removeFromLeft(gap);
    preampCardRect = area.removeFromLeft(card2W);
    area.removeFromLeft(gap);
    leslieCardRect = area;

    // 1. Layout Percussion & Scanner Card
    {
      auto c = percScanCardRect.reduced(10, 6);
      c.removeFromTop(18);
      auto rPerc = c.removeFromTop(28);
      const int pw = rPerc.getWidth() / 4;
      percOnToggle.setBounds(rPerc.removeFromLeft(pw + 4).reduced(1, 0));
      percSoftToggle.setBounds(rPerc.removeFromLeft(pw - 2).reduced(1, 0));
      percFastToggle.setBounds(rPerc.removeFromLeft(pw + 4).reduced(1, 0));
      percThirdToggle.setBounds(rPerc.reduced(2, 0));

      c.removeFromTop(4);
      auto rScan = c.removeFromTop(18);
      for (int i = 0; i < 7; ++i)
        scannerButtons[(size_t)i]->setBounds(
            rScan.removeFromLeft(rScan.getWidth() / (7 - i)).reduced(1, 0));

      auto rDepth = c.removeFromBottom(20);
      scannerDepthLabel.setBounds(rDepth.removeFromLeft(68));
      scannerDepthSlider.setBounds(rDepth.reduced(2, 1));

      scannerDial.setBounds(c.reduced(4, 1));
    }

    // 2. Layout AO-28 Preamp Card
    {
      auto c = preampCardRect.reduced(10, 6);
      c.removeFromTop(18);
      auto swellRow = c.removeFromBottom(22);
      swellLabel.setBounds(swellRow.removeFromLeft(46));
      swellSlider.setBounds(swellRow.reduced(2, 1));
      c.removeFromBottom(4);

      const int kw = c.getWidth() / 5;
      auto placeKnob = [&](juce::Slider &knob, juce::Label &lbl, int idx) {
        auto cell = c.withX(c.getX() + idx * kw).withWidth(kw);
        lbl.setBounds(cell.removeFromTop(13));
        cell.removeFromTop(11); // room for glowing amber pilot LED
        knob.setBounds(cell.reduced(2));
      };
      placeKnob(driveKnob, driveLabel, 0);
      placeKnob(toneKnob, toneLabel, 1);
      placeKnob(clickKnob, clickLabel, 2);
      placeKnob(leakKnob, leakLabel, 3);
      placeKnob(volKnob, volLabel, 4);
    }

    // 3. Layout Leslie 122 Card
    {
      auto c = leslieCardRect.reduced(10, 6);
      c.removeFromTop(18);
      auto rotors = c.removeFromLeft(juce::jlimit(68, 92, c.getWidth() * 28 / 100));
      leslieDisplay.setBounds(rotors.reduced(2));
      c.removeFromLeft(6);

      auto row1Sw = c.removeFromTop(23);
      const int mw = row1Sw.getWidth() / 3;
      leslieSlowBtn.setBounds(row1Sw.removeFromLeft(mw).reduced(1, 1));
      leslieBrakeBtn.setBounds(row1Sw.removeFromLeft(mw).reduced(1, 1));
      leslieFastBtn.setBounds(row1Sw.reduced(1, 1));

      c.removeFromTop(2);
      auto row2Sw = c.removeFromTop(20);
      leslieBypassToggle.setBounds(row2Sw.removeFromLeft(mw).reduced(1, 0));
      cc64LeslieToggle.setBounds(row2Sw.reduced(1, 0));

      auto row2 = c.removeFromBottom(20);
      {
        auto h = row2.removeFromLeft(row2.getWidth() / 2);
        leslieChorusLabel.setBounds(h.removeFromLeft(46));
        leslieChorusSlider.setBounds(h.reduced(2, 1));
        leslieWarmthLabel.setBounds(row2.removeFromLeft(46));
        leslieWarmthSlider.setBounds(row2.reduced(2, 1));
      }
      auto row1 = c.removeFromBottom(20);
      {
        auto h = row1.removeFromLeft(row1.getWidth() / 2);
        roomMixLabel.setBounds(h.removeFromLeft(46));
        roomMixSlider.setBounds(h.reduced(2, 1));
        micSpreadLabel.setBounds(row1.removeFromLeft(46));
        micSpreadSlider.setBounds(row1.reduced(2, 1));
      }
    }

    if (resizer != nullptr)
      resizer->setBounds(getWidth() - 16, getHeight() - 16, 16, 16);
  }

private:
  RackSlot &slotRef;
  int chainIndex;
  B3OrganProcessor &proc;
  std::function<void()> closeCallback;

  juce::Label title, regBadge;
  juce::TextButton prevPresetBtn, nextPresetBtn, saveBtn, initBtn, midiBtn, closeBtn;
  juce::ComboBox presetBox, ccProfileBox;
  juce::ToggleButton invertToggle, octaveUpToggle, keysToggle;

  FanfareB3::Ui::B3VintageLookAndFeel vintageLaf;
  FanfareB3::Ui::HammondDrawbarStrip drawbarStrip;
  FanfareB3::Ui::B3ScannerDial scannerDial;
  FanfareB3::Ui::LeslieRotorDisplay leslieDisplay;
  FanfareB3::Ui::B3WaterfallKeyboard waterfallKeys;
  juce::Image cachedBgImage;
  juce::Rectangle<int> cachedPreampRect;
  bool cachedShowKb = false;

  juce::ToggleButton percOnToggle, percSoftToggle, percFastToggle, percThirdToggle;
  std::array<std::unique_ptr<juce::ToggleButton>, 7> scannerButtons;
  juce::Slider scannerDepthSlider;
  juce::Label scannerDepthLabel;

  juce::Slider driveKnob, toneKnob, clickKnob, leakKnob, volKnob, swellSlider;
  juce::Label driveLabel, toneLabel, clickLabel, leakLabel, volLabel, swellLabel;

  juce::ToggleButton leslieSlowBtn, leslieBrakeBtn, leslieFastBtn;
  juce::ComboBox leslieCabBox;
  juce::ToggleButton leslieBypassToggle, cc64LeslieToggle;
  juce::Slider roomMixSlider, micSpreadSlider, leslieChorusSlider, leslieWarmthSlider;
  juce::Label roomMixLabel, micSpreadLabel, leslieChorusLabel, leslieWarmthLabel;

  juce::Slider eqLowKnob, eqMidKnob, eqHighKnob, compKnob, revKnob, delayKnob;
  juce::Label eqLowLabel, eqMidLabel, eqHighLabel, compLabel, revLabel, delayLabel;
  std::array<juce::Label, B3OrganProcessor::kNumExtSlots> extSlotLabel;
  std::array<juce::TextButton, B3OrganProcessor::kNumExtSlots> extPickBtn, extEditBtn;
  std::array<juce::ToggleButton, B3OrganProcessor::kNumExtSlots> extBypBtn;

  juce::Rectangle<int> percScanCardRect, preampCardRect, leslieCardRect,
      studioCardRect, extCardRect, meterRect;
  juce::ComponentBoundsConstrainer resizeLimits;
  std::unique_ptr<juce::ResizableCornerComponent> resizer;
  bool showKeyboard = false;
  bool userDraggedCorner = false;
  bool applyingHostBounds = false;

  void showMidiRoutingDialog() {
    juce::Component::SafePointer<B3OrganComponent> safeThis(this);
    auto *dlg = new SynthMidiSetupDialog(
        slotRef, chainIndex, false,
        [safeThis] { if (safeThis && safeThis->onOpenNoteRange) safeThis->onOpenNoteRange(); },
        [safeThis] { if (safeThis && safeThis->onOpenCCDialog) safeThis->onOpenCCDialog(); },
        [safeThis] { if (safeThis && safeThis->onOpenMidiEffects) safeThis->onOpenMidiEffects(); },
        {}, "Fanfare B3");
    dlg->setSize(520, 440);
    auto &box = juce::CallOutBox::launchAsynchronously(
        std::unique_ptr<juce::Component>(dlg),
        midiBtn.getScreenBounds(),
        this);
    box.setDismissalMouseClicksAreAlwaysConsumed(true);
  }

  void stepPreset(int delta) {
    const auto &cat = proc.getPresetCatalog();
    if (cat.empty())
      return;
    int idx = proc.getCurrentPresetIndex();
    if (idx < 0)
      idx = 0;
    else
      idx = (idx + delta + (int)cat.size()) % (int)cat.size();
    proc.loadPresetAtIndex(idx);
    syncFromProcessor();
  }

  void promptSavePreset() {
    juce::Component::SafePointer<B3OrganComponent> safeThis(this);
    auto *w = new juce::AlertWindow("Save B3 Registration", "Preset name:",
                                    juce::MessageBoxIconType::NoIcon, this);
    w->addTextEditor("name", proc.getCurrentPresetName());
    w->addButton("Save", 1, juce::KeyPress(juce::KeyPress::returnKey));
    w->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
    w->enterModalState(
        true, juce::ModalCallbackFunction::create([safeThis, w](int r) {
          const juce::String name = w->getTextEditorContents("name").trim();
          delete w;
          if (safeThis == nullptr || r != 1 || name.isEmpty())
            return;
          if (safeThis->proc.saveUserPreset(name)) {
            safeThis->proc.refreshPresetCatalog();
            safeThis->rebuildPresetList();
            safeThis->syncFromProcessor();
          }
        }));
  }

  void rebuildPresetList() {
    presetBox.clear(juce::dontSendNotification);
    int id = 1;
    for (const auto &e : proc.getPresetCatalog())
      presetBox.addItem(e.isUser ? ("[U] " + e.name) : e.name, id++);
    if (proc.getCurrentPresetIndex() >= 0)
      presetBox.setSelectedId(proc.getCurrentPresetIndex() + 1,
                              juce::dontSendNotification);
  }

  void refreshRegistrationBadge() {
    juce::String txt = proc.getRegistrationString();
    if (proc.isPresetDirty())
      txt << " *";
    if (regBadge.getText() != txt)
      regBadge.setText(txt, juce::dontSendNotification);
  }

  void syncLeslieButtons() {
    const int lm = proc.leslieMode.load(std::memory_order_relaxed);
    leslieSlowBtn.setToggleState(lm == 0, juce::dontSendNotification);
    leslieBrakeBtn.setToggleState(lm == 1, juce::dontSendNotification);
    leslieFastBtn.setToggleState(lm == 2, juce::dontSendNotification);
  }

  void rebuildCcProfileBox() {
    proc.refreshCustomCcMaps();
    ccProfileBox.clear(juce::dontSendNotification);
    ccProfileBox.addItem("CC: Auto (CK88+All)", B3OrganProcessor::CcAuto + 1);
    ccProfileBox.addItem("CC: CK88 (77-86)", B3OrganProcessor::CcCk88 + 1);
    ccProfileBox.addItem("CC: Faders (21-29)", B3OrganProcessor::CcFaders + 1);
    ccProfileBox.addItem("CC: Sound (70-78)", B3OrganProcessor::CcSound + 1);
    ccProfileBox.addItem("CC: Classic (12-20)", B3OrganProcessor::CcClassic + 1);
    const auto &maps = proc.getCustomCcMaps();
    if (!maps.empty()) {
      ccProfileBox.addSeparator();
      for (int i = 0; i < (int)maps.size(); ++i)
        ccProfileBox.addItem("CC: " + maps[(size_t)i].name, 100 + i);
    }
    ccProfileBox.addSeparator();
    ccProfileBox.addItem("+ Learn / Edit Drawbar Map...", 999);
    syncCcProfileBoxSelection();
  }

  void syncCcProfileBoxSelection() {
    const int prof = proc.ccProfile.load(std::memory_order_relaxed);
    if (prof == B3OrganProcessor::CcCustom) {
      const auto &maps = proc.getCustomCcMaps();
      for (int i = 0; i < (int)maps.size(); ++i) {
        if (maps[(size_t)i].name.equalsIgnoreCase(proc.getCustomCcMapName())) {
          ccProfileBox.setSelectedId(100 + i, juce::dontSendNotification);
          return;
        }
      }
    }
    ccProfileBox.setSelectedId(juce::jlimit(0, 4, prof) + 1,
                               juce::dontSendNotification);
  }

  void showDrawbarMapDialog() {
    juce::Component::SafePointer<B3OrganComponent> safeThis(this);
    auto *dlg = new FanfareB3::Ui::B3DrawbarMapDialog(proc, [safeThis] {
      if (safeThis) {
        safeThis->rebuildCcProfileBox();
        safeThis->syncFromProcessor();
      }
    });
    auto &box = juce::CallOutBox::launchAsynchronously(
        std::unique_ptr<juce::Component>(dlg),
        ccProfileBox.getScreenBounds(),
        this);
    box.setDismissalMouseClicksAreAlwaysConsumed(true);
  }

  void syncFromProcessor() {
    if (proc.getCurrentPresetIndex() >= 0)
      presetBox.setSelectedId(proc.getCurrentPresetIndex() + 1,
                              juce::dontSendNotification);
    syncCcProfileBoxSelection();
    invertToggle.setToggleState(proc.invertDrawbars.load(std::memory_order_relaxed),
                                juce::dontSendNotification);
    octaveUpToggle.setToggleState(proc.octaveUp.load(std::memory_order_relaxed),
                                  juce::dontSendNotification);
    cc64LeslieToggle.setToggleState(
        proc.sustainControlsLeslie.load(std::memory_order_relaxed),
        juce::dontSendNotification);

    percOnToggle.setToggleState(proc.percOn.load(), juce::dontSendNotification);
    percSoftToggle.setToggleState(proc.percSoft.load(), juce::dontSendNotification);
    percFastToggle.setToggleState(proc.percFast.load(), juce::dontSendNotification);
    percThirdToggle.setToggleState(proc.percThird.load(), juce::dontSendNotification);

    const int sm = proc.scannerMode.load();
    for (int i = 0; i < 7; ++i)
      scannerButtons[(size_t)i]->setToggleState(sm == i, juce::dontSendNotification);
    scannerDial.repaint();
    scannerDepthSlider.setValue(proc.scannerDepth.load(), juce::dontSendNotification);

    driveKnob.setValue(valueToCentered(proc.drive.load(), kDriveC, kDriveMax), juce::dontSendNotification);
    toneKnob.setValue(valueToCentered(proc.tone.load(), kToneC, 1.0), juce::dontSendNotification);
    clickKnob.setValue(valueToCentered(proc.clickDepth.load(), kClickC, 1.0), juce::dontSendNotification);
    leakKnob.setValue(valueToCentered(proc.leakage.load(), kLeakC, kLeakMax), juce::dontSendNotification);
    volKnob.setValue(valueToCentered(proc.volume.load(), kVolC, 1.0), juce::dontSendNotification);
    swellSlider.setValue(proc.swell.load(), juce::dontSendNotification);

    syncLeslieButtons();
    leslieBypassToggle.setToggleState(proc.leslieBypass.load(), juce::dontSendNotification);
    roomMixSlider.setValue(valueToCentered(proc.roomMix.load(), kRoomC, 1.0), juce::dontSendNotification);
    leslieChorusSlider.setValue(valueToCentered(proc.leslieChorus.load(), kChorusC, 1.0), juce::dontSendNotification);
    leslieWarmthSlider.setValue(valueToCentered(proc.leslieWarmth.load(), kWarmC, 1.0), juce::dontSendNotification);
    micSpreadSlider.setValue(valueToCentered(proc.micSpread.load(), kSpreadC, 1.0), juce::dontSendNotification);

    eqLowKnob.setValue(proc.eqLowDb.load(), juce::dontSendNotification);
    eqMidKnob.setValue(proc.eqMidDb.load(), juce::dontSendNotification);
    eqHighKnob.setValue(proc.eqHighDb.load(), juce::dontSendNotification);
    compKnob.setValue(proc.compAmount.load(), juce::dontSendNotification);
    revKnob.setValue(proc.reverbMix.load(), juce::dontSendNotification);
    delayKnob.setValue(valueToCentered(proc.delayMix.load(), kDelayC, kDelayMax), juce::dontSendNotification);
    refreshExtPluginLabels();

    refreshRegistrationBadge();
    drawbarStrip.repaint();
    leslieDisplay.repaint();
  }

  void timerCallback() override {
    refreshRegistrationBadge();

    // Sync hardware MIDI CCs (drawbars, swell, volume, mod wheel -> Leslie) to UI
    if (!swellSlider.isMouseButtonDown()) {
      const float sw = proc.swell.load(std::memory_order_relaxed);
      if (std::abs((float)swellSlider.getValue() - sw) > 0.005f)
        swellSlider.setValue(sw, juce::dontSendNotification);
    }
    if (!volKnob.isMouseButtonDown()) {
      const float vo = (float)valueToCentered(proc.volume.load(std::memory_order_relaxed), kVolC, 1.0);
      if (std::abs((float)volKnob.getValue() - vo) > 0.005f)
        volKnob.setValue(vo, juce::dontSendNotification);
    }

    syncLeslieButtons();
    if (!drawbarStrip.isUserDragging())
      drawbarStrip.repaint();
    leslieDisplay.repaint();
    if (!meterRect.isEmpty())
      repaint(meterRect);
  }

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(B3OrganComponent)
};
