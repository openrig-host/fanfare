#include "BoutiqueLookAndFeel.h"
#include "AssetLoader.h"
#include "EmbeddedFonts.h"

namespace {

juce::Typeface::Ptr loadGzippedTypeface(const unsigned char* gzData, size_t gzLen) {
  juce::MemoryInputStream mis(gzData, gzLen, false);
  juce::GZIPDecompressorInputStream gz(mis);
  juce::MemoryBlock uncompressed;
  gz.readIntoMemoryBlock(uncompressed);
  if (uncompressed.getSize() == 0)
    return nullptr;
  return juce::Typeface::createSystemTypefaceFor(uncompressed.getData(), uncompressed.getSize());
}

juce::Colour contrastingInk(juce::Colour bg) {
  return bg.getPerceivedBrightness() > 0.54f
             ? juce::Colour(0xFF14181B)
             : juce::Colour(0xFFF6F5F0);
}

} // namespace

juce::Typeface::Ptr BoutiqueLookAndFeel::getCondensedTypeface() {
  static juce::Typeface::Ptr tf = loadGzippedTypeface(
      Fanfare::EmbeddedFonts::BARLOW_CONDENSED_SEMIBOLD_GZ,
      Fanfare::EmbeddedFonts::BARLOW_CONDENSED_SEMIBOLD_GZ_LEN);
  return tf;
}

juce::Typeface::Ptr BoutiqueLookAndFeel::getUiRegularTypeface() {
  static juce::Typeface::Ptr tf = loadGzippedTypeface(
      Fanfare::EmbeddedFonts::SOURCE_SANS_REGULAR_GZ,
      Fanfare::EmbeddedFonts::SOURCE_SANS_REGULAR_GZ_LEN);
  return tf;
}

juce::Typeface::Ptr BoutiqueLookAndFeel::getUiBoldTypeface() {
  static juce::Typeface::Ptr tf = loadGzippedTypeface(
      Fanfare::EmbeddedFonts::SOURCE_SANS_SEMIBOLD_GZ,
      Fanfare::EmbeddedFonts::SOURCE_SANS_SEMIBOLD_GZ_LEN);
  return tf;
}

juce::Font BoutiqueLookAndFeel::condensedFont(float height, float kerning) {
  if (auto tf = getCondensedTypeface()) {
    return juce::Font(juce::FontOptions(tf).withHeight(height).withKerningFactor(kerning));
  }
  return juce::Font(juce::FontOptions(height, juce::Font::bold).withKerningFactor(kerning));
}

juce::Font BoutiqueLookAndFeel::uiFont(float height, bool bold) {
  const bool forceMediumOnBright = !ThemeManager::getInstance().active().dark && height <= 14.5f;
  const bool useBoldFace = bold || forceMediumOnBright;
  if (auto tf = useBoldFace ? getUiBoldTypeface() : getUiRegularTypeface()) {
    return juce::Font(juce::FontOptions(tf).withHeight(height));
  }
  return juce::Font(juce::FontOptions(height, useBoldFace ? juce::Font::bold : juce::Font::plain));
}

juce::Font BoutiqueLookAndFeel::monoFont(float height, bool bold) {
  return juce::Font(juce::FontOptions("Consolas", height, bold ? juce::Font::bold : juce::Font::plain));
}

void BoutiqueLookAndFeel::drawSegmentedMeterBar(juce::Graphics& g,
                                                juce::Rectangle<float> barArea,
                                                float levelNormalized,
                                                int numSegments,
                                                juce::Colour lowCol,
                                                juce::Colour midCol,
                                                juce::Colour highCol) {
  if (barArea.getWidth() <= 0.5f || barArea.getHeight() <= 2.0f || numSegments < 2)
    return;

  const float gap = 1.5f;
  const float segH = juce::jmax(1.0f, (barArea.getHeight() - (numSegments - 1) * gap) / (float)numSegments);
  const float clamped = juce::jlimit(0.0f, 1.0f, levelNormalized);
  const int activeSegs = (int)std::round(clamped * (float)numSegments);
  const auto unlitBase = ThemeManager::get(Theme::Role::bypassed).withAlpha(0.28f);

  for (int i = 0; i < numSegments; ++i) {
    float segY = barArea.getBottom() - (i + 1) * segH - i * gap;
    juce::Rectangle<float> segRect(barArea.getX(), segY, barArea.getWidth(), segH);

    // Anchored to scale top: green 0..62%, gold 62..86%, red 86..100%
    float pos = (float)i / (float)(numSegments - 1);
    juce::Colour zoneCol = (pos < 0.62f) ? lowCol : ((pos < 0.86f) ? midCol : highCol);

    if (i < activeSegs) {
      g.setColour(zoneCol);
    } else {
      g.setColour(unlitBase.interpolatedWith(zoneCol, 0.10f));
    }
    g.fillRect(segRect);
  }
}

void BoutiqueLookAndFeel::drawStereoSegmentedMeter(juce::Graphics& g,
                                                   juce::Rectangle<float> area,
                                                   float leftNorm,
                                                   float rightNorm,
                                                   int numSegments,
                                                   juce::Colour lowCol,
                                                   juce::Colour midCol,
                                                   juce::Colour highCol) {
  Fanfare::ModalFrame::paintRecessedWell(g, area, 2.0f);
  auto inner = area.reduced(2.0f);
  const float midGap = 1.5f;
  float barW = juce::jmax(1.0f, (inner.getWidth() - midGap) * 0.5f);
  juce::Rectangle<float> lBar(inner.getX(), inner.getY(), barW, inner.getHeight());
  juce::Rectangle<float> rBar(inner.getRight() - barW, inner.getY(), barW, inner.getHeight());

  drawSegmentedMeterBar(g, lBar, leftNorm, numSegments, lowCol, midCol, highCol);
  drawSegmentedMeterBar(g, rBar, rightNorm, numSegments, lowCol, midCol, highCol);
}

