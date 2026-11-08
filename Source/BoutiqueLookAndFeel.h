#pragma once
#include <JuceHeader.h>
#include "AssetLoader.h"
#include "ButtonRoles.h"
#include "ThemeManager.h"

/**
 * BoutiqueLookAndFeel
 * Two render styles, chosen by the active theme:
 *  - Studio (default, Theme::isStudio): Nota-style flat surfaces, 1px
 *    hairlines, sentence-case Source Sans 3 labels, JetBrains Mono numbers,
 *    one orange accent for active state only.
 *  - Front Panel: the hardware aesthetic shared with the Tablet Stage HUD:
 *    charcoal/paper panel faces, hairline borders, top bevel
 * highlights, engraved Barlow Condensed labels, Source Sans 3 body text,
 * gold FOH / cyan IEM bus coding, and brushed-metal fader caps.
 */
class BoutiqueLookAndFeel : public juce::LookAndFeel_V4 {
public:
  bool useModernStyle = true;

  BoutiqueLookAndFeel();

  // Embedded SIL OFL typeface accessors
  static juce::Typeface::Ptr getCondensedTypeface();
  static juce::Typeface::Ptr getUiRegularTypeface();
  static juce::Typeface::Ptr getUiBoldTypeface();
  static juce::Typeface::Ptr getMonoTypeface(bool bold = false);

  static juce::Font condensedFont(float height, float kerning = 0.05f);
  static juce::Font uiFont(float height, bool bold = false);
  static juce::Font monoFont(float height, bool bold = false);

  // Slider value boxes render in the mono face to match telemetry readouts.
  juce::Label *createSliderTextBox(juce::Slider &slider) override;

  // Reusable segmented LED meter renderer (red zone anchored to top of scale)
  static void drawSegmentedMeterBar(juce::Graphics& g,
                                    juce::Rectangle<float> barArea,
                                    float levelNormalized,
                                    int numSegments,
                                    juce::Colour lowCol,
                                    juce::Colour midCol,
                                    juce::Colour highCol);

  static void drawStereoSegmentedMeter(juce::Graphics& g,
                                       juce::Rectangle<float> area,
                                       float leftNorm,
                                       float rightNorm,
                                       int numSegments,
                                       juce::Colour lowCol,
                                       juce::Colour midCol,
                                       juce::Colour highCol);

  void applyThemeColors();

  juce::Typeface::Ptr getTypefaceForFont(const juce::Font& font) override;
  juce::Font getTextButtonFont(juce::TextButton&, int buttonHeight) override;
  juce::Font getComboBoxFont(juce::ComboBox&) override;
  juce::Font getPopupMenuFont() override;
  juce::Font getAlertWindowTitleFont() override;
  juce::Font getAlertWindowMessageFont() override;
  juce::Font getAlertWindowFont() override;

  void drawToggleSwitch(juce::Graphics &g, juce::ToggleButton &button,
                        const juce::String &svgAsset, bool isOrange = false);

  void drawSkeuomorphicButtonBackground(juce::Graphics &g, juce::Button &button,
                                        const juce::Colour &baseColour);

  void drawRotarySlider(juce::Graphics &g, int x, int y, int width, int height,
                        float sliderPos, const float rotaryStartAngle,
                        const float rotaryEndAngle,
                        juce::Slider &slider) override;

  void drawButtonBackground(juce::Graphics &g, juce::Button &button,
                            const juce::Colour &backgroundColour,
                            bool shouldDrawButtonAsHighlighted,
                            bool shouldDrawButtonAsDown) override;

  void drawButtonText(juce::Graphics &g, juce::TextButton &button,
                      bool shouldDrawButtonAsHighlighted,
                      bool shouldDrawButtonAsDown) override;

  void drawToggleButton(juce::Graphics &g, juce::ToggleButton &button,
                        bool shouldDrawButtonAsHighlighted,
                        bool shouldDrawButtonAsDown) override;

  void drawComboBox(juce::Graphics &g, int width, int height, bool isButtonDown,
                    int buttonX, int buttonY, int buttonW, int buttonH,
                    juce::ComboBox &box) override;

  void positionComboBoxText(juce::ComboBox &box, juce::Label &label) override;

  void drawPopupMenuBackground(juce::Graphics &g, int width, int height) override;

  // Menu bar (File / View / Rig / Tools / Help) - flat, text-only items
  juce::Font getMenuBarFont(juce::MenuBarComponent &, int itemIndex, const juce::String &itemText) override;
  void drawMenuBarBackground(juce::Graphics &g, int width, int height, bool isMouseOverBar,
                             juce::MenuBarComponent &) override;
  void drawMenuBarItem(juce::Graphics &g, int width, int height, int itemIndex,
                       const juce::String &itemText, bool isMouseOverItem, bool isMenuOpen,
                       bool isMouseOverBar, juce::MenuBarComponent &) override;

  void drawScrollbar(juce::Graphics &g, juce::ScrollBar &scrollbar, int x, int y,
                     int width, int height, bool isScrollbarVertical,
                     int thumbStartPosition, int thumbSize, bool isMouseOver,
                     bool isMouseDown) override;

  void drawTabButton(juce::TabBarButton &button, juce::Graphics &g,
                     bool isMouseOver, bool isMouseDown) override;

  void drawAlertBox(juce::Graphics &g, juce::AlertWindow &alert,
                    const juce::Rectangle<int> &textArea,
                    juce::TextLayout &textLayout) override;

  void drawCallOutBoxBackground(juce::CallOutBox &box, juce::Graphics &g,
                                const juce::Path &path,
                                juce::Image &cachedImage) override;

  int getCallOutBoxBorderSize(const juce::CallOutBox &box) override;

  void drawLinearSlider(juce::Graphics &g, int x, int y, int width, int height,
                        float sliderPos, float minSliderPos, float maxSliderPos,
                        const juce::Slider::SliderStyle style,
                        juce::Slider &slider) override {
    drawLinearSliderBackground(g, x, y, width, height, sliderPos, minSliderPos,
                               maxSliderPos, style, slider);
    drawLinearSliderThumb(g, x, y, width, height, sliderPos, minSliderPos,
                          maxSliderPos, style, slider);
  }

  void drawLinearSliderThumb(juce::Graphics &g, int x, int y, int width,
                             int height, float sliderPos,
                             float minSliderPos, float maxSliderPos,
                             const juce::Slider::SliderStyle style,
                             juce::Slider &slider) override;

  void drawLinearSliderBackground(juce::Graphics &g, int x, int y, int width,
                                  int height, float sliderPos,
                                  float minSliderPos, float maxSliderPos,
                                  const juce::Slider::SliderStyle style,
                                  juce::Slider &slider) override;

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(BoutiqueLookAndFeel)

protected:
  std::unique_ptr<juce::Drawable> knobSvg;
  std::unique_ptr<juce::Drawable> toggleSwitchSvg;
  std::unique_ptr<juce::Drawable> toggleOrangeSvg;

  juce::Drawable* getCachedSvg(std::unique_ptr<juce::Drawable>& cache,
                               const juce::String& name) {
    if (!cache)
      cache = AssetLoader::getInstance().loadSVG(name);
    return cache.get();
  }
};
