#pragma once

#include <JuceHeader.h>
#include <mutex>
#include <map>
#include <set>
#include <condition_variable>
#include <chrono>
#include <functional>

namespace Fanfare {

// Per-path build gate: at most one build per normalized plugin path may be in
// flight — INCLUDING an abandoned one still running on its detached thread.
// Single-instance plugins (Kontakt, Zenology, Super 8) block or crash when a
// second instance touches them while the first is mid-setStateInformation, so
// a timed-out plugin load must fully finish (and destroy its instance) before
// another build of the same path starts.
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

// Waits up to timeoutMs (polling shouldAbort every 100ms) for any in-flight
// build of `path` to finish. Returns true ONLY if the gate was acquired (and
// increments the live count so the caller MUST pair with releasePathBuildGate).
// Returns false on abort or if an earlier build is still wedged past timeoutMs.
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

// Centralized thread-affinity classification for VST3 instantiation + state restore.
// Plugins using Native Instruments' Qt/QML/HostIntegration runtime (Kontakt, Super 8,
// Replika XT, Raum, Massive, Solid series, etc.), Spectrasonics, Toontrack, and
// Steinberg HALion require STA message-thread instantiation/restore and deadlock if
// invoked on a COINIT_MULTITHREADED background worker.
// Conversely, UVI Workstation requires an unblocked pumping message thread, so it
// must run on a background worker while the message thread is idle.
inline bool pluginRequiresMessageThread(const juce::String &path) {
    if (path.isEmpty())
        return false;
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
    for (const char *token : kMessageThreadTokens) {
        if (path.containsIgnoreCase(token))
            return true;
    }
    return false;
}

} // namespace Fanfare

