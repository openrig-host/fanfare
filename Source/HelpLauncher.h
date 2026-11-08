#pragma once

#include <JuceHeader.h>
#include "ButtonRoles.h"
#include "ThemeManager.h"
#include "Version.h"

namespace Fanfare {

inline void openHelpManual(const juce::String& anchor = {}) {
    juce::String frag = anchor.trim();
    if (frag.startsWithChar('#'))
        frag = frag.substring(1);

    // manual.html + its CSS/JS are compiled directly into EmbeddedWebAssets, so
    // the local server always serves the latest v1.50 manual offline from any
    // folder (including a standalone Fanfare.exe on the Desktop).
    juce::String urlStr = "http://127.0.0.1:8080/manual.html?v=" OPENRIG_VERSION_STRING;

    if (frag.isNotEmpty())
        urlStr += "#" + frag;

    juce::URL(urlStr).launchInDefaultBrowser();
}

inline void styleHelpButton(juce::TextButton& btn, const juce::String& anchor, const juce::String& tooltipText = {}) {
    btn.setButtonText("Help");
    setButtonRole(btn, ButtonRole::Help);
    btn.setColour(juce::TextButton::buttonColourId, ThemeManager::get(Theme::Role::helpFaceTop));
    btn.setColour(juce::TextButton::textColourOffId, ThemeManager::get(Theme::Role::helpText));
    btn.setTooltip(tooltipText.isNotEmpty()
                       ? tooltipText.replace("HAAALP! ", "")
                       : "Open the Fanfare Stage Manual to this exact section");
    btn.onClick = [anchor]() {
        openHelpManual(anchor);
    };
}

} // namespace Fanfare
