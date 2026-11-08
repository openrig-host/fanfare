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
    static bool isStudio() { return getInstance().active().isStudio(); }

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
                const bool studioIntroduced = static_cast<bool>(obj->getProperty("studioIntroduced"));
                const juce::String rawId = obj->getProperty("themeId").toString().trim();
                const juce::String normId = Theme::normalizeThemeId(rawId);

                // Honour any valid saved themeId; only fall back to Studio (index 0)
                // when no valid themeId is present. Persist normalized/migrated state once.
                for (size_t i = 0; i < Theme::all().size(); ++i) {
                    if (Theme::all()[i].id == normId) {
                        currentIndex = (int)i;
                        if (!studioIntroduced || normId != rawId)
                            saveToSettings();
                        return;
                    }
                }

                if (!studioIntroduced) {
                    currentIndex = 0; // Studio
                    saveToSettings();
                    return;
                }
            }
        }
        currentIndex = 0; // Studio
    }

    void saveToSettings() {
        auto f = settingsFile();
        f.getParentDirectory().createDirectory();
        juce::DynamicObject::Ptr obj = new juce::DynamicObject();
        obj->setProperty("themeId", getCurrentId());
        obj->setProperty("studioIntroduced", true);
        juce::TemporaryFile tmp(f);
        if (tmp.getFile().replaceWithText(juce::JSON::toString(juce::var(obj.get()))))
            tmp.overwriteTargetFileWithTemporary();
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