BoutiqueLookAndFeel::BoutiqueLookAndFeel() {
  if (auto tf = getUiRegularTypeface())
    setDefaultSansSerifTypeface(tf);
  else
    setDefaultSansSerifTypefaceName("Segoe UI");

  applyThemeColors();
}

void BoutiqueLookAndFeel::applyThemeColors() {
  useModernStyle = ThemeManager::getInstance().isFlat();
  const auto& t = ThemeManager::getInstance().active();
  const auto fieldBg = t.dark ? t.sunken : t.raised;
  const auto fieldOutline = t.dark ? t.border : t.borderStrong;

  if (auto tf = t.dark ? getUiRegularTypeface() : getUiBoldTypeface())
    setDefaultSansSerifTypeface(tf);

  setColour(juce::ResizableWindow::backgroundColourId, t.background);
  setColour(juce::DocumentWindow::backgroundColourId, t.background);

  setColour(juce::AlertWindow::backgroundColourId, t.panel);
  setColour(juce::AlertWindow::textColourId, t.text);
  setColour(juce::AlertWindow::outlineColourId, t.borderStrong);

  setColour(juce::Slider::thumbColourId, t.knobThumb);
  setColour(juce::Slider::trackColourId, t.trackGroove);
  setColour(juce::Slider::textBoxTextColourId, t.text);
  setColour(juce::Slider::textBoxBackgroundColourId, fieldBg);
  setColour(juce::Slider::textBoxOutlineColourId, fieldOutline);
  setColour(juce::Slider::textBoxHighlightColourId, t.accent);

  setColour(juce::TextButton::buttonColourId, t.raised);
  setColour(juce::TextButton::buttonOnColourId, t.accent);
  setColour(juce::TextButton::textColourOffId, t.text);
  setColour(juce::TextButton::textColourOnId, t.textOnAccent);

  setColour(juce::ToggleButton::textColourId, t.text);
  setColour(juce::ToggleButton::tickColourId, t.accent);
  setColour(juce::ToggleButton::tickDisabledColourId, t.textFaint);

  setColour(juce::Label::textColourId, t.text);
  setColour(juce::Label::backgroundColourId, juce::Colours::transparentBlack);
  setColour(juce::Label::outlineColourId, juce::Colours::transparentBlack);

  setColour(juce::ComboBox::backgroundColourId, fieldBg);
  setColour(juce::ComboBox::outlineColourId, fieldOutline);
  setColour(juce::ComboBox::textColourId, t.text);
  setColour(juce::ComboBox::arrowColourId, t.dark ? t.textDim : t.text);

  setColour(juce::PopupMenu::backgroundColourId, t.panel);
  setColour(juce::PopupMenu::textColourId, t.text);
  setColour(juce::PopupMenu::headerTextColourId, t.accent);
  setColour(juce::PopupMenu::highlightedBackgroundColourId, t.dark ? t.raised : t.panelAlt);
  setColour(juce::PopupMenu::highlightedTextColourId, t.accent);

  setColour(juce::ListBox::backgroundColourId, t.panel);
  setColour(juce::ListBox::outlineColourId, t.border);
  setColour(juce::ListBox::textColourId, t.text);

  setColour(juce::ScrollBar::backgroundColourId, t.sunken);
  setColour(juce::ScrollBar::thumbColourId, t.borderStrong);
  setColour(juce::ScrollBar::trackColourId, t.sunken);

  setColour(juce::TextEditor::backgroundColourId, fieldBg);
  setColour(juce::TextEditor::textColourId, t.text);
  setColour(juce::TextEditor::outlineColourId, fieldOutline);
  setColour(juce::TextEditor::highlightColourId, t.accent.withAlpha(0.30f));
  setColour(juce::TextEditor::highlightedTextColourId, t.text);
  setColour(juce::TextEditor::focusedOutlineColourId, t.accent);

  setColour(juce::TabbedComponent::backgroundColourId, t.panel);
  setColour(juce::TabbedComponent::outlineColourId, t.border);
  setColour(juce::TabbedButtonBar::tabOutlineColourId, t.border);
  setColour(juce::TabbedButtonBar::frontOutlineColourId, t.accent);

  setColour(juce::TooltipWindow::backgroundColourId, t.dark ? t.panelAlt : t.raised);
  setColour(juce::TooltipWindow::textColourId, t.text);
  setColour(juce::TooltipWindow::outlineColourId, t.borderStrong);
}

juce::Typeface::Ptr BoutiqueLookAndFeel::getTypefaceForFont(const juce::Font& font) {
  const auto name = font.getTypefaceName();
  if (name.containsIgnoreCase("Consolas") || name.containsIgnoreCase("Cascadia") ||
      name.containsIgnoreCase("Mono") || name == juce::Font::getDefaultMonospacedFontName()) {
    return juce::LookAndFeel_V4::getTypefaceForFont(font);
  }
  if (name.containsIgnoreCase("Barlow") || name.containsIgnoreCase("Condensed")) {
    if (auto tf = getCondensedTypeface())
      return tf;
  }
  if (name == juce::Font::getDefaultSansSerifFontName() ||
      name == juce::Font::getSystemUIFontName() ||
      name.containsIgnoreCase("Source Sans") ||
      name.containsIgnoreCase("Segoe")) {
    const bool useBold = font.isBold() || !ThemeManager::getInstance().active().dark;
    if (auto tf = useBold ? getUiBoldTypeface() : getUiRegularTypeface())
      return tf;
  }
  return juce::LookAndFeel_V4::getTypefaceForFont(font);
}

juce::Font BoutiqueLookAndFeel::getTextButtonFont(juce::TextButton&, int buttonHeight) {
  float h = juce::jlimit(10.5f, 13.5f, (float)buttonHeight * 0.56f);
  return condensedFont(h, 0.05f);
}

juce::Font BoutiqueLookAndFeel::getComboBoxFont(juce::ComboBox& box) {
  float h = juce::jlimit(11.5f, 13.5f, (float)box.getHeight() * 0.62f);
  return uiFont(h, true);
}

