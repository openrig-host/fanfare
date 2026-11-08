#pragma once

#include <JuceHeader.h>
#include "ButtonRoles.h"
#include "ThemeManager.h"

namespace Fanfare {

inline void openHelpManual(const juce::String& anchor = {}) {
    juce::String frag = anchor.trim();
    if (frag.startsWithChar('#'))
        frag = frag.substring(1);

    auto exeDir = juce::File::getSpecialLocation(juce::File::currentExecutableFile).getParentDirectory();
    bool hasLocalManual =
        exeDir.getChildFile("manual.html").existsAsFile() ||
        exeDir.getChildFile("docs/manual.html").existsAsFile() ||
        exeDir.getChildFile("Web/static/manual.html").existsAsFile() ||
        exeDir.getChildFile("Source/Web/static/manual.html").existsAsFile() ||
        juce::File("C:/davecore/docs/manual.html").existsAsFile();

    juce::String urlStr = hasLocalManual
        ? "http://127.0.0.1:8080/manual.html"
        : "https://openrig-host.github.io/fanfare/manual.html";

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
