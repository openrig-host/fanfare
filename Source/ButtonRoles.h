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

// Label casing for the active theme. Front Panel themes engrave UPPERCASE;
// Studio uses sentence case ("SAVE SET" -> "Save set") while keeping known
// acronyms (FOH, IEM, MIDI...) and anything that isn't all-caps (plugin and
// song names) untouched.
inline juce::String toSentenceCase(const juce::String& s) {
    if (s.isEmpty() || s != s.toUpperCase() || s == s.toLowerCase())
        return s; // mixed case (user names) or no letters at all
    static const juce::StringArray acronyms {
        "FOH", "IEM", "CC", "NR", "DYN", "SMP", "MP3", "MIDI", "DSP", "EQ", "IR",
        "VST", "VST3", "CPU", "RAM", "LAT", "XRUNS", "USB", "ASIO", "HUD", "BPM",
        "DJ", "FX", "OK", "PC", "LFO", "ADSR", "UNLK", "GUI", "ID", "MS", "DB",
        "HZ", "KHZ", "WAV", "CSV", "JSON", "SQLITE", "M3U", "LPF", "HPF", "VU",
        "OFF", "ON", "A", "B", "L", "R", "M", "S", "E", "I", "F"};
    juce::StringArray words;
    words.addTokens(s, " ", "");
    bool firstAlphaDone = false;
    for (auto& w : words) {
        juce::String core = w.retainCharacters("ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789");
        if (core.isEmpty() || core.containsAnyOf("0123456789") || acronyms.contains(core)) {
            if (core.containsAnyOf("ABCDEFGHIJKLMNOPQRSTUVWXYZ"))
                firstAlphaDone = true;
            continue;
        }
        juce::String lower = w.toLowerCase();
        if (!firstAlphaDone) {
            for (int i = 0; i < lower.length(); ++i) {
                if (juce::CharacterFunctions::isLetter(lower[i])) {
                    lower = lower.substring(0, i) + lower.substring(i, i + 1).toUpperCase() +
                            lower.substring(i + 1);
                    break;
                }
            }
            firstAlphaDone = true;
        }
        w = lower;
    }
    return words.joinIntoString(" ");
}

inline juce::String themedLabel(const juce::String& s) {
    return ThemeManager::isStudio() ? toSentenceCase(s) : s.toUpperCase();
}

// Shared Front Panel chassis frame for modal overlays and inspector panels.
namespace ModalFrame {

inline void paintOverlayScrimAndPanel(juce::Graphics& g,
                                      juce::Rectangle<int> fullBounds,
                                      juce::Rectangle<int> panelBounds,
                                      const juce::String& engravedTitle = {},
                                      int headerHeight = 38,
                                      bool drawOuterBezel = true) {
    if (panelBounds != fullBounds) {
        g.setColour(ThemeManager::get(Theme::Role::scrim));
        g.fillRect(fullBounds);
    }

    auto pf = panelBounds.toFloat();
    const bool isDark = ThemeManager::getInstance().active().dark;

    // Studio: flat Nota-style card - soft shadow, 1px border, sentence-case
    // title over a hairline. No sunken header, no bezel, no engraving.
    if (ThemeManager::isStudio()) {
        if (panelBounds != fullBounds) {
            juce::DropShadow(juce::Colours::black.withAlpha(0.55f), 18, {0, 6})
                .drawForRectangle(g, panelBounds);
        }
        g.setColour(ThemeManager::get(Theme::Role::panel));
        g.fillRoundedRectangle(pf, 6.0f);
        if (headerHeight > 0) {
            g.setColour(ThemeManager::get(Theme::Role::border));
            g.fillRect(pf.getX() + 1.0f, (float)(panelBounds.getY() + headerHeight - 1),
                       pf.getWidth() - 2.0f, 1.0f);
            if (engravedTitle.isNotEmpty()) {
                g.setColour(ThemeManager::get(Theme::Role::text));
                g.setFont(juce::FontOptions(15.0f, juce::Font::bold));
                g.drawText(toSentenceCase(engravedTitle),
                           panelBounds.getX() + 16, panelBounds.getY(),
                           panelBounds.getWidth() - 160, headerHeight,
                           juce::Justification::centredLeft, true);
            }
        }
        g.setColour(ThemeManager::get(Theme::Role::borderStrong));
        g.drawRoundedRectangle(pf.reduced(0.5f), 6.0f, 1.0f);
        juce::ignoreUnused(drawOuterBezel, isDark);
        return;
    }

    // Drop shadow (visible whenever panelBounds sits inside a larger scrim)
    if (panelBounds != fullBounds) {
        g.setColour(juce::Colours::black.withAlpha(isDark ? 0.65f : 0.36f));
        g.fillRoundedRectangle(pf.translated(0.0f, 5.0f).expanded(2.0f), 6.0f);
    }

    // Main charcoal/paper panel face
    g.setColour(ThemeManager::get(Theme::Role::panel));
    g.fillRoundedRectangle(pf, 4.0f);

    // Sunken header strip
    if (headerHeight > 0) {
        const float inset = drawOuterBezel ? 3.5f : 1.0f;
        auto headerRect = panelBounds.withHeight(headerHeight).toFloat().reduced(inset, 0.0f);
        headerRect.removeFromTop(drawOuterBezel ? 3.5f : 1.0f);
        g.setColour(ThemeManager::get(Theme::Role::sunken));
        g.fillRect(headerRect);

        // Strong divider under header
        g.setColour(isDark ? juce::Colour(0xFF0A0A0A)
                           : ThemeManager::get(Theme::Role::borderStrong));
        g.fillRect(pf.getX() + inset, (float)(panelBounds.getY() + headerHeight - 1),
                   pf.getWidth() - inset * 2.0f, 1.5f);

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

    if (drawOuterBezel) {
        // Subtle inner contrast rim so the black bezel also pops cleanly in Dark mode
        g.setColour(isDark ? juce::Colours::white.withAlpha(0.18f)
                           : juce::Colours::white.withAlpha(0.75f));
        g.drawRoundedRectangle(pf.reduced(4.5f), 2.5f, 1.0f);

        // Bold 4px solid black outer frame so floating cards never blend into the rack
        g.setColour(juce::Colour(0xFF0A0A0A));
        g.drawRoundedRectangle(pf.reduced(2.0f), 4.0f, 4.0f);
    } else {
        g.setColour(ThemeManager::get(Theme::Role::borderStrong));
        g.drawRoundedRectangle(pf.reduced(0.5f), 3.0f, 1.0f);
    }
}

inline void paintRecessedWell(juce::Graphics& g, juce::Rectangle<float> bounds, float radius = 2.5f) {
    const bool isDark = ThemeManager::getInstance().active().dark;
    if (ThemeManager::isStudio()) {
        // Studio: flat field, 1px hairline, no inner shadow
        g.setColour(ThemeManager::get(Theme::Role::sunken));
        g.fillRoundedRectangle(bounds, juce::jmax(radius, 3.0f));
        g.setColour(ThemeManager::get(Theme::Role::border));
        g.drawRoundedRectangle(bounds.reduced(0.5f), juce::jmax(radius, 3.0f), 1.0f);
        return;
    }
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
