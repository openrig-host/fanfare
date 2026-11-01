#pragma once

#include <JuceHeader.h>
#include "ThemeManager.h"

namespace Fanfare {

enum class ButtonRole : int {
    Standard = 0,
    Primary,
    Panic,
    Help,
    Mute,
    Foh,
    Iem,
    PluginSlot,
    Danger,
    Subtle
};

inline void setButtonRole(juce::Button& btn, ButtonRole role) {
    btn.getProperties().set("fanfareRole", static_cast<int>(role));
}

inline ButtonRole getButtonRole(const juce::Button& btn) {
    const auto& props = btn.getProperties();
    if (props.contains("fanfareRole"))
        return static_cast<ButtonRole>(static_cast<int>(props["fanfareRole"]));
    if (static_cast<bool>(props.getWithDefault("isPluginSlot", false)))
        return ButtonRole::PluginSlot;
    return ButtonRole::Standard;
}

// Shared Front Panel chassis frame for modal overlays and inspector panels.
namespace ModalFrame {

inline void paintOverlayScrimAndPanel(juce::Graphics& g,
                                      juce::Rectangle<int> fullBounds,
                                      juce::Rectangle<int> panelBounds,
                                      const juce::String& engravedTitle = {},
                                      int headerHeight = 38) {
    if (panelBounds != fullBounds) {
        g.setColour(ThemeManager::get(Theme::Role::scrim));
        g.fillRect(fullBounds);
    }

    auto pf = panelBounds.toFloat();
    const bool isDark = ThemeManager::getInstance().active().dark;

    // Drop shadow
    g.setColour(juce::Colours::black.withAlpha(isDark ? 0.50f : 0.24f));
    g.fillRoundedRectangle(pf.translated(0.0f, 3.0f), 4.0f);

    // Main charcoal/paper panel face
    g.setColour(ThemeManager::get(Theme::Role::panel));
    g.fillRoundedRectangle(pf, 3.0f);

    // Sunken header strip
    if (headerHeight > 0) {
        auto headerRect = panelBounds.withHeight(headerHeight).toFloat();
        g.setColour(ThemeManager::get(Theme::Role::sunken));
        g.fillRoundedRectangle(headerRect, 3.0f);
        g.fillRect(headerRect.withTrimmedTop(3.0f));

        // Hairline divider under header
        g.setColour(ThemeManager::get(Theme::Role::hairlineStrong));
        g.drawHorizontalLine(panelBounds.getY() + headerHeight - 1,
                             pf.getX() + 1.0f, pf.getRight() - 1.0f);

        if (engravedTitle.isNotEmpty()) {
            g.setColour(ThemeManager::get(Theme::Role::text));
            auto f = juce::Font(juce::FontOptions("Barlow Condensed", 15.5f, juce::Font::bold)
                                    .withKerningFactor(0.08f));
            g.setFont(f);
            g.drawText(engravedTitle.toUpperCase(),
                       panelBounds.getX() + 14, panelBounds.getY(),
                       panelBounds.getWidth() - 150, headerHeight,
                       juce::Justification::centredLeft, true);
        }
    }

    // Top bevel highlight + outer hairline border
    g.setColour(ThemeManager::get(Theme::Role::bevelHi));
    g.drawHorizontalLine(panelBounds.getY() + 1, pf.getX() + 2.0f, pf.getRight() - 2.0f);

    g.setColour(ThemeManager::get(Theme::Role::borderStrong));
    g.drawRoundedRectangle(pf.reduced(0.5f), 3.0f, 1.0f);
}

inline void paintRecessedWell(juce::Graphics& g, juce::Rectangle<float> bounds, float radius = 2.5f) {
    const bool isDark = ThemeManager::getInstance().active().dark;
    g.setColour(ThemeManager::get(Theme::Role::sunken));
    g.fillRoundedRectangle(bounds, radius);

    // Inner top shadow for recessed LCD/meter depth
    g.setColour(juce::Colours::black.withAlpha(isDark ? 0.32f : 0.08f));
    g.drawHorizontalLine((int)bounds.getY() + 1, bounds.getX() + 1.0f, bounds.getRight() - 1.0f);

    g.setColour(isDark ? ThemeManager::get(Theme::Role::hairline)
                       : ThemeManager::get(Theme::Role::border));
    g.drawRoundedRectangle(bounds.reduced(0.5f), radius, 1.0f);
}

} // namespace ModalFrame

} // namespace Fanfare