juce::Font BoutiqueLookAndFeel::getPopupMenuFont() {
  return uiFont(13.5f, false);
}

juce::Font BoutiqueLookAndFeel::getAlertWindowTitleFont() {
  return condensedFont(17.5f, 0.06f);
}

juce::Font BoutiqueLookAndFeel::getAlertWindowMessageFont() {
  return uiFont(14.0f, false);
}

juce::Font BoutiqueLookAndFeel::getAlertWindowFont() {
  return uiFont(13.5f, false);
}

void BoutiqueLookAndFeel::drawToggleSwitch(juce::Graphics &g,
                                           juce::ToggleButton &button,
                                           const juce::String &svgAsset,
                                           bool isOrange) {
  auto bounds = button.getLocalBounds().toFloat();
  auto* svg = isOrange ? getCachedSvg(toggleOrangeSvg, svgAsset)
                       : getCachedSvg(toggleSwitchSvg, svgAsset);
  if (svg == nullptr) {
    juce::LookAndFeel_V4::drawToggleButton(g, button, button.isOver(), button.isDown());
    return;
  }

  svg->setTransformToFit(bounds, juce::RectanglePlacement::centred);
  float rotation = button.getToggleState() ? 0.0f : 180.0f;
  if (rotation != 0.0f) {
    auto transform = svg->getTransform().followedBy(
        juce::AffineTransform::rotation(juce::degreesToRadians(rotation),
                                        bounds.getCentreX(), bounds.getCentreY()));
    svg->setTransform(transform);
  }
  svg->draw(g, 1.0f);
}

void BoutiqueLookAndFeel::drawSkeuomorphicButtonBackground(
    juce::Graphics &g, juce::Button &button, const juce::Colour &baseColour) {
  auto bounds = button.getLocalBounds().toFloat();
  auto cornerSize = 3.0f;

  g.setColour(juce::Colours::black.withAlpha(0.4f));
  g.fillRoundedRectangle(bounds.translated(1, 1), cornerSize);

  juce::ColourGradient bodyGrad(baseColour.brighter(0.15f), bounds.getX(),
                                bounds.getY(), baseColour.darker(0.25f),
                                bounds.getX(), bounds.getBottom(), false);
  g.setGradientFill(bodyGrad);
  g.fillRoundedRectangle(bounds, cornerSize);

  g.setColour(ThemeManager::get(Theme::Role::bevelHi));
  g.drawRoundedRectangle(bounds.reduced(0.5f), cornerSize, 1.0f);
}

void BoutiqueLookAndFeel::drawToggleButton(juce::Graphics &g,
                                           juce::ToggleButton &button,
                                           bool shouldDrawButtonAsHighlighted,
                                           bool /*shouldDrawButtonAsDown*/) {
  if (button.getProperties().contains("useToggleSwitch")) {
    bool isOrange = button.getProperties()["isOrangeToggle"];
    juce::String svgAsset = isOrange ? "toggle_orange.svg" : "toggle_switch.svg";
    drawToggleSwitch(g, button, svgAsset, isOrange);
    return;
  }

  const auto& t = ThemeManager::getInstance().active();
  auto bounds = button.getLocalBounds().toFloat();
  const float boxSize = 13.0f;
  juce::Rectangle<float> ledBox(bounds.getX() + 2.0f,
                                bounds.getCentreY() - boxSize * 0.5f,
                                boxSize, boxSize);

  Fanfare::ModalFrame::paintRecessedWell(g, ledBox, 2.0f);

  if (button.getToggleState()) {
    auto lit = ledBox.reduced(2.5f);
    g.setColour(t.accent);
    g.fillRoundedRectangle(lit, 1.0f);
  } else if (shouldDrawButtonAsHighlighted) {
    g.setColour(t.borderStrong);
    g.drawRoundedRectangle(ledBox.reduced(0.5f), 2.0f, 1.0f);
  }

  g.setColour(button.isEnabled()
                  ? (button.getToggleState() ? t.text : t.textDim)
                  : t.textFaint);
  g.setFont(condensedFont(12.5f, 0.05f));
  auto textRect = bounds.withTrimmedLeft(boxSize + 7.0f);
  g.drawFittedText(button.getButtonText().toUpperCase(),
                   textRect.toNearestInt(),
                   juce::Justification::centredLeft, 1);
}

void BoutiqueLookAndFeel::drawRotarySlider(juce::Graphics &g, int x, int y,
                                           int width, int height,
                                           float sliderPos,
                                           const float rotaryStartAngle,
                                           const float rotaryEndAngle,
                                           juce::Slider &slider) {
  const auto& t = ThemeManager::getInstance().active();
  auto bounds = juce::Rectangle<int>(x, y, width, height).toFloat();
  auto angle = rotaryStartAngle + sliderPos * (rotaryEndAngle - rotaryStartAngle);

  auto radius = juce::jmin(bounds.getWidth(), bounds.getHeight()) * 0.5f;
  auto center = bounds.getCentre();
  float knobRadius = juce::jmax(6.0f, radius * 0.78f);

  // Value arc track around rim
  float arcRadius = knobRadius + 3.0f;
  if (arcRadius < radius) {
    juce::Path bgArc;
    bgArc.addCentredArc(center.x, center.y, arcRadius, arcRadius, 0.0f,
                        rotaryStartAngle, rotaryEndAngle, true);
    g.setColour(t.sunken);
    g.strokePath(bgArc, juce::PathStrokeType(2.2f));

    if (sliderPos > 0.005f) {
      juce::Path valArc;
      valArc.addCentredArc(center.x, center.y, arcRadius, arcRadius, 0.0f,
                           rotaryStartAngle, angle, true);
      juce::Colour arcCol = t.accent;
      auto cid = slider.getComponentID();
      if (cid.containsIgnoreCase("foh")) arcCol = t.foh;
      else if (cid.containsIgnoreCase("iem")) arcCol = t.iem;
      g.setColour(arcCol);
      g.strokePath(valArc, juce::PathStrokeType(2.2f));
    }
  }

  // Drop shadow
  g.setColour(juce::Colours::black.withAlpha(0.42f));
  g.fillEllipse(center.x - knobRadius, center.y - knobRadius + 1.5f,
                knobRadius * 2.0f, knobRadius * 2.0f);

  // Machined aluminum/charcoal cap
  juce::ColourGradient knobGrad(t.faceTop.brighter(0.08f), center.x, center.y - knobRadius,
                                t.faceBot.darker(0.15f), center.x, center.y + knobRadius, false);
  g.setGradientFill(knobGrad);
  g.fillEllipse(center.x - knobRadius, center.y - knobRadius,
                knobRadius * 2.0f, knobRadius * 2.0f);

  // Knurled rim border
  g.setColour(t.borderStrong);
  g.drawEllipse(center.x - knobRadius, center.y - knobRadius,
                knobRadius * 2.0f, knobRadius * 2.0f, 1.0f);

  // Crisp pointer notch
  juce::Path p;
  float pointerLength = knobRadius * 0.72f;
  p.addRoundedRectangle(-1.2f, -knobRadius + 2.0f, 2.4f, pointerLength, 1.0f);
  p.applyTransform(juce::AffineTransform::rotation(angle).translated(center.x, center.y));
  g.setColour(t.accent);
  g.fillPath(p);
}

