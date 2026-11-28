#pragma once

#include <JuceHeader.h>
#include <mutex>
#include <map>
#include <set>
#include <condition_variable>
#include <chrono>
#include <functional>

namespace Fanfare {

inline std::mutex &buildGateMutex() {
    static std::mutex m;
    return m;
}
inline std::map<juce::String, int> &liveBuildsByPath() {
    static std::map<juce::String, int> m;
    return m;
}
inline std::condition_variable &buildGateCv() {
    static std::condition_variable cv;
    return cv;
}

inline bool tryAcquirePathBuildGate(const juce::String &path, int timeoutMs,
                                    const std::function<bool()> &shouldAbort = {}) {
    if (path.isEmpty())
        return true;

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    std::unique_lock<std::mutex> lk(buildGateMutex());
    for (;;) {
        auto it = liveBuildsByPath().find(path);
        if (it == liveBuildsByPath().end() || it->second <= 0) {
            liveBuildsByPath()[path] = 1;
            return true;
        }
        if (shouldAbort && shouldAbort())
            return false;
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline)
            return false;
        const auto slice = std::min<std::chrono::milliseconds>(
            std::chrono::milliseconds(100),
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now) + std::chrono::milliseconds(1));
        buildGateCv().wait_for(lk, slice);
    }
}

inline void releasePathBuildGate(const juce::String &path) {
    if (path.isEmpty())
        return;
    {
        std::lock_guard<std::mutex> lk(buildGateMutex());
        auto it = liveBuildsByPath().find(path);
        if (it != liveBuildsByPath().end() && --it->second <= 0)
            liveBuildsByPath().erase(it);
    }
    buildGateCv().notify_all();
}

inline std::set<juce::String> &poisonedBuildPaths() {
    static std::set<juce::String> s;
    return s;
}

inline bool isPathPoisoned(const juce::String &path) {
    if (path.isEmpty())
        return false;
    std::lock_guard<std::mutex> lk(buildGateMutex());
    return poisonedBuildPaths().count(path) > 0;
}

inline void poisonPath(const juce::String &path) {
    if (path.isEmpty())
        return;
    {
        std::lock_guard<std::mutex> lk(buildGateMutex());
        poisonedBuildPaths().insert(path);
    }
    buildGateCv().notify_all();
}

inline std::set<juce::String> getPoisonedPaths() {
    std::lock_guard<std::mutex> lk(buildGateMutex());
    return poisonedBuildPaths();
}

inline void clearPoisonedPaths() {
    {
        std::lock_guard<std::mutex> lk(buildGateMutex());
        poisonedBuildPaths().clear();
    }
    buildGateCv().notify_all();
}

inline bool pluginRequiresMessageThread(const juce::String &path) {
    if (path.isEmpty())
        return false;
    if (path.trim().startsWithIgnoreCase("AudioUnit:"))
        return true;
    static const char *const kMessageThreadTokens[] = {
        // Native Instruments instruments & QML/Qt FX shell
        "Kontakt", "Super 8", "Replika", "Raum", "Massive", "Komplete",
        "Guitar Rig", "FM8", "Absynth", "Reaktor", "Battery",
        "Solid Bus", "Solid Dynamics", "Solid EQ", "Supercharger",
        "Phasis", "Flair", "Choral", "Bite", "Dirt", "Freak",
        "Driver", "Transient Master", "VC 2A", "VC 76", "VC 160",
        "RC 24", "RC 48", "Enhanced EQ", "Passive EQ", "Vari Comp",
        // Spectrasonics
        "Omnisphere", "Keyscape", "Trilian", "Stylus RMX", "Spectrasonics",
        // Toontrack
        "EZkeys", "EZdrummer", "EZbass", "Superior Drummer",
        // Steinberg
        "HALion"
    };
    const int lastSlash = path.lastIndexOfChar('/');
    const int lastBackslash = path.lastIndexOfChar('\\');
    const int sep = juce::jmax(lastSlash, lastBackslash);
    const juce::String leafName = (sep >= 0) ? path.substring(sep + 1) : path;
    const juce::String &matchTarget = leafName.isNotEmpty() ? leafName : path;
    for (const char *token : kMessageThreadTokens) {
        if (matchTarget.containsIgnoreCase(token))
            return true;
    }
    return false;
}

} // namespace Fanfare
