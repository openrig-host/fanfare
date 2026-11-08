#pragma once
// Internal shared declarations for the MainComponent translation units
// (split out of MainComponent.cpp mechanically; do not edit by hand).
#include <JuceHeader.h>
#include "ThemeManager.h"
#include "Logger.h"

namespace {
// Procedurally renders a gear icon for the settings ImageButton (no asset dep).
juce::Image createGearImage(int size, juce::Colour colour) {
  juce::Image img(juce::Image::ARGB, size, size, true);
  juce::Graphics g(img);
  g.setColour(colour);

  const float cx = size * 0.5f;
  const float cy = size * 0.5f;
  const float bodyR = size * 0.34f;
  const float toothR = size * 0.46f;
  const int teeth = 8;

  juce::Path gear;
  for (int i = 0; i < teeth; ++i) {
    float a0 = juce::MathConstants<float>::twoPi * (i + 0.08f) / teeth;
    float a1 = juce::MathConstants<float>::twoPi * (i + 0.42f) / teeth;
    float a2 = juce::MathConstants<float>::twoPi * (i + 0.5f) / teeth;
    float a3 = juce::MathConstants<float>::twoPi * (i + 0.92f) / teeth;
    auto p = [&](float ang, float r) {
      gear.lineTo(cx + r * std::cos(ang), cy + r * std::sin(ang));
    };
    if (i == 0)
      gear.startNewSubPath(cx + bodyR * std::cos(a0), cy + bodyR * std::sin(a0));
    p(a0, bodyR);
    p(a1, toothR);
    p(a2, toothR);
    p(a3, bodyR);
  }
  gear.closeSubPath();
  g.fillPath(gear);

  // Center hole (drawn in panel colour to read as a hole on the dark header)
  g.setColour(ThemeManager::get(Theme::Role::background));
  g.fillEllipse(cx - size * 0.16f, cy - size * 0.16f, size * 0.32f,
                size * 0.32f);
  return img;
}
} // namespace

class PluginWindow : public juce::DocumentWindow {
public:
  std::function<void()> onWindowClosed;
  juce::AudioPluginInstance* pluginInstance = nullptr;

  PluginWindow(const juce::String &name, juce::Component *content, juce::AudioPluginInstance* pi)
      : DocumentWindow(name, ThemeManager::get(Theme::Role::panel),
                       DocumentWindow::closeButton |
                           DocumentWindow::minimiseButton),
        pluginInstance(pi) {
    setContentOwned(content, true);
    setResizable(true, false);
    setUsingNativeTitleBar(true);
    centreWithSize(content->getWidth(), content->getHeight());
    setVisible(true);
    toFront(true);
  }

  // Tear down the hosted VST3 IPlugView / AudioProcessorEditor synchronously
  // while both the parent DocumentWindow HWND and the owning AudioPluginInstance
  // are still alive (avoiding use-after-free when unloading plugins).
  void destroyEditorNow() {
    if (getContentComponent() != nullptr) {
      FanfareLog::safeExecutePluginCall([this]() {
        clearContentComponent();
      }, "PluginEditor destructor");
    }
    setVisible(false);
    pluginInstance = nullptr;
  }

  ~PluginWindow() override {
    destroyEditorNow();
  }

  void closeButtonPressed() override {
    if (onWindowClosed)
      onWindowClosed();
  }

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PluginWindow)
};