void BoutiqueLookAndFeel::drawButtonBackground(juce::Graphics &g,
                                               juce::Button &button,
                                               const juce::Colour &backgroundColour,
                                               bool shouldDrawButtonAsHighlighted,
                                               bool shouldDrawButtonAsDown) {
  const auto& t = ThemeManager::getInstance().active();
  auto fullBounds = button.getLocalBounds().toFloat();
  if (fullBounds.isEmpty())
    return;

  const float cornerSize = 3.0f;
  const auto role = Fanfare::getButtonRole(button);
  const bool isActive = button.getToggleState();

  // Plugin slot plate: recessed well when empty, raised plate + 3px left bar when loaded
  if (role == Fanfare::ButtonRole::PluginSlot) {
    bool hasPlugin = static_cast<bool>(button.getProperties().getWithDefault("hasPlugin", false));
    bool isMidiOut = static_cast<bool>(button.getProperties().getWithDefault("isMidiOut", false));

    auto r = fullBounds.reduced(0.5f);
    if (!hasPlugin) {
      g.setColour(shouldDrawButtonAsHighlighted ? t.panelAlt : t.sunken);
      g.fillRoundedRectangle(r, 2.5f);
      g.setColour(shouldDrawButtonAsHighlighted ? t.borderStrong : t.border);
      g.drawRoundedRectangle(r, 2.5f, 1.0f);
    } else {
      juce::Colour topC = shouldDrawButtonAsDown ? t.pressTop : t.faceTop;
      juce::Colour botC = shouldDrawButtonAsDown ? t.pressBot : t.faceBot;
      if (shouldDrawButtonAsHighlighted && !shouldDrawButtonAsDown) {
        topC = topC.brighter(0.08f);
        botC = botC.brighter(0.06f);
      }
      juce::ColourGradient grad(topC, r.getX(), r.getY(), botC, r.getX(), r.getBottom(), false);
      g.setGradientFill(grad);
      g.fillRoundedRectangle(r, 2.5f);

      juce::Colour barCol = isMidiOut ? t.midiPC : t.accent;
      if (button.getProperties().contains("slotAccent"))
        barCol = juce::Colour((juce::uint32)(juce::int64)button.getProperties()["slotAccent"]);

      g.setColour(barCol);
      g.fillRect(r.getX() + 0.5f, r.getY() + 1.0f, 3.0f, r.getHeight() - 2.0f);

      g.setColour(t.borderStrong);
      g.drawRoundedRectangle(r, 2.5f, 1.0f);
    }
    return;
  }

  // Hardware switch geometry: 1px press travel
  auto bounds = fullBounds.reduced(0.5f);
  if (shouldDrawButtonAsDown) {
    bounds = bounds.translated(0.0f, 1.0f).withTrimmedBottom(1.0f);
  } else {
    // 1px dark hardware lip beneath unpressed switch
    g.setColour(juce::Colours::black.withAlpha(t.dark ? 0.45f : 0.20f));
    g.fillRoundedRectangle(bounds.withTrimmedTop(1.0f), cornerSize);
    bounds = bounds.withTrimmedBottom(1.0f);
  }

  juce::Colour topCol = t.faceTop;
  juce::Colour botCol = t.faceBot;
  juce::Colour borderCol = t.border;
  bool drawTopBevel = !shouldDrawButtonAsDown;

  switch (role) {
    case Fanfare::ButtonRole::Panic: {
      juce::Colour p = t.panic;
      topCol = shouldDrawButtonAsDown ? p.darker(0.28f) : p.brighter(0.06f);
      botCol = shouldDrawButtonAsDown ? p.darker(0.12f) : p.darker(0.28f);
      borderCol = p.brighter(0.22f);
      break;
    }
    case Fanfare::ButtonRole::Help: {
      topCol = shouldDrawButtonAsDown ? t.helpFaceBot.darker(0.12f) : t.helpFaceTop;
      botCol = shouldDrawButtonAsDown ? t.helpFaceTop.darker(0.10f) : t.helpFaceBot;
      borderCol = t.helpFaceBot.darker(0.25f);
      break;
    }
    case Fanfare::ButtonRole::Foh: {
      if (isActive) {
        juce::Colour cap = t.faderCapFoh;
        topCol = shouldDrawButtonAsDown ? cap.darker(0.20f) : cap.brighter(0.08f);
        botCol = shouldDrawButtonAsDown ? cap.darker(0.05f) : cap.darker(0.22f);
        borderCol = t.dark ? cap.brighter(0.20f) : cap.darker(0.35f);
      } else {
        topCol = shouldDrawButtonAsDown ? t.pressTop : t.sunken;
        botCol = shouldDrawButtonAsDown ? t.pressBot : t.panel;
        borderCol = t.border;
      }
      break;
    }
    case Fanfare::ButtonRole::Iem: {
      if (isActive) {
        juce::Colour cap = t.faderCapIem;
        topCol = shouldDrawButtonAsDown ? cap.darker(0.20f) : cap.brighter(0.08f);
        botCol = shouldDrawButtonAsDown ? cap.darker(0.05f) : cap.darker(0.22f);
        borderCol = t.dark ? cap.brighter(0.20f) : cap.darker(0.35f);
      } else {
        topCol = shouldDrawButtonAsDown ? t.pressTop : t.sunken;
        botCol = shouldDrawButtonAsDown ? t.pressBot : t.panel;
        borderCol = t.border;
      }
      break;
    }
    case Fanfare::ButtonRole::Mute: {
      if (isActive) {
        topCol = shouldDrawButtonAsDown ? t.danger.darker(0.22f) : t.danger.brighter(0.08f);
        botCol = shouldDrawButtonAsDown ? t.danger.darker(0.08f) : t.danger.darker(0.24f);
        borderCol = t.danger.brighter(0.22f);
      } else {
        topCol = shouldDrawButtonAsDown ? t.pressTop : t.faceTop;
        botCol = shouldDrawButtonAsDown ? t.pressBot : t.faceBot;
      }
      break;
    }
    case Fanfare::ButtonRole::Primary: {
      if (isActive) {
        topCol = t.accent.brighter(0.06f);
        botCol = t.accent.darker(0.20f);
        borderCol = t.accent;
      } else {
        topCol = shouldDrawButtonAsDown ? t.pressTop : t.faceTop;
        botCol = shouldDrawButtonAsDown ? t.pressBot : t.faceBot;
        borderCol = t.accentDim;
      }
      break;
    }
    case Fanfare::ButtonRole::Danger: {
      topCol = shouldDrawButtonAsDown ? t.pressTop : t.faceTop;
      botCol = shouldDrawButtonAsDown ? t.pressBot : t.faceBot;
      borderCol = t.danger.withAlpha(0.65f);
      break;
    }
    case Fanfare::ButtonRole::Subtle: {
      topCol = shouldDrawButtonAsDown ? t.sunken : t.panel;
      botCol = shouldDrawButtonAsDown ? t.sunken : t.panel.darker(0.08f);
      borderCol = t.border;
      break;
    }
    case Fanfare::ButtonRole::Standard:
    case Fanfare::ButtonRole::PluginSlot:
    default: {
      if (isActive) {
        topCol = shouldDrawButtonAsDown ? t.accent.darker(0.20f) : t.accent;
        botCol = shouldDrawButtonAsDown ? t.accent.darker(0.05f) : t.accent.darker(0.22f);
        borderCol = t.accent.brighter(0.15f);
      } else {
        juce::ignoreUnused(backgroundColour);
        topCol = shouldDrawButtonAsDown ? t.pressTop : t.faceTop;
        botCol = shouldDrawButtonAsDown ? t.pressBot : t.faceBot;
      }
      break;
    }
  }

  if (shouldDrawButtonAsHighlighted && !shouldDrawButtonAsDown) {
    topCol = topCol.brighter(0.08f);
    botCol = botCol.brighter(0.06f);
    if (borderCol == t.border)
      borderCol = t.borderStrong;
  }

  if (!button.isEnabled()) {
    topCol = topCol.withAlpha(0.45f);
    botCol = botCol.withAlpha(0.45f);
    borderCol = borderCol.withAlpha(0.40f);
  }

  juce::ColourGradient faceGrad(topCol, bounds.getX(), bounds.getY(),
                                botCol, bounds.getX(), bounds.getBottom(), false);
  g.setGradientFill(faceGrad);
  g.fillRoundedRectangle(bounds, cornerSize);

  if (drawTopBevel) {
    g.setColour(t.bevelHi);
    g.drawHorizontalLine((int)bounds.getY() + 1, bounds.getX() + 2.0f, bounds.getRight() - 2.0f);
  } else {
    g.setColour(juce::Colours::black.withAlpha(0.28f));
    g.drawHorizontalLine((int)bounds.getY() + 1, bounds.getX() + 2.0f, bounds.getRight() - 2.0f);
  }

  g.setColour(borderCol);
  g.drawRoundedRectangle(bounds, cornerSize, 1.0f);
}

