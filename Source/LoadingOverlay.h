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
    cancelBtn.setButtonText("CANCEL");
    Fanfare::setButtonRole(cancelBtn, Fanfare::ButtonRole::Subtle);
    cancelBtn.setVisible(false);
    cancelBtn.onClick = [this] {
      if (onCancel) {
        cancelBtn.setEnabled(false);
        cancelBtn.setButtonText("CANCELLING...");
        onCancel();
      }
    };
    addChildComponent(cancelBtn);
    startTimer(30);
  }

  void resetProgress() {
    completedSteps = 0;
    totalSteps = 0;
  }

  void setProgressSteps(int completed, int total) {
    totalSteps = juce::jmax(0, total);
    completedSteps = juce::jlimit(0, totalSteps, completed);
    repaint();
  }

  void setTitle(const juce::String &t) { title = t; }
  void setMessage(const juce::String &m) {
    message = m;
    updateStepStateFromMessage(m);
  }

  void setOnCancel(std::function<void()> cb) {
    onCancel = std::move(cb);
    cancelBtn.setEnabled(true);
    cancelBtn.setButtonText("CANCEL");
    cancelBtn.setVisible((bool)onCancel);
    resized();
    repaint();
  }

  void resized() override {
    auto bounds = getLocalBounds();
    auto center = bounds.getCentre();
    const bool hasCancel = (bool)onCancel;
    const int w = 448, h = hasCancel ? 156 : 122;
    juce::Rectangle<int> box(center.x - w / 2, center.y - h / 2, w, h);
    if (hasCancel) {
      cancelBtn.setBounds(center.x - 54, box.getBottom() - 34, 108, 24);
    }
  }

  void paint(juce::Graphics &g) override {
    const auto &t = ThemeManager::getInstance().active();
    auto bounds = getLocalBounds();
    auto center = bounds.getCentre();

    const bool hasCancel = (bool)onCancel;
    const int w = 448, h = hasCancel ? 156 : 122;
    juce::Rectangle<int> box(center.x - w / 2, center.y - h / 2, w, h);

    Fanfare::ModalFrame::paintOverlayScrimAndPanel(
        g, bounds, box,
        title.isEmpty() ? "ARMING STAGE SONG" : title,
        34);

    // Body message in recessed status plate
    auto bodyArea = box.withTrimmedTop(34)
                       .withTrimmedBottom(hasCancel ? 40 : 12)
                       .reduced(14, 0)
                       .withTrimmedTop(10);
    Fanfare::ModalFrame::paintRecessedWell(g, bodyArea.toFloat(), 2.5f);

    g.setColour(t.text);
    g.setFont(BoutiqueLookAndFeel::uiFont(13.0f, false));
    auto textArea = (totalSteps > 0 ? bodyArea.withTrimmedBottom(12) : bodyArea).reduced(10, 6);
    g.drawFittedText(message.isEmpty() ? "Warming up VSTs & stage patches..." : message,
                     textArea, juce::Justification::centred, 2, 0.92f);

    if (totalSteps > 0) {
      // Discrete N-segment LED progress bar: 1 segment per build step.
      const float padX = 8.0f;
      const float trackX = (float)bodyArea.getX() + padX;
      const float trackW = (float)bodyArea.getWidth() - padX * 2.0f;
      const float barH = 5.0f;
      const float barY = (float)bodyArea.getBottom() - barH - 5.0f;
      const float gap = (totalSteps > 28) ? 1.5f : (totalSteps > 16 ? 2.0f : 3.0f);
      const float segW = juce::jmax(2.0f, (trackW - gap * (float)(totalSteps - 1)) / (float)totalSteps);

      const double tSec = juce::Time::getMillisecondCounterHiRes() * 0.001;
      const float pulseAlpha = 0.32f + 0.53f * (0.5f + 0.5f * (float)std::sin(tSec * 5.5));
      const juce::Colour unlitCol = t.text.withAlpha(t.dark ? 0.14f : 0.12f);

      for (int i = 0; i < totalSteps; ++i) {
        const float sx = trackX + (float)i * (segW + gap);
        juce::Rectangle<float> segRect(sx, barY, segW, barH);
        g.setColour(unlitCol);
        g.fillRoundedRectangle(segRect, 1.2f);

        if (i < completedSteps) {
          g.setColour(t.accent);
          g.fillRoundedRectangle(segRect, 1.2f);
        } else if (i == completedSteps && completedSteps < totalSteps) {
          g.setColour(t.accent.withAlpha(pulseAlpha));
          g.fillRoundedRectangle(segRect, 1.2f);
        }
      }
    } else {
      // Indeterminate sweep bar before total step count is known
      double phase = std::fmod(juce::Time::getMillisecondCounterHiRes() * 0.0012, 1.0);
      float barW = bodyArea.getWidth() * 0.32f;
      float travel = (float)bodyArea.getWidth() - barW - 4.0f;
      float pingPong = (float)(phase < 0.5 ? (phase * 2.0) : (2.0 - phase * 2.0));
      float barX = (float)bodyArea.getX() + 2.0f + travel * pingPong;

      g.setColour(t.accent);
      g.fillRect(barX, (float)bodyArea.getBottom() - 3.0f, barW, 2.0f);
    }
  }

  void timerCallback() override { repaint(); }

  void mouseDown(const juce::MouseEvent &) override {}
  void mouseUp(const juce::MouseEvent &) override {}
  void mouseDrag(const juce::MouseEvent &) override {}
  void mouseMove(const juce::MouseEvent &) override {}

private:
  void updateStepStateFromMessage(const juce::String &m) {
    int openIdx = m.indexOfChar('[');
    while (openIdx >= 0) {
      int closeIdx = m.indexOfChar(openIdx + 1, ']');
      if (closeIdx < 0)
        break;
      juce::String inside = m.substring(openIdx + 1, closeIdx).trim();
      int slashIdx = inside.indexOfChar('/');
      if (slashIdx > 0 && slashIdx < inside.length() - 1) {
        juce::String left = inside.substring(0, slashIdx).trim();
        juce::String right = inside.substring(slashIdx + 1).trim();
        if (left.containsOnly("0123456789") && right.containsOnly("0123456789")) {
          int cur = left.getIntValue();
          int tot = right.getIntValue();
          if (tot > 0) {
            totalSteps = tot;
            completedSteps = juce::jlimit(0, tot, cur - 1);
            return;
          }
        }
      }
      openIdx = m.indexOfChar(closeIdx + 1, '[');
    }
    if (totalSteps > 0 && m.trimStart().startsWithIgnoreCase("Applying")) {
      completedSteps = totalSteps;
    }
  }

  juce::String title;
  juce::String message;
  int completedSteps = 0;
  int totalSteps = 0;
  juce::TextButton cancelBtn;
  std::function<void()> onCancel;

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(LoadingOverlay)
};
