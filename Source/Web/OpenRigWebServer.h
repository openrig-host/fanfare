#pragma once

#include <JuceHeader.h>
#include "PluginMirrorCapture.h"
#include "PluginMirrorInput.h"
#include "PluginMirrorStreamer.h"
#include <vector>
#include <string>
#include <memory>
#include <functional>
#include <atomic>
#include <thread>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <unordered_set>

class FanfareEngine;
class MainComponent;

namespace OpenRig {

/**
 * Struct representing real-time telemetry sent to tablet clients via WebSocket.
 */
struct LiveTelemetry {
    float fohPeakL = 0.0f;
    float fohPeakR = 0.0f;
    float iemPeakL = 0.0f;
    float iemPeakR = 0.0f;
    float cpuLoad = 0.0f;
    int ramUsageMb = 0;
    int underruns = 0;

    struct SlotMeter {
        int index = 0;
        float peakL = 0.0f;
        float peakR = 0.0f;
        bool midiActive = false;
    };
    std::vector<SlotMeter> slotMeters;
};

/**
 * Lightweight, high-performance C++ HTTP and WebSocket server for OpenRig.
 * Enables Android tablets, iPads, and smartphones to connect directly over local Wi-Fi or USB tether.
 */
class WebServer : public juce::Thread, private juce::Timer {
public:
    WebServer(FanfareEngine& engine, MainComponent* mainComponent = nullptr);
    ~WebServer() override;

    // Server lifecycle
    bool startServer(int port = 8080);
    void stopServer();
    bool isRunning() const { return serverRunning.load(); }
    int getPort() const { return serverPort; }
    const juce::String& getSessionToken() const noexcept { return sessionToken; }

    // IP Addresses helper for UI display (e.g., http://192.168.1.50:8080)
    static juce::StringArray getLocalIpAddresses();
    static juce::String getPrimaryUrl(int port);

    // Callbacks to push state updates immediately
    void notifySceneChanged(int sceneIndex, const juce::String& sceneName);
    void notifySetlistChanged();
    void notifyNotesChanged();
    void notifyMixerChanged();
    void notifyMp3Changed();

    // Broadcast raw WebSocket text message to all connected clients
    void broadcastWebSocket(const juce::String& jsonMessage);

    // ===== Live Plugin Mirror (prototype) ================================
    // Streams JPEG frames of a plugin editor to subscribed tablet clients over
    // binary WebSocket frames and injects reverse touch events back into the
    // captured window. See PluginMirrorCapture/Streamer/Input headers.
    // ---------------------------------------------------------------------

    /** Registers the plugin editor window to mirror (called on the message
        thread when a plugin editor opens). Prefers native window capture when
        a handle is given (required for editors hosting their own child
        windows), falling back to the JUCE component snapshot path. */
    void registerPluginMirrorSource(juce::Component* editorContent,
                                    void* nativeWindowHandle,
                                    const juce::String& name);

    /** Starts the capture/stream loop (message thread). Usually auto-called
        when the first client sends mirror_subscribe. */
    bool startPluginMirror(int targetFps = 20, float jpegQuality = 0.72f);
    void stopPluginMirror();
    bool isPluginMirrorActive() const;

    /** Status/stats payload shared by get_mirror_status and the HUD. */
    juce::String getPluginMirrorStatusJson();

private:
    struct LifetimeState {
        std::atomic<bool> alive{true};
    };

    struct ClientWorker {
        std::thread thread;
        std::atomic<bool> finished{false};
    };

    void run() override; // Worker thread listening for incoming TCP connections
    void timerCallback() override; // ~30Hz Telemetry & VU broadcast timer

    // HTTP / WebSocket connection handling
    void handleClientConnection(void* socketHandle);
    bool handleHttpRequest(void* socketHandle,
                           const juce::String& requestMethod,
                           const juce::String& requestPath,
                           const juce::String& requestQuery,
                           const juce::String& requestHeaders,
                           const juce::String& requestBody);
    bool handleWebSocketHandshake(void* socketHandle, const juce::String& secWebSocketKey);
    void handleWebSocketSession(void* socketHandle);