void BoutiqueLookAndFeel::drawButtonText(juce::Graphics &g, juce::TextButton &button,
                                         bool /*shouldDrawButtonAsHighlighted*/,
                                         bool shouldDrawButtonAsDown) {
  const auto& t = ThemeManager::getInstance().active();
  const auto role = Fanfare::getButtonRole(button);
  const bool isActive = button.getToggleState();

  auto bounds = button.getLocalBounds();
  if (shouldDrawButtonAsDown && role != Fanfare::ButtonRole::PluginSlot)
    bounds = bounds.translated(0, 1);

  juce::Colour textCol = t.text;
  switch (role) {
    case Fanfare::ButtonRole::Panic:
      textCol = juce::Colour(0xFFFFF8F6);
      break;
    case Fanfare::ButtonRole::Help:
      textCol = t.helpText;
      break;
    case Fanfare::ButtonRole::Foh:
      textCol = isActive ? contrastingInk(t.faderCapFoh) : t.textDim;
      break;
    case Fanfare::ButtonRole::Iem:
      textCol = isActive ? contrastingInk(t.faderCapIem) : t.textDim;
      break;
    case Fanfare::ButtonRole::Mute:
      textCol = isActive ? juce::Colour(0xFFFFF8F6) : t.textDim;
      break;
    case Fanfare::ButtonRole::Danger:
      textCol = t.danger;
      break;
    case Fanfare::ButtonRole::PluginSlot: {
      bool hasPlugin = static_cast<bool>(button.getProperties().getWithDefault("hasPlugin", false));
      textCol = hasPlugin ? t.text : (t.dark ? t.textFaint : t.textDim);
      break;
    }
    case Fanfare::ButtonRole::Primary:
      textCol = isActive ? contrastingInk(t.accent) : t.accent;
      break;
    case Fanfare::ButtonRole::Subtle:
      textCol = t.textDim;
      break;
    case Fanfare::ButtonRole::Standard:
    default:
      textCol = isActive ? contrastingInk(t.accent) : t.text;
      break;
  }

  if (!button.isEnabled())
    textCol = textCol.withMultipliedAlpha(0.45f);

  g.setColour(textCol);

  if (role == Fanfare::ButtonRole::PluginSlot) {
    bool hasPlugin = static_cast<bool>(button.getProperties().getWithDefault("hasPlugin", false));
    g.setFont(hasPlugin ? uiFont(11.5f, true) : condensedFont(11.0f, 0.06f));
    auto slotBounds = bounds.withTrimmedLeft(hasPlugin ? 7 : 4).withTrimmedRight(3);
    g.drawFittedText(hasPlugin ? button.getButtonText() : button.getButtonText().toUpperCase(),
                     slotBounds,
                     hasPlugin ? juce::Justification::centredLeft : juce::Justification::centred,
                     1);
    return;
  }

  int w = button.getWidth();
  int h = button.getHeight();
  float fontSize = 12.5f;
  float tracking = 0.05f;
  if (w < 34 || h < 22) {
    fontSize = 11.0f;
    tracking = 0.02f;
  } else if (role == Fanfare::ButtonRole::Panic) {
    fontSize = 13.5f;
    tracking = 0.08f;
  }

  g.setFont(condensedFont(fontSize, tracking));
  auto padBounds = (w < 34) ? bounds : bounds.reduced(3, 0);
  g.drawFittedText(button.getButtonText().toUpperCase(),
                   padBounds,
                   juce::Justification::centred, 1);
}

