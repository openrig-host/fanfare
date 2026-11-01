#pragma once
#include <JuceHeader.h>
#include "BoutiqueLookAndFeel.h"
#include "ButtonRoles.h"
#include "ThemeManager.h"

/**
 * Full-screen Front Panel status overlay shown while plugins load or songs switch.
 */
class LoadingOverlay : public juce::Component, public juce::Timer {
public:
  LoadingOverlay() {
    setAlwaysOnTop(true);
    startTimer(30);
  }

  void setTitle(const juce::String &t) { title = t; }
  void setMessage(const juce::String &m) { message = m; }

  void paint(juce::Graphics &g) override {
    const auto &t = ThemeManager::getInstance().active();
    auto bounds = getLocalBounds();
    auto center = bounds.getCentre();

    const int w = 360, h = 118;
    juce::Rectangle<int> box(center.x - w / 2, center.y - h / 2, w, h);

    Fanfare::ModalFrame::paintOverlayScrimAndPanel(
        g, bounds, box,
        title.isEmpty() ? "ARMING STAGE SONG" : title,
        34);

    // Body message in recessed status plate
    auto bodyArea = box.withTrimmedTop(34).reduced(14, 12);
    Fanfare::ModalFrame::paintRecessedWell(g, bodyArea.toFloat(), 2.5f);

    g.setColour(t.text);
    g.setFont(BoutiqueLookAndFeel::uiFont(13.5f, false));
    g.drawText(message.isEmpty() ? "Warming up VSTs & stage patches..." : message,
               bodyArea.reduced(10, 4), juce::Justification::centred, true);

    // Hardware activity sweep bar along bottom of well
    double phase = std::fmod(juce::Time::getMillisecondCounterHiRes() * 0.0012, 1.0);
    float barW = bodyArea.getWidth() * 0.32f;
    float travel = (float)bodyArea.getWidth() - barW - 4.0f;
    float pingPong = (float)(phase < 0.5 ? (phase * 2.0) : (2.0 - phase * 2.0));
    float barX = (float)bodyArea.getX() + 2.0f + travel * pingPong;

    g.setColour(t.accent);
    g.fillRect(barX, (float)bodyArea.getBottom() - 3.0f, barW, 2.0f);
  }

  void timerCallback() override { repaint(); }

  void mouseDown(const juce::MouseEvent &) override {}
  void mouseUp(const juce::MouseEvent &) override {}
  void mouseDrag(const juce::MouseEvent &) override {}
  void mouseMove(const juce::MouseEvent &) override {}

private:
  juce::String title;
  juce::String message;

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(LoadingOverlay)
};
