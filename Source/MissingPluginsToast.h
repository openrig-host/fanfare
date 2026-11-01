#pragma once
#include <JuceHeader.h>
#include "BoutiqueLookAndFeel.h"
#include "ButtonRoles.h"
#include "ThemeManager.h"

struct MissingPluginInfo {
    juce::String name;
    juce::String location;
    juce::String path;
};

/**
 * Silent, non-modal Front Panel toast surfacing uninstalled plugins required by a song.
 */
class MissingPluginsToast : public juce::Component, public juce::Timer {
public:
    MissingPluginsToast(const std::vector<MissingPluginInfo>& missingList, std::function<void()> onDismissCallback)
        : items(missingList), onDismiss(std::move(onDismissCallback))
    {
        setAlwaysOnTop(true);
        startTimer(100);
    }

    ~MissingPluginsToast() override {
        stopTimer();
    }

    int getDesiredHeight() const {
        return 36 + 22 + (int)items.size() * 22 + 30 + 18;
    }

    void paint(juce::Graphics& g) override {
        const auto& t = ThemeManager::getInstance().active();
        auto bounds = getLocalBounds().toFloat().reduced(2.0f);

        // Drop shadow
        g.setColour(juce::Colours::black.withAlpha(0.50f));
        g.fillRoundedRectangle(bounds.translated(0.0f, 3.0f), 4.0f);

        // Front Panel chassis
        g.setColour(t.panel);
        g.fillRoundedRectangle(bounds, 3.0f);

        // 3px amber warning stripe along top edge
        g.setColour(t.warn);
        g.fillRect(bounds.getX() + 1.0f, bounds.getY() + 1.0f, bounds.getWidth() - 2.0f, 3.0f);

        g.setColour(t.borderStrong);
        g.drawRoundedRectangle(bounds.reduced(0.5f), 3.0f, 1.0f);

        auto inner = bounds.reduced(14.0f, 10.0f);

        // Header
        auto headerArea = inner.removeFromTop(22.0f);
        g.setColour(t.warn);
        g.setFont(BoutiqueLookAndFeel::condensedFont(14.5f, 0.06f));
        g.drawText("MISSING PLUGINS (NOT INSTALLED)",
                   headerArea.withTrimmedRight(32.0f), juce::Justification::centredLeft, true);

        // Close 'X'
        closeButtonArea = juce::Rectangle<float>(bounds.getRight() - 32.0f, bounds.getY() + 7.0f, 24.0f, 22.0f);
        g.setColour(t.textDim);
        g.setFont(BoutiqueLookAndFeel::condensedFont(13.5f, 0.02f));
        g.drawText("X", closeButtonArea, juce::Justification::centred, false);

        // Subtitle
        auto subArea = inner.removeFromTop(18.0f);
        g.setColour(t.textDim);
        g.setFont(BoutiqueLookAndFeel::uiFont(12.5f, false));
        g.drawText("This song calls for plugins not installed on this machine:", subArea, juce::Justification::centredLeft, true);

        inner.removeFromTop(4.0f);

        // Plugin item list
        for (const auto& item : items) {
            auto itemArea = inner.removeFromTop(22.0f);

            // Small amber indicator bar instead of bullet glyph
            g.setColour(t.warn);
            g.fillRect(itemArea.getX() + 2.0f, itemArea.getCentreY() - 3.0f, 3.0f, 6.0f);
            itemArea.removeFromLeft(12.0f);

            auto nameFont = BoutiqueLookAndFeel::uiFont(13.0f, true);
            g.setColour(t.text);
            g.setFont(nameFont);
            float nameWidth = nameFont.getStringWidthFloat(item.name) + 8.0f;
            g.drawText(item.name, itemArea.removeFromLeft(nameWidth), juce::Justification::centredLeft, true);

            g.setColour(t.engrave);
            g.setFont(BoutiqueLookAndFeel::monoFont(11.5f, false));
            g.drawText("[" + item.location + "]", itemArea, juce::Justification::centredLeft, true);
        }

        // Footer
        auto footerArea = inner.removeFromTop(24.0f);
        dismissButtonArea = juce::Rectangle<float>(bounds.getRight() - 92.0f, bounds.getBottom() - 30.0f, 78.0f, 22.0f);

        juce::ColourGradient capGrad(t.faceTop, dismissButtonArea.getX(), dismissButtonArea.getY(),
                                     t.faceBot, dismissButtonArea.getX(), dismissButtonArea.getBottom(), false);
        g.setGradientFill(capGrad);
        g.fillRoundedRectangle(dismissButtonArea, 3.0f);
        g.setColour(t.border);
        g.drawRoundedRectangle(dismissButtonArea, 3.0f, 1.0f);

        g.setColour(t.text);
        g.setFont(BoutiqueLookAndFeel::condensedFont(11.5f, 0.06f));
        g.drawText("DISMISS", dismissButtonArea, juce::Justification::centred, false);

        int remainingSecs = (int)std::ceil((autoDismissMs - elapsedMs) / 1000.0f);
        if (remainingSecs > 0) {
            g.setColour(t.textFaint);
            g.setFont(BoutiqueLookAndFeel::uiFont(11.5f, false));
            g.drawText("Silent notice - auto-closing in " + juce::String(remainingSecs) + "s",
                       footerArea.withTrimmedRight(95.0f), juce::Justification::centredLeft, true);
        }
    }

    void mouseUp(const juce::MouseEvent&) override {
        dismiss();
    }

    void timerCallback() override {
        elapsedMs += 100;
        if (elapsedMs >= autoDismissMs) {
            stopTimer();
            dismiss();
            return;
        }
        repaint();
    }

    void dismiss() {
        stopTimer();
        if (onDismiss) {
            auto cb = std::move(onDismiss);
            onDismiss = nullptr;
            cb();
        }
    }

private:
    std::vector<MissingPluginInfo> items;
    std::function<void()> onDismiss;
    juce::Rectangle<float> closeButtonArea;
    juce::Rectangle<float> dismissButtonArea;
    int elapsedMs = 0;
    static constexpr int autoDismissMs = 12000;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MissingPluginsToast)
};
