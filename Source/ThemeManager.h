#pragma once

#include <JuceHeader.h>
#include "Theme.h"

// ThemeManager.h - singleton owning the active Front Panel theme.

class ThemeManager : public juce::ChangeBroadcaster {
public:
    static ThemeManager& getInstance() {
        static ThemeManager instance;
        return instance;
    }

    const Theme& active() const { return Theme::all()[juce::jlimit(0, (int)Theme::all().size() - 1, currentIndex)]; }

    static juce::Colour get(Theme::Role r) { return getInstance().active().get(r); }

    int getCurrentIndex() const { return currentIndex; }
    juce::String getCurrentId() const { return active().id; }
    juce::StringArray getThemeNames() const {
        juce::StringArray names;
        for (const auto& t : Theme::all())
            names.add(t.name);
        return names;
    }
    bool isFlat() const { return active().flat; }

    void setCurrentByIndex(int idx) {
        idx = juce::jlimit(0, (int)Theme::all().size() - 1, idx);
        if (idx == currentIndex)
            return;
        currentIndex = idx;
        saveToSettings();
        sendChangeMessage();
    }

    void setCurrentById(const juce::String& rawId) {
        juce::String id = Theme::normalizeThemeId(rawId);
        if (Theme::findById(id) != nullptr) {
            for (size_t i = 0; i < Theme::all().size(); ++i) {
                if (Theme::all()[i].id == id) {
                    setCurrentByIndex((int)i);
                    return;
                }
            }
        }
    }

    void loadFromSettings() {
        auto f = settingsFile();
        if (!f.existsAsFile()) {
            auto leg = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
                           .getChildFile("OpenRig/settings/theme.json");
            if (leg.existsAsFile())
                f = leg;
        }
        if (f.existsAsFile()) {
            auto parsed = juce::JSON::parse(f);
            if (auto* obj = parsed.getDynamicObject()) {
                juce::String rawId = obj->getProperty("themeId").toString();
                juce::String normId = Theme::normalizeThemeId(rawId);
                for (size_t i = 0; i < Theme::all().size(); ++i) {
                    if (Theme::all()[i].id == normId) {
                        currentIndex = (int)i;
                        if (normId != rawId)
                            saveToSettings(); // persist one-time migration
                        return;
                    }
                }
            }
        }
        currentIndex = 0; // Front Panel Dark
    }

    void saveToSettings() {
        auto f = settingsFile();
        f.getParentDirectory().createDirectory();
        juce::DynamicObject::Ptr obj = new juce::DynamicObject();
        obj->setProperty("themeId", getCurrentId());
        juce::FileOutputStream fos(f);
        if (fos.openedOk()) {
            fos.setPosition(0);
            fos.truncate();
            juce::JSON::writeToStream(fos, juce::var(obj.get()));
        }
    }

private:
    ThemeManager() = default;

    static juce::File settingsFile() {
        return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
            .getChildFile("Fanfare/settings/theme.json");
    }

    int currentIndex = 0;

public:
    ThemeManager(const ThemeManager&) = delete;
    ThemeManager& operator=(const ThemeManager&) = delete;
};
