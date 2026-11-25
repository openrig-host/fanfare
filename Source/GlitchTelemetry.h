#pragma once

#include <JuceHeader.h>
#include "FanfareConstants.h"
#include <atomic>

namespace Fanfare {

/**
 * Real-time-safe audio callback timing, underrun counters, jitter statistics,
 * and batched message-thread glitch log writer extracted from MainComponent.
 *
 * Audio-thread methods use only lock-free atomics and perform zero heap
 * allocations or locks. Log formatting and file flushing run on the message
 * thread.
 */
class GlitchTelemetry {
public:
    // --- Real-time safe methods (callable from audio or message thread) ---

    void suppressForMs(int durationMs) noexcept {
        const juce::int64 now = juce::Time::getHighResolutionTicks();
        const juce::int64 delta =
            juce::Time::secondsToHighResolutionTicks(durationMs * 0.001);
        const juce::int64 target = now + delta;
        juce::int64 prev = suppressGlitchUntilTicks.load(std::memory_order_relaxed);
        while (prev < target &&
               !suppressGlitchUntilTicks.compare_exchange_weak(
                   prev, target, std::memory_order_release,
                   std::memory_order_relaxed)) {
        }
    }

    void resetLastCallbackTicks() noexcept {
        lastAudioCallbackTicks.store(0);
    }

    juce::int64 getLastCallbackTicks() const noexcept {
        return lastAudioCallbackTicks.load(std::memory_order_relaxed);
    }

    void recordCallbackArrival(juce::int64 nowTicks,
                               int numSamples,
                               double sampleRate,
                               bool isRigTransitioning,
                               juce::int64 lastRigApplyEndTicks) noexcept {
        const juce::int64 prevTicks = lastAudioCallbackTicks.exchange(nowTicks);
        if (prevTicks <= 0)
            return;

        const double elapsedMs =
            juce::Time::highResolutionTicksToSeconds(nowTicks - prevTicks) * 1000.0;
        const double sr = (sampleRate > 0.0) ? sampleRate : 44100.0;
        const double expectedMs = static_cast<double>(numSamples) / sr * 1000.0;

        if (expectedMs <= 0.0 ||
            elapsedMs <= expectedMs * FanfareConstants::kLateCallbackRatio) {
            return;
        }

        const juce::int64 suppressUntil =
            suppressGlitchUntilTicks.load(std::memory_order_relaxed);
        const bool postApplySettle =
            (lastRigApplyEndTicks > 0) &&
            (juce::Time::highResolutionTicksToSeconds(nowTicks - lastRigApplyEndTicks) < 0.5);
        const double prevDurationMs =
            lastCallbackDurationMs.load(std::memory_order_relaxed);
        const bool isSystemSleepOrSuspendGap =
            (elapsedMs > 5000.0 && prevDurationMs < 500.0);

        if (isSystemSleepOrSuspendGap) {
            // Multi-second/multi-hour timestamp jump across OS sleep/hibernate or
            // USB audio standby: re-arm post-wake settle window without logging a
            // fake underrun.
            suppressForMs(2000);
        } else if (nowTicks < suppressUntil || isRigTransitioning || postApplySettle) {
            // Suppressed during brief device-open settle or active rig state swap.
        } else {
            lateCallbackCount.fetch_add(1, std::memory_order_relaxed);
            if (elapsedMs > maxCallbackIntervalMs.load(std::memory_order_relaxed))
                maxCallbackIntervalMs.store(elapsedMs, std::memory_order_relaxed);

            if (elapsedMs > expectedMs * FanfareConstants::kUnderrunCallbackRatio) {
                audioUnderrunCount++;
                lastGlitchStallMs.store(elapsedMs);
                pendingGlitchActualMs.store(elapsedMs);
                pendingGlitchExpectedMs.store(expectedMs);
                pendingGlitchDurationMs.store(prevDurationMs);
                hasPendingGlitchLog.store(true);
            }
        }
    }

    void recordCallbackDurationMs(double callbackMs, double expectedMs) noexcept {
        lastCallbackDurationMs.store(callbackMs, std::memory_order_relaxed);
        if (expectedMs > 0.0 && callbackMs > expectedMs * FanfareConstants::kHeavyCallbackLoad)
            heavyCallbackCount.fetch_add(1, std::memory_order_relaxed);
        if (callbackMs > maxCallbackDurationMs.load(std::memory_order_relaxed))
            maxCallbackDurationMs.store(callbackMs, std::memory_order_relaxed);
    }

    void incrementEngineStall() noexcept {
        engineStallCount++;
    }

    void resetUnderrunCounts() noexcept {
        audioUnderrunCount.store(0);
        engineStallCount.store(0);
    }

