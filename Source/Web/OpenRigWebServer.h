#pragma once

#include <JuceHeader.h>
#include <vector>
#include <string>
#include <memory>
#include <functional>
#include <atomic>
#include <thread>
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
    
    // IP Addresses helper for UI display (e.g., http://192.168.1.50:8080)
    static juce::StringArray getLocalIpAddresses();
    static juce::String getPrimaryUrl(int port);

    // Pairing token that tablet clients must present (?k=... or X-OpenRig-Token).
    // Generated on first run and persisted, so it never changes between gigs —
    // the QR/bookmark URL stays valid forever. Embed it in the QR pairing URL.
    static juce::String getPairingToken() { ensureTokenLoaded(); return pairingToken; }

    // Callbacks to push state updates immediately
    void notifySceneChanged(int sceneIndex, const juce::String& sceneName);
    void notifySetlistChanged();
    void notifyNotesChanged();
    void notifyMixerChanged();
    void notifyMp3Changed();

    // Broadcast raw WebSocket text message to all connected clients
    void broadcastWebSocket(const juce::String& jsonMessage);

    // Number of browsers holding a live WebSocket session (0 = nobody attached)
    int getConnectedClientCount();

private:
    void run() override; // Worker thread listening for incoming TCP connections
    void timerCallback() override; // ~30Hz Telemetry & VU broadcast timer

    // HTTP / WebSocket connection handling
    void handleClientConnection(void* socketHandle);
    bool handleHttpRequest(void* socketHandle, const juce::String& requestMethod, const juce::String& requestPath, const juce::String& requestHeaders, const juce::String& requestBody);
    bool handleWebSocketHandshake(void* socketHandle, const juce::String& secWebSocketKey);
    void handleWebSocketSession(void* socketHandle);

    // Security: load/create the pairing token + remote-access settings
    static void ensureTokenLoaded();   // loads from disk or generates+persists once
    void loadPairingSettings();        // token + remote_security.json (per-instance settings)
    // True when the request presents the pairing token (query "k=" or the
    // X-OpenRig-Token header). requestText is the raw request incl. headers.
    bool isAuthorized(const juce::String& rawPathWithQuery, const juce::String& requestText) const;

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
    void processIncomingWsMessage(const juce::String& messageText);

    // Static Asset Serving
    juce::String getMimeType(const juce::String& filePath);
    bool serveStaticFile(void* socketHandle, const juce::String& relativePath);
    void sendHttpResponse(void* socketHandle, int statusCode, const juce::String& contentType, const juce::String& body);
    void sendHttpDataResponse(void* socketHandle, int statusCode, const juce::String& contentType, const void* data, size_t dataSize);

    // Embedded Fallback Assets (used if files are not on disk)
    static juce::String getEmbeddedHtml();
    static juce::String getEmbeddedCss();
    static juce::String getEmbeddedJs();

    FanfareEngine& engine;
    MainComponent* mainComponent;

    std::atomic<bool> serverRunning{false};
    int serverPort = 8080;
    void* listenSocket = nullptr; // Raw socket descriptor (SOCKET / int)

    // Pairing / security state
    static juce::String pairingToken;      // shared with the QR overlay UI
    bool allowRemoteDownloads = false;     // guard for POST /api/mp3/download
    std::atomic<int> activeConnectionCount{0};
    static constexpr int kMaxConnections = 8;

    // Active WebSocket clients set
    std::mutex clientSocketsMutex;
    std::unordered_set<void*> activeWsClients;

    // Telemetry throttling
    int telemetryThrottleCount = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(WebServer)
};

} // namespace OpenRig
