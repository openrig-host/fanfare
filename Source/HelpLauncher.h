#pragma once

#include <JuceHeader.h>
#include "ButtonRoles.h"
#include "ThemeManager.h"
#include "Version.h"

namespace Fanfare {

juce::File ensureOfflineManualExtracted();

inline void openHelpManual(const juce::String& anchor = {}) {
    juce::String frag = anchor.trim();
    if (frag.startsWithChar('#'))
        frag = frag.substring(1);

    const juce::File manualFile = ensureOfflineManualExtracted();
    if (!manualFile.existsAsFile())
        return;

    juce::String urlStr = juce::URL(manualFile).toString(false);

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