void BoutiqueLookAndFeel::drawComboBox(juce::Graphics &g, int width, int height,
                                       bool /*isButtonDown*/,
                                       int /*buttonX*/, int /*buttonY*/,
                                       int /*buttonW*/, int /*buttonH*/,
                                       juce::ComboBox &box) {
  const auto& t = ThemeManager::getInstance().active();
  juce::Rectangle<float> r(0.0f, 0.0f, (float)width, (float)height);

  if (t.dark) {
    Fanfare::ModalFrame::paintRecessedWell(g, r, 2.5f);
  } else {
    g.setColour(t.raised);
    g.fillRoundedRectangle(r, 2.5f);
    g.setColour(t.borderStrong);
    g.drawRoundedRectangle(r.reduced(0.5f), 2.5f, 1.0f);
  }

  if (box.hasKeyboardFocus(true)) {
    g.setColour(t.accent);
    g.drawRoundedRectangle(r.reduced(0.5f), 2.5f, 1.2f);
  }

  // Crisp chevron on right
  float arrowX = (float)width - 12.0f;
  float arrowY = (float)height * 0.5f;
  juce::Path chevron;
  chevron.startNewSubPath(arrowX - 3.5f, arrowY - 1.8f);
  chevron.lineTo(arrowX, arrowY + 2.2f);
  chevron.lineTo(arrowX + 3.5f, arrowY - 1.8f);
  g.setColour(box.isEnabled() ? (t.dark ? t.textDim : t.text) : t.textFaint);
  g.strokePath(chevron, juce::PathStrokeType(1.5f));
}

void BoutiqueLookAndFeel::positionComboBoxText(juce::ComboBox &box,
                                               juce::Label &label) {
  label.setBounds(6, 1, juce::jmax(10, box.getWidth() - 21), box.getHeight() - 2);
  label.setFont(getComboBoxFont(box));
  label.setColour(juce::Label::textColourId,
                  box.findColour(juce::ComboBox::textColourId));
}

void BoutiqueLookAndFeel::drawPopupMenuBackground(juce::Graphics &g, int width, int height) {
  const auto& t = ThemeManager::getInstance().active();
  juce::Rectangle<float> r(0.0f, 0.0f, (float)width, (float)height);
  g.setColour(t.panel);
  g.fillRect(r);
  g.setColour(juce::Colour(0xFF0A0A0A));
  g.drawRect(r, 2.5f);
}

void BoutiqueLookAndFeel::drawTabButton(juce::TabBarButton &button,
                                        juce::Graphics &g,
                                        bool isMouseOver,
                                        bool /*isMouseDown*/) {
  const auto& t = ThemeManager::getInstance().active();
  auto area = button.getActiveArea().toFloat();
  const bool isFront = button.isFrontTab();

  g.setColour(isFront ? t.panelAlt : (isMouseOver ? t.panel : t.sunken));
  g.fillRect(area);

  g.setColour(t.border);
  g.drawRect(area, 0.5f);

  if (isFront) {
    // 2px panel-teal active underline bar
    g.setColour(t.accent);
    g.fillRect(area.getX(), area.getBottom() - 2.0f, area.getWidth(), 2.0f);
  }

  g.setColour(isFront ? t.text : (isMouseOver ? t.text : t.textDim));
  g.setFont(condensedFont(12.0f, 0.06f));
  g.drawFittedText(button.getButtonText().trim().toUpperCase(),
                   button.getTextArea(),
                   juce::Justification::centred, 1);
}

void BoutiqueLookAndFeel::drawAlertBox(juce::Graphics &g,
                                       juce::AlertWindow &alert,
                                       const juce::Rectangle<int> &textArea,
                                       juce::TextLayout &textLayout) {
  const auto& t = ThemeManager::getInstance().active();
  auto bounds = alert.getLocalBounds();

  Fanfare::ModalFrame::paintOverlayScrimAndPanel(g, bounds, bounds, {}, 36);

  // Subtle teal accent rule along header bottom
  g.setColour(t.accentDim);
  g.fillRect(4, 35, bounds.getWidth() - 8, 1);

  g.setColour(t.text);
  textLayout.draw(g, textArea.toFloat());
}

