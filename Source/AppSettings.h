#pragma once

#include <JuceHeader.h>
#include "FanfareConstants.h"
#include <functional>

namespace Fanfare {

/**
    Typed JSON-backed settings service for Fanfare (`ui_settings.json` by default).
    Thread-safe, refuses to overwrite corrupt non-empty JSON files, and writes
    atomically with a `.bak` backup of the previous file.
*/
class AppSettings {
public:
    static AppSettings& getInstance() {
        static AppSettings instance(
            FanfareConstants::getAppDirectory().getChildFile("ui_settings.json"));
        return instance;
    }

    explicit AppSettings(const juce::File& file) : settingsFile(file) {}

    bool getBool(const juce::Identifier& key, bool defaultVal = false) const {
        juce::ScopedLock sl(lock);
        juce::var root;
        if (!loadLocked(root))
            return defaultVal;
        if (root.hasProperty(key))
            return (bool)root[key];
        return defaultVal;
    }

    int getInt(const juce::Identifier& key, int defaultVal = 0) const {
        juce::ScopedLock sl(lock);
        juce::var root;
        if (!loadLocked(root))
            return defaultVal;
        if (root.hasProperty(key))
            return (int)root[key];
        return defaultVal;
    }

    double getDouble(const juce::Identifier& key, double defaultVal = 0.0) const {
        juce::ScopedLock sl(lock);
        juce::var root;
        if (!loadLocked(root))
            return defaultVal;
        if (root.hasProperty(key))
            return (double)root[key];
        return defaultVal;
    }

    juce::String getString(const juce::Identifier& key, const juce::String& defaultVal = {}) const {
        juce::ScopedLock sl(lock);
        juce::var root;
        if (!loadLocked(root))
            return defaultVal;
        if (root.hasProperty(key))
            return root[key].toString();
        return defaultVal;
    }

    bool setBool(const juce::Identifier& key, bool value) {
        return update([&](juce::DynamicObject& obj) { obj.setProperty(key, value); });
    }

    bool setInt(const juce::Identifier& key, int value) {
        return update([&](juce::DynamicObject& obj) { obj.setProperty(key, value); });
    }

    bool setDouble(const juce::Identifier& key, double value) {
        return update([&](juce::DynamicObject& obj) { obj.setProperty(key, value); });
    }

    bool setString(const juce::Identifier& key, const juce::String& value) {
        return update([&](juce::DynamicObject& obj) { obj.setProperty(key, value); });
    }

    juce::var getSnapshot() const {
        juce::ScopedLock sl(lock);
        juce::var root;
        if (!loadLocked(root))
            return juce::var(new juce::DynamicObject());
        return root;
    }

    bool update(const std::function<void(juce::DynamicObject&)>& mutator) {
        juce::ScopedLock sl(lock);
        juce::var root;
        if (!loadLocked(root))
            return false;
        if (auto* obj = root.getDynamicObject()) {
            mutator(*obj);
            return saveLocked(root);
        }
        return false;
    }

    // Typed helpers for Stage Web Remote
    juce::String getWebRemoteBindScope() const {
        const auto s = getString("webRemoteBindScope", "loopback").trim().toLowerCase();
        if (s == "off" || s == "loopback" || s == "lan")
            return s;
        return "loopback";
    }

    bool setWebRemoteBindScope(const juce::String& scope) {
        const auto norm = scope.trim().toLowerCase();
        const juce::String val = (norm == "off" || norm == "loopback" || norm == "lan")
                                     ? norm
                                     : juce::String("loopback");
        return setString("webRemoteBindScope", val);
    }

    int getWebRemotePort() const {
        return juce::jlimit(1024, 65535, getInt("webRemotePort", 8080));
    }

    bool setWebRemotePort(int port) {
        return setInt("webRemotePort", juce::jlimit(1024, 65535, port));
    }

private:
    bool loadLocked(juce::var& outRoot) const {
        if (!settingsFile.existsAsFile()) {
            outRoot = juce::var(new juce::DynamicObject());
            return true;
        }
        const auto raw = settingsFile.loadFileAsString().trim();
        if (raw.isEmpty()) {
            outRoot = juce::var(new juce::DynamicObject());
            return true;
        }
        auto parsed = juce::JSON::parse(raw);
        if (!parsed.isObject())
            return false; // Corrupt JSON — refuse to clobber unrelated user data
        outRoot = parsed;
        return true;
    }

    bool saveLocked(const juce::var& root) const {
        auto parent = settingsFile.getParentDirectory();
        if (!parent.exists())
            parent.createDirectory();

        if (settingsFile.existsAsFile()) {
            auto bak = settingsFile.getSiblingFile(settingsFile.getFileName() + ".bak");
            settingsFile.copyFileTo(bak);
        }

        auto temp = settingsFile.getSiblingFile(
            settingsFile.getFileName() + "." + juce::Uuid().toString().substring(0, 8) + ".tmp");
        if (temp.existsAsFile())
            temp.deleteFile();

        if (!temp.replaceWithText(juce::JSON::toString(root))) {
            temp.deleteFile();
            return false;
        }
        if (!temp.moveFileTo(settingsFile)) {
            temp.deleteFile();
            return false;
        }
        return true;
    }

    juce::File settingsFile;
    mutable juce::CriticalSection lock;
};

} // namespace Fanfare