    int getAudioUnderruns() const noexcept {
        return audioUnderrunCount.load();
    }

    int getEngineStalls() const noexcept {
        return engineStallCount.load();
    }

    int getTotalUnderruns() const noexcept {
        return audioUnderrunCount.load() + engineStallCount.load();
    }

    double getLastGlitchStallMs() const noexcept {
        return lastGlitchStallMs.load();
    }

    bool consumePendingGlitch(double& actualMs,
                              double& expectedMs,
                              double& durationMs) noexcept {
        if (!hasPendingGlitchLog.exchange(false))
            return false;
        actualMs = pendingGlitchActualMs.exchange(0.0);
        expectedMs = pendingGlitchExpectedMs.exchange(0.0);
        durationMs = pendingGlitchDurationMs.exchange(0.0);
        return actualMs > 0.0;
    }

    bool drainJitterStats(int& lateCount,
                          int& heavyCount,
                          double& maxIntervalMs,
                          double& maxDurationMs) noexcept {
        lateCount = lateCallbackCount.exchange(0, std::memory_order_relaxed);
        heavyCount = heavyCallbackCount.exchange(0, std::memory_order_relaxed);
        maxIntervalMs = maxCallbackIntervalMs.exchange(0.0, std::memory_order_relaxed);
        maxDurationMs = maxCallbackDurationMs.exchange(0.0, std::memory_order_relaxed);
        return lateCount > 0 || heavyCount > 0;
    }

    // --- Message-thread log batching and file persistence ---

    static juce::File getDefaultLogFile() {
        auto appData =
            juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
                .getChildFile("Fanfare");
        return appData.getChildFile("audio_underrun_log.txt");
    }

    void logAudioGlitch(double actualMs,
                        double expectedMs,
                        double cbDurationMs,
                        const juce::String& cpuText,
                        const juce::String& ramText,
                        const juce::String& thermalTag) {
        juce::String entry =
            "[" + formatTimestamp() +
            "] [AUDIO UNDERRUN GLITCH] Buffer stall: " + juce::String(actualMs, 2) +
            "ms (expected " + juce::String(expectedMs, 2) +
            "ms) | callback took: " + juce::String(cbDurationMs, 2) +
            "ms | CPU: " + cpuText + " | RAM: " + ramText +
            " | " + thermalTag + "\n";

        const juce::ScopedLock sl(glitchLogLock);
        pendingGlitchLogText += entry;
    }

    void logAudioNanSpike(int channel, int sampleIndex, float badVal) {
        juce::String entry =
            "[" + formatTimestamp() +
            "] [SAMPLE CORRUPTION / NAN SPIKE] Ch " + juce::String(channel) +
            " Sample " + juce::String(sampleIndex) +
            " Value: " + juce::String(badVal) + "\n";

        const juce::ScopedLock sl(glitchLogLock);
        pendingGlitchLogText += entry;
    }

    void flushGlitchLog(const juce::File& logFile) {
        juce::String batch;
        {
            const juce::ScopedLock sl(glitchLogLock);
            if (pendingGlitchLogText.isEmpty())
                return;
            batch.swapWith(pendingGlitchLogText);
        }
        auto parentDir = logFile.getParentDirectory();
        if (!parentDir.exists())
            parentDir.createDirectory();
        logFile.appendText(batch);
    }

    void flushGlitchLog() {
        flushGlitchLog(getDefaultLogFile());
    }

private:
    static juce::String formatTimestamp() {
        const auto now = juce::Time::getCurrentTime();
        return now.formatted("%H:%M:%S.") +
               juce::String(now.getMilliseconds()).paddedLeft('0', 3);
    }

    std::atomic<juce::int64> suppressGlitchUntilTicks{0};
    std::atomic<int> audioUnderrunCount{0};
    std::atomic<int> engineStallCount{0};
    std::atomic<juce::int64> lastAudioCallbackTicks{0};
    std::atomic<double> lastGlitchStallMs{0.0};
    std::atomic<double> pendingGlitchActualMs{0.0};
    std::atomic<double> pendingGlitchExpectedMs{0.0};
    std::atomic<double> lastCallbackDurationMs{0.0};
    std::atomic<double> pendingGlitchDurationMs{0.0};
    std::atomic<bool> hasPendingGlitchLog{false};

    std::atomic<int> lateCallbackCount{0};
    std::atomic<int> heavyCallbackCount{0};
    std::atomic<double> maxCallbackIntervalMs{0.0};
    std::atomic<double> maxCallbackDurationMs{0.0};

    juce::String pendingGlitchLogText;
    juce::CriticalSection glitchLogLock;
};

} // namespace Fanfare