    // Auth & Origin validation
    bool isAuthorizedRequest(const juce::String& headers, const juce::String& query) const;
    bool isOriginAllowed(const juce::String& headers, bool requireOrigin) const;

    // REST API Handlers
    juce::var buildStatusJson();
    juce::var buildScenesJson();
    juce::var buildSetlistJson();
    juce::var buildSongsLibraryJson();
    juce::var buildNotepadJson();
    juce::var buildMixerJson();
    juce::var buildMp3Json();

    juce::String handleApiPost(const juce::String& path, const juce::var& jsonPayload);

    // WebSocket Message Dispatch
    void processIncomingWsMessage(void* socketHandle, const juce::String& messageText);

    // Live Plugin Mirror plumbing
    void sendWebSocketTextToClient(void* socketHandle, const juce::String& jsonMessage);
    void broadcastBinaryToMirrorSubscribers(const void* data, size_t size);
    bool hasMirrorSubscribers();
    void handleMirrorSubscribe(void* socketHandle);
    void handleMirrorUnsubscribe(void* socketHandle);
    juce::var buildMirrorInfoJson();

    // Static Asset Serving
    juce::String getMimeType(const juce::String& filePath);
    bool serveStaticFile(void* socketHandle, const juce::String& relativePath, const juce::String& extraHeader = {});
    void sendHttpResponse(void* socketHandle, int statusCode, const juce::String& contentType, const juce::String& body, const juce::String& extraHeader = {});
    void sendHttpDataResponse(void* socketHandle, int statusCode, const juce::String& contentType, const void* data, size_t dataSize, const juce::String& extraHeader = {});

    // Embedded Fallback Assets (used if files are not on disk)
    static juce::String getEmbeddedHtml();
    static juce::String getEmbeddedCss();
    static juce::String getEmbeddedJs();

    FanfareEngine& engine;
    MainComponent* mainComponent;

    juce::String sessionToken;
    std::shared_ptr<LifetimeState> lifetime = std::make_shared<LifetimeState>();

    std::atomic<bool> serverRunning{false};
    int serverPort = 8080;
    void* listenSocket = nullptr; // Raw socket descriptor (SOCKET / int)

    // Active HTTP and WebSocket clients (closed ONLY by their owning worker thread;
    // stopServer() calls shutdown() on them to unblock recv/send without handle reuse races)
    std::mutex clientSocketsMutex;
    std::unordered_set<void*> activeHttpSockets;
    std::unordered_set<void*> activeWsClients;
    std::atomic<int> activeWsClientCount{0};

    // Non-blocking outbound WebSocket delivery (message thread / encoder never stall on TCP sendAll)
    struct OutboundWsItem {
        std::vector<uint8_t> frame;
        void* singleClient = nullptr;
        bool mirrorOnly = false;
    };
    static std::vector<uint8_t> buildWsFrame(uint8_t finOpcode, const void* data, size_t size);
    void enqueueWsFrame(std::vector<uint8_t> frame, void* singleClient, bool isMeter, bool mirrorOnly);
    void wsOutboundLoop();

    std::mutex wsOutMutex;
    std::condition_variable wsOutCv;
    std::thread wsOutboundThread;
    bool hasPendingMeter = false;
    std::vector<uint8_t> pendingMeterFrame;
    std::deque<OutboundWsItem> wsOutQueue;

    // Joinable per-connection threads
    std::mutex workersMutex;
    std::vector<std::shared_ptr<ClientWorker>> clientWorkers;

    // Telemetry throttling
    int telemetryThrottleCount = 0;

    // Live Plugin Mirror state (subscribers guarded by clientSocketsMutex so a
    // socket can never be closed mid-broadcast; see activeWsClients)
    std::unordered_set<void*> mirrorSubscribers;
    std::atomic<int> mirrorSubscriberCount{0};
    std::unique_ptr<PluginMirrorCapture> mirrorCapture;
    std::unique_ptr<PluginMirrorStreamer> mirrorStreamer;
    PluginMirrorInputHandler mirrorInput;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(WebServer)
};

} // namespace OpenRig
