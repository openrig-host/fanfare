#pragma once

#include <JuceHeader.h>
#include "PluginMirrorCapture.h"
#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <vector>

namespace OpenRig {

/**
 * Streaming loop for the Live Plugin Mirror.
 *
 * Captures at a target rate of 15-25 FPS and hands JPEG frames to a sink
 * (the OpenRigWebServer binary WebSocket broadcaster) with a latency budget
 * of < 30 ms capture-to-socket:
 *
 *  - Capture runs on the message thread via juce::Timer (required by
 *    Component::createComponentSnapshot); it is a cheap bitmap snapshot.
 *  - JPEG compression runs on a dedicated encoder thread, so the message
 *    thread never blocks on the compressor.
 *  - Latest-frame-wins: the queue depth is exactly one image. If encoding
 *    falls behind, intermediate frames are dropped (and counted) instead of
 *    building a backlog - stale frames are pure latency.
 *  - Audience gating: when no client is subscribed, neither capture nor
 *    encoding runs, so the mirror costs nothing while idle.
 */
class PluginMirrorStreamer : private juce::Timer, private juce::Thread {
public:
    /** Receives each finished JPEG payload (called on the encoder thread). */
    using FrameSink = std::function<void(const std::vector<uint8_t>& jpeg)>;
    /** Cheap probe: false => skip capture/encode entirely. */
    using AudienceProbe = std::function<bool()>;

    struct Stats {
        std::atomic<int> framesCaptured{0};
        std::atomic<int> framesEncoded{0};
        std::atomic<int> framesSent{0};
        std::atomic<int> framesDropped{0};
        std::atomic<double> lastEncodeMs{0.0};
        std::atomic<double> measuredFps{0.0};
        std::atomic<int64_t> lastJpegBytes{0};
    };

    explicit PluginMirrorStreamer(PluginMirrorCapture& captureToUse);
    ~PluginMirrorStreamer() override;

    // Lifecycle (message thread)
    void start(int targetFps = 20, float jpegQuality = 0.72f);
    void stop();
    bool isStreaming() const noexcept { return streaming.load(); }

    void setFrameSink(FrameSink sink);
    void setAudienceProbe(AudienceProbe probe);
    void setTargetFps(int fps);   // clamped to 15..25
    void setJpegQuality(float q); // clamped to 0.3..0.9

    int getTargetFps() const noexcept { return targetFps.load(); }
    float getJpegQuality() const noexcept { return jpegQuality.load(); }
    Stats& getStats() noexcept { return stats; }

private:
    void timerCallback() override; // message thread: snapshot capture
    void run() override;           // encoder thread: JPEG compress + sink

    PluginMirrorCapture& capture;

    FrameSink frameSink;
    AudienceProbe audienceProbe;
    std::mutex sinkMutex;

    // Single-slot, latest-frame-wins queue between capture and encoder.
    std::mutex frameMutex;
    std::condition_variable frameReady;
    juce::Image pendingImage;
    uint64_t pendingSeq = 0;

    std::atomic<bool> streaming{false};
    std::atomic<int> targetFps{20};
    std::atomic<float> jpegQuality{0.72f};
    Stats stats;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PluginMirrorStreamer)
};

} // namespace OpenRig
