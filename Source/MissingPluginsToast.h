#pragma once
#include <JuceHeader.h>
#include "BoutiqueLookAndFeel.h"
#include "ButtonRoles.h"
#include "ThemeManager.h"

struct MissingPluginInfo {
    juce::String name;
    juce::String location;
    juce::String path;
    juce::String reason;
};

/**
 * Silent, non-modal Front Panel toast surfacing uninstalled or stalled plugins required by a song.
 */
class MissingPluginsToast : public juce::Component, public juce::Timer {
public:
    MissingPluginsToast(const std::vector<MissingPluginInfo>& missingList,
                        std::function<void()> onDismissCallback,
                        std::function<void()> onDiagnoseCallback = {})
        : items(missingList),
          onDismiss(std::move(onDismissCallback)),
          onDiagnose(std::move(onDiagnoseCallback))
    {
        setAlwaysOnTop(true);
        for (const auto& it : items) {
            const bool fileExists = it.path.isNotEmpty() && juce::File(it.path).existsAsFile();
            if (fileExists || it.reason.containsIgnoreCase("stall") ||
                it.reason.containsIgnoreCase("timed out") ||
                it.reason.containsIgnoreCase("wedged")) {
                hasStallOrFault = true;
            }
        }
        startTimer(100);
    }

    ~MissingPluginsToast() override {
        stopTimer();
    }

    int getDesiredHeight() const {
        return 36 + 22 + (int)items.size() * 22 + 32 + 18;
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

        // 3px amber/danger warning stripe along top edge
        g.setColour(hasStallOrFault ? t.danger : t.warn);
        g.fillRect(bounds.getX() + 1.0f, bounds.getY() + 1.0f, bounds.getWidth() - 2.0f, 3.0f);

        g.setColour(t.borderStrong);
        g.drawRoundedRectangle(bounds.reduced(0.5f), 3.0f, 1.0f);

        auto inner = bounds.reduced(14.0f, 10.0f);

        // Header
        auto headerArea = inner.removeFromTop(22.0f);
        g.setColour(hasStallOrFault ? t.danger : t.warn);
        g.setFont(BoutiqueLookAndFeel::condensedFont(14.5f, 0.06f));
        g.drawText(hasStallOrFault ? "PLUGIN LOAD ISSUES DETECTED"
                                   : "MISSING PLUGINS (NOT INSTALLED)",
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
        g.drawText(hasStallOrFault
                       ? "Some plugins in this song failed or timed out while loading:"
                       : "This song calls for plugins not installed on this machine:",
                   subArea, juce::Justification::centredLeft, true);

        inner.removeFromTop(4.0f);

        // Plugin item list
        for (const auto& item : items) {
            auto itemArea = inner.removeFromTop(22.0f);

            // Small indicator bar
            const bool fileExists = item.path.isNotEmpty() && juce::File(item.path).existsAsFile();
            g.setColour(fileExists ? t.danger : t.warn);
            g.fillRect(itemArea.getX() + 2.0f, itemArea.getCentreY() - 3.0f, 3.0f, 6.0f);
            itemArea.removeFromLeft(12.0f);

            auto nameFont = BoutiqueLookAndFeel::uiFont(13.0f, true);
            g.setColour(t.text);
            g.setFont(nameFont);
            float nameWidth = juce::jmin(itemArea.getWidth() * 0.45f,
                                         juce::GlyphArrangement::getStringWidth(nameFont, item.name) + 8.0f);
            g.drawText(item.name, itemArea.removeFromLeft(nameWidth), juce::Justification::centredLeft, true);

            juce::String detail = "[" + item.location + "]";
            if (item.reason.isNotEmpty())
                detail << " - " << item.reason;
            else if (!fileExists)
                detail << " - not installed";

            g.setColour(t.engrave);
            g.setFont(BoutiqueLookAndFeel::monoFont(11.0f, false));
            g.drawText(detail, itemArea, juce::Justification::centredLeft, true);
        }

        // Footer
        auto footerArea = inner.removeFromTop(26.0f);
        dismissButtonArea = juce::Rectangle<float>(bounds.getRight() - 92.0f, bounds.getBottom() - 30.0f, 78.0f, 22.0f);

        if (onDiagnose) {
            diagnoseButtonArea = juce::Rectangle<float>(dismissButtonArea.getX() - 118.0f,
                                                        dismissButtonArea.getY(), 110.0f, 22.0f);
            g.setColour(t.accent.withAlpha(0.20f));
            g.fillRoundedRectangle(diagnoseButtonArea, 3.0f);
            g.setColour(t.accent);
            g.drawRoundedRectangle(diagnoseButtonArea, 3.0f, 1.0f);
            g.setFont(BoutiqueLookAndFeel::condensedFont(11.5f, 0.06f));
            g.drawText("RIG DOCTOR...", diagnoseButtonArea, juce::Justification::centred, false);
        } else {
            diagnoseButtonArea = {};
        }

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
            const float rightTrim = onDiagnose ? 215.0f : 95.0f;
            g.drawText("Auto-closing in " + juce::String(remainingSecs) + "s",
                       footerArea.withTrimmedRight(rightTrim), juce::Justification::centredLeft, true);
        }
    }

    void mouseUp(const juce::MouseEvent& e) override {
        auto pt = e.position;
        if (onDiagnose && diagnoseButtonArea.contains(pt)) {
            stopTimer();
            auto diagCb = onDiagnose;
            dismiss();
            diagCb();
            return;
        }
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
    std::function<void()> onDiagnose;
    juce::Rectangle<float> closeButtonArea;
    juce::Rectangle<float> dismissButtonArea;
    juce::Rectangle<float> diagnoseButtonArea;
    bool hasStallOrFault = false;
    int elapsedMs = 0;
    static constexpr int autoDismissMs = 15000;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MissingPluginsToast)
};
