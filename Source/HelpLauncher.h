#pragma once

#include <JuceHeader.h>
#include "ThemeManager.h"

namespace Fanfare {

inline void openHelpManual(const juce::String& anchor = {}) {
    juce::String frag = anchor.trim();
    if (frag.startsWithChar('#'))
        frag = frag.substring(1);

    // Check if manual.html exists locally on disk so the embedded HTTP server
    // (port 8080) can serve it, otherwise fall back to the online GitHub Pages manual.
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
    btn.setButtonText("HAAALP!");
    btn.setColour(juce::TextButton::buttonColourId, juce::Colour(0xFFB71C1C).darker(0.15f));
    btn.setColour(juce::TextButton::textColourOffId, juce::Colours::white);
    btn.setTooltip(tooltipText.isNotEmpty()
                       ? tooltipText
                       : "Open the Fanfare Stage Manual to this exact section");
    btn.onClick = [anchor]() {
        openHelpManual(anchor);
    };
}

} // namespace Fanfare