void BoutiqueLookAndFeel::drawCallOutBoxBackground(juce::CallOutBox &box,
                                                   juce::Graphics &g,
                                                   const juce::Path &path,
                                                   juce::Image &cachedImage) {
  const auto &t = ThemeManager::getInstance().active();

  if (cachedImage.isNull()) {
    cachedImage = juce::Image(
        juce::Image::ARGB, box.getWidth(), box.getHeight(), true,
        *g.getInternalContext().getPreferredImageTypeForTemporaryImages());
    cachedImage.setBackupEnabled(false);

    juce::Graphics g2(cachedImage);
    juce::DropShadow(juce::Colours::black.withAlpha(t.dark ? 0.82f : 0.55f),
                     12, {0, 4})
        .drawForPath(g2, path);
  }

  g.setColour(juce::Colours::black);
  g.drawImageAt(cachedImage, 0, 0);

  // Solid opaque panel fill so underlying rack clutter never bleeds through
  g.setColour(t.panel);
  g.fillPath(path);

  // Bold 4px solid black outer frame around the CallOutBox card + pointer arrow
  g.setColour(juce::Colour(0xFF0A0A0A));
  g.strokePath(path, juce::PathStrokeType(4.0f, juce::PathStrokeType::curved,
                                          juce::PathStrokeType::rounded));
}

int BoutiqueLookAndFeel::getCallOutBoxBorderSize(const juce::CallOutBox &) {
  return 18;
}

void BoutiqueLookAndFeel::drawLinearSliderThumb(juce::Graphics &g, int x, int y,
                                                int width, int height,
                                                float sliderPos,
                                                float /*minSliderPos*/,
                                                float /*maxSliderPos*/,
                                                const juce::Slider::SliderStyle /*style*/,
                                                juce::Slider &slider) {
  const auto& t = ThemeManager::getInstance().active();
  auto id = slider.getComponentID();

  juce::Colour lineCol = t.accent;
  if (id == "foh" || id == "master_foh")
    lineCol = t.faderCapFoh;
  else if (id == "iem" || id == "master_iem")
    lineCol = t.faderCapIem;

  if (slider.isHorizontal()) {
    float thumbH = juce::jlimit(10.0f, 18.0f, (float)height * 0.72f);
    float thumbW = juce::jmax(9.0f, thumbH * 0.58f);
    juce::Rectangle<float> thumbRect(sliderPos - thumbW * 0.5f,
                                     (float)y + ((float)height - thumbH) * 0.5f,
                                     thumbW, thumbH);

    g.setColour(juce::Colours::black.withAlpha(0.45f));
    g.fillRoundedRectangle(thumbRect.translated(0.0f, 1.0f), 2.0f);

    juce::ColourGradient capGrad(juce::Colour(0xFFE8E4DA), thumbRect.getX(), thumbRect.getY(),
                                 juce::Colour(0xFFB5B0A4), thumbRect.getX(), thumbRect.getBottom(), false);
    g.setGradientFill(capGrad);
    g.fillRoundedRectangle(thumbRect, 2.0f);

    g.setColour(lineCol);
    g.fillRect(thumbRect.getCentreX() - 1.0f, thumbRect.getY() + 2.0f, 2.0f, thumbRect.getHeight() - 4.0f);

    g.setColour(juce::Colours::black.withAlpha(0.55f));
    g.drawRoundedRectangle(thumbRect, 2.0f, 1.0f);
    return;
  }

  // Brushed-metal vertical console fader cap (matches HUD .fader-handle)
  float thumbW = juce::jlimit(16.0f, 34.0f, (float)width * 0.76f);
  float thumbH = juce::jlimit(18.0f, 28.0f, thumbW * 0.92f);
  juce::Rectangle<float> thumbRect((float)x + ((float)width - thumbW) * 0.5f,
                                   sliderPos - thumbH * 0.5f,
                                   thumbW, thumbH);

  // Drop shadow
  g.setColour(juce::Colours::black.withAlpha(0.50f));
  g.fillRoundedRectangle(thumbRect.translated(0.0f, 2.0f), 2.5f);

  // Concave brushed-metal gradient
  juce::ColourGradient metalGrad(juce::Colour(0xFFE8E4DA), thumbRect.getX(), thumbRect.getY(),
                                 juce::Colour(0xFFD8D4CA), thumbRect.getX(), thumbRect.getBottom(), false);
  metalGrad.addColour(0.44, juce::Colour(0xFFB9B4A8));
  metalGrad.addColour(0.50, juce::Colour(0xFF8F8B80));
  metalGrad.addColour(0.56, juce::Colour(0xFFB9B4A8));
  g.setGradientFill(metalGrad);
  g.fillRoundedRectangle(thumbRect, 2.5f);

  // Top specular bevel
  g.setColour(juce::Colours::white.withAlpha(0.55f));
  g.drawHorizontalLine((int)thumbRect.getY() + 1, thumbRect.getX() + 2.0f, thumbRect.getRight() - 2.0f);

  // 2px Gold (FOH) / Cyan (IEM) / Teal indicator stripe in the concave groove
  juce::Rectangle<float> stripe(thumbRect.getX() + 3.0f,
                                thumbRect.getCentreY() - 1.25f,
                                thumbRect.getWidth() - 6.0f,
                                2.5f);
  g.setColour(juce::Colours::black.withAlpha(0.55f));
  g.fillRect(stripe.expanded(0.5f, 0.5f));
  g.setColour(lineCol);
  g.fillRect(stripe);

  g.setColour(juce::Colours::black.withAlpha(0.60f));
  g.drawRoundedRectangle(thumbRect, 2.5f, 1.0f);
}

