#include "PluginMirrorStreamer.h"
#include <chrono>

namespace OpenRig {

namespace {
    constexpr int kMinTargetFps = 15;
    constexpr int kMaxTargetFps = 25;
}

PluginMirrorStreamer::PluginMirrorStreamer(PluginMirrorCapture& captureToUse)
    : juce::Thread("PluginMirrorEncoder"),
      capture(captureToUse) {
}

PluginMirrorStreamer::~PluginMirrorStreamer() {
    setFrameSink(nullptr);
    setAudienceProbe(nullptr);
    stop();
    stopThread(-1);
}

// =====================================================================
// Lifecycle (message thread)
// =====================================================================

void PluginMirrorStreamer::start(int fps, float quality) {
    setTargetFps(fps);
    setJpegQuality(quality);

    if (streaming.load())
        return;

    streaming.store(true);
    startThread(juce::Thread::Priority::normal);
    startTimerHz(targetFps.load());
}

void PluginMirrorStreamer::stop() {
    streaming.store(false);
    stopTimer();
    signalThreadShouldExit();
    frameReady.notify_all();
    stopThread(2000);
}

void PluginMirrorStreamer::setFrameSink(FrameSink sink) {
    std::lock_guard<std::mutex> lock(sinkMutex);
    frameSink = std::move(sink);
}

void PluginMirrorStreamer::setAudienceProbe(AudienceProbe probe) {
    std::lock_guard<std::mutex> lock(sinkMutex);
    audienceProbe = std::move(probe);
}

void PluginMirrorStreamer::setTargetFps(int fps) {
    const int clamped = juce::jlimit(kMinTargetFps, kMaxTargetFps, fps);
    const bool changed = targetFps.exchange(clamped) != clamped;
    if (changed && streaming.load())
        startTimerHz(clamped); // message thread only
}

void PluginMirrorStreamer::setJpegQuality(float quality) {
    jpegQuality.store(juce::jlimit(0.3f, 0.9f, quality));
}

// =====================================================================
// Capture pump (message thread)
// =====================================================================

void PluginMirrorStreamer::timerCallback() {
    AudienceProbe probe;
    {
        std::lock_guard<std::mutex> lock(sinkMutex);
        probe = audienceProbe;
    }
    if (probe && !probe())
        return; // idle: no subscribers, skip snapshot entirely

    juce::Image image = capture.captureFrame();
    if (image.isNull())
        return;

    {
        std::lock_guard<std::mutex> lock(frameMutex);
        pendingImage = std::move(image);
        ++pendingSeq;
    }
    frameReady.notify_one();
    ++stats.framesCaptured;
}

// =====================================================================
// Encode + deliver loop (encoder thread)
// =====================================================================

void PluginMirrorStreamer::run() {
    uint64_t encodedSeq = 0;
    int fpsWindowCount = 0;
    double fpsWindowStart = juce::Time::getMillisecondCounterHiRes();

    while (!threadShouldExit()) {
        juce::Image image;
        {
            std::unique_lock<std::mutex> lock(frameMutex);
            frameReady.wait_for(lock, std::chrono::milliseconds(100), [&] {
                return threadShouldExit() || pendingSeq != encodedSeq;
            });

            if (threadShouldExit())
                break;
            if (pendingSeq == encodedSeq)
                continue;

            // Latest-frame-wins: account for any frames that arrived while the
            // previous encode was in flight - they are dropped by design.
            if (pendingSeq > encodedSeq + 1)
                stats.framesDropped += (int)(pendingSeq - encodedSeq - 1);

            image = pendingImage;
            encodedSeq = pendingSeq;
        }

        FrameSink sink;
        AudienceProbe probe;
        {
            std::lock_guard<std::mutex> lock(sinkMutex);
            probe = audienceProbe;
            sink = frameSink;
        }
        if (probe && !probe())
            continue;

        const double t0 = juce::Time::getMillisecondCounterHiRes();
        std::vector<uint8_t> jpeg = PluginMirrorCapture::encodeJpeg(image, jpegQuality.load());
        const double encodeMs = juce::Time::getMillisecondCounterHiRes() - t0;

        if (jpeg.empty()) {
            ++stats.framesDropped;
            continue;
        }

        stats.lastEncodeMs.store(encodeMs);
        stats.lastJpegBytes.store((int64_t)jpeg.size());
        ++stats.framesEncoded;

        // Called without sinkMutex held: the sink performs blocking socket I/O.
        if (sink)
            sink(jpeg);
        ++stats.framesSent;

        // Rolling 1 s FPS window
        if (++fpsWindowCount >= 4) {
            const double now = juce::Time::getMillisecondCounterHiRes();
            const double elapsed = now - fpsWindowStart;
            if (elapsed >= 1000.0) {
                stats.measuredFps.store(fpsWindowCount * 1000.0 / elapsed);
                fpsWindowCount = 0;
                fpsWindowStart = now;
            }
        }
    }
}

} // namespace OpenRig