void BoutiqueLookAndFeel::drawLinearSliderBackground(juce::Graphics &g, int x, int y,
                                                     int width, int height,
                                                     float sliderPos,
                                                     float /*minSliderPos*/,
                                                     float /*maxSliderPos*/,
                                                     const juce::Slider::SliderStyle /*style*/,
                                                     juce::Slider &slider) {
  const auto& t = ThemeManager::getInstance().active();
  auto id = slider.getComponentID();

  juce::Colour busCol = t.accent;
  if (id == "foh" || id == "master_foh")
    busCol = t.foh;
  else if (id == "iem" || id == "master_iem")
    busCol = t.iem;

  if (slider.isVertical()) {
    const float trackW = 6.0f;
    const float endY = (float)(y + height);

    // Engraved dB/level scale ticks flanking the fader slot
    for (int i = 0; i <= 10; ++i) {
      float normPos = (float)i / 10.0f;
      float yPos = endY - normPos * (float)height;
      bool major = (i % 2 == 0);
      float span = major ? 9.0f : 5.5f;
      g.setColour(major ? t.hairlineStrong : t.hairline);
      g.drawLine((float)x + width * 0.5f - trackW * 0.5f - span, yPos,
                 (float)x + width * 0.5f - trackW * 0.5f - 2.0f, yPos,
                 major ? 1.0f : 0.75f);
      g.drawLine((float)x + width * 0.5f + trackW * 0.5f + 2.0f, yPos,
                 (float)x + width * 0.5f + trackW * 0.5f + span, yPos,
                 major ? 1.0f : 0.75f);
    }

    juce::Rectangle<float> trackArea((float)x + ((float)width - trackW) * 0.5f,
                                     (float)y, trackW, (float)height);
    Fanfare::ModalFrame::paintRecessedWell(g, trackArea, 2.0f);

    // Subtle bus-coded fill inside the recessed track
    float fillTop = juce::jlimit((float)y, endY, sliderPos);
    float fillH = endY - fillTop;
    if (fillH > 1.5f) {
      juce::Rectangle<float> fillRect(trackArea.getX() + 1.0f, fillTop,
                                      trackArea.getWidth() - 2.0f, fillH - 1.0f);
      juce::ColourGradient fillGrad(busCol.withAlpha(0.80f), fillRect.getX(), fillRect.getY(),
                                    busCol.withAlpha(0.28f), fillRect.getX(), fillRect.getBottom(), false);
      g.setGradientFill(fillGrad);
      g.fillRect(fillRect);
    }
  } else {
    const float trackH = 6.0f;
    juce::Rectangle<float> trackArea((float)x,
                                     (float)y + ((float)height - trackH) * 0.5f,
                                     (float)width, trackH);
    Fanfare::ModalFrame::paintRecessedWell(g, trackArea, 2.0f);

    float fillW = juce::jlimit(0.0f, (float)width, sliderPos - (float)x);
    if (fillW > 1.5f) {
      juce::Rectangle<float> activeArea(trackArea.getX() + 1.0f, trackArea.getY() + 1.0f,
                                        fillW - 1.0f, trackArea.getHeight() - 2.0f);
      g.setColour(busCol.withAlpha(0.85f));
      g.fillRoundedRectangle(activeArea, 1.5f);
    }
  }
}

void BoutiqueLookAndFeel::drawScrollbar(
    juce::Graphics &g, juce::ScrollBar &, int x, int y, int width, int height,
    bool isScrollbarVertical, int thumbStartPosition, int thumbSize,
    bool isMouseOver, bool isMouseDown) {
  const auto &t = ThemeManager::getInstance().active();
  auto trackBounds =
      juce::Rectangle<int>(x, y, width, height).toFloat().reduced(0.5f);

  g.setColour(t.sunken);
  g.fillRoundedRectangle(trackBounds, 3.0f);
  g.setColour(t.border);
  g.drawRoundedRectangle(trackBounds, 3.0f, 1.0f);

  if (thumbSize <= 0)
    return;

  juce::Rectangle<float> thumb =
      isScrollbarVertical
          ? juce::Rectangle<float>((float)x + 1.5f, (float)thumbStartPosition + 1.0f,
                                   juce::jmax(4.0f, (float)width - 3.0f),
                                   juce::jmax(8.0f, (float)thumbSize - 2.0f))
          : juce::Rectangle<float>((float)thumbStartPosition + 1.0f, (float)y + 1.5f,
                                   juce::jmax(8.0f, (float)thumbSize - 2.0f),
                                   juce::jmax(4.0f, (float)height - 3.0f));

  const juce::Colour topCol =
      isMouseDown ? t.accent.withAlpha(0.35f).interpolatedWith(t.raised, 0.55f)
                  : (isMouseOver ? t.raised.brighter(0.18f) : t.raised.brighter(0.08f));
  const juce::Colour botCol = t.dark ? t.panelAlt : t.faceBot;

  juce::ColourGradient grad(topCol, thumb.getX(), thumb.getY(), botCol,
                            thumb.getX(), thumb.getBottom(), false);
  g.setGradientFill(grad);
  g.fillRoundedRectangle(thumb, 2.5f);

  const juce::Colour rimCol =
      isMouseDown ? t.foh
                  : (isMouseOver ? t.accent : t.textDim.withAlpha(0.65f));
  g.setColour(rimCol);
  g.drawRoundedRectangle(thumb, 2.5f, 1.0f);

  // Subtle machined center grip knurls
  g.setColour(isMouseOver ? t.text.withAlpha(0.65f) : t.engrave.withAlpha(0.80f));
  if (isScrollbarVertical && thumb.getHeight() >= 20.0f) {
    const float cy = thumb.getCentreY();
    const float x1 = thumb.getX() + 3.0f;
    const float x2 = thumb.getRight() - 3.0f;
    for (int k = -1; k <= 1; ++k)
      g.drawHorizontalLine((int)std::round(cy + k * 3.0f), x1, x2);
  } else if (!isScrollbarVertical && thumb.getWidth() >= 20.0f) {
    const float cx = thumb.getCentreX();
    const float y1 = thumb.getY() + 3.0f;
    const float y2 = thumb.getBottom() - 3.0f;
    for (int k = -1; k <= 1; ++k)
      g.drawVerticalLine((int)std::round(cx + k * 3.0f), y1, y2);
  }
}
