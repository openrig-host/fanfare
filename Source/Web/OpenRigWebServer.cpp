#ifdef _WIN32
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #pragma comment(lib, "ws2_32.lib")
#endif

#include "OpenRigWebServer.h"
#include "EmbeddedWebAssets.h"
#include "../FanfareConstants.h"
#include "../FanfareEngine.h"
#include "../HelpLauncher.h"
#include "../RigLibrary.h"
#include "../SetlistManager.h"
#include "../Version.h"
#include "../YoutubeDownloadManager.h"
#include <sstream>
#include <random>
#include <cmath>
#include <cctype>
#include <cstring>
#include <unordered_map>

#if JUCE_WINDOWS
  typedef SOCKET SocketType;
  #define INVALID_SOCKET_VAL INVALID_SOCKET
  #define SOCKET_CLOSE(s) closesocket(s)
  #define SOCKET_SHUTDOWN_BOTH(s) ::shutdown(s, SD_BOTH)
#else
  #include <sys/types.h>
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <netinet/tcp.h>
  #include <arpa/inet.h>
  #include <unistd.h>
  #include <fcntl.h>
  #include <errno.h>
  typedef int SocketType;
  #define INVALID_SOCKET_VAL -1
  #define SOCKET_CLOSE(s) close(s)
  #define SOCKET_SHUTDOWN_BOTH(s) ::shutdown(s, SHUT_RDWR)
#endif

namespace {
struct EmbeddedManualAsset {
    const char* path;
    const unsigned char* data;
    size_t len;
    const char* mime;
};

static const EmbeddedManualAsset kEmbeddedManualAssets[] = {
    { "/manual.html", OpenRig::EmbeddedWebAssets::MANUAL_HTML_DATA, OpenRig::EmbeddedWebAssets::MANUAL_HTML_DATA_LEN, "text/html; charset=utf-8" },
    { "/Fanfare_Icon.png", OpenRig::EmbeddedWebAssets::MANUAL_ICON_PNG_DATA, OpenRig::EmbeddedWebAssets::MANUAL_ICON_PNG_DATA_LEN, "image/png" },
    { "/assets-readonly/css/main.css", OpenRig::EmbeddedWebAssets::MANUAL_MAIN_CSS_DATA, OpenRig::EmbeddedWebAssets::MANUAL_MAIN_CSS_DATA_LEN, "text/css; charset=utf-8" },
    { "/assets-readonly/css/fontawesome-all.min.css", OpenRig::EmbeddedWebAssets::MANUAL_FA_CSS_DATA, OpenRig::EmbeddedWebAssets::MANUAL_FA_CSS_DATA_LEN, "text/css; charset=utf-8" },
    { "/assets-readonly/js/jquery.min.js", OpenRig::EmbeddedWebAssets::MANUAL_JQUERY_JS_DATA, OpenRig::EmbeddedWebAssets::MANUAL_JQUERY_JS_DATA_LEN, "application/javascript; charset=utf-8" },
    { "/assets-readonly/js/jquery.scrollex.min.js", OpenRig::EmbeddedWebAssets::MANUAL_SCROLLEX_JS_DATA, OpenRig::EmbeddedWebAssets::MANUAL_SCROLLEX_JS_DATA_LEN, "application/javascript; charset=utf-8" },
    { "/assets-readonly/js/jquery.scrolly.min.js", OpenRig::EmbeddedWebAssets::MANUAL_SCROLLY_JS_DATA, OpenRig::EmbeddedWebAssets::MANUAL_SCROLLY_JS_DATA_LEN, "application/javascript; charset=utf-8" },
    { "/assets-readonly/js/browser.min.js", OpenRig::EmbeddedWebAssets::MANUAL_BROWSER_JS_DATA, OpenRig::EmbeddedWebAssets::MANUAL_BROWSER_JS_DATA_LEN, "application/javascript; charset=utf-8" },
    { "/assets-readonly/js/breakpoints.min.js", OpenRig::EmbeddedWebAssets::MANUAL_BREAKPOINTS_JS_DATA, OpenRig::EmbeddedWebAssets::MANUAL_BREAKPOINTS_JS_DATA_LEN, "application/javascript; charset=utf-8" },
    { "/assets-readonly/js/util.js", OpenRig::EmbeddedWebAssets::MANUAL_UTIL_JS_DATA, OpenRig::EmbeddedWebAssets::MANUAL_UTIL_JS_DATA_LEN, "application/javascript; charset=utf-8" },
    { "/assets-readonly/js/main.js", OpenRig::EmbeddedWebAssets::MANUAL_MAIN_JS_DATA, OpenRig::EmbeddedWebAssets::MANUAL_MAIN_JS_DATA_LEN, "application/javascript; charset=utf-8" },
};
} // namespace

namespace OpenRig {

static const char* WS_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

namespace {
    constexpr size_t kMaxHttpHeaderBytes = 16 * 1024;
    constexpr size_t kMaxHttpBodyBytes   = 256 * 1024;
    constexpr uint64_t kMaxWsPayloadBytes = 1024ULL * 1024ULL; // 1 MB hard cap per WebSocket frame

    juce::String generateSessionTokenHex() {
        std::random_device rd;
        char buf[33] = {};
        const uint32_t w0 = rd();
        const uint32_t w1 = rd();
        const uint32_t w2 = rd();
        const uint32_t w3 = rd();
        std::snprintf(buf, sizeof(buf), "%08x%08x%08x%08x", w0, w1, w2, w3);
        return juce::String(buf);
    }

    bool constantTimeEquals(const juce::String& a, const juce::String& b) noexcept {
        const auto* pa = a.toRawUTF8();
        const auto* pb = b.toRawUTF8();
        const size_t la = std::strlen(pa);
        const size_t lb = std::strlen(pb);
        if (la == 0 || la != lb)
            return false;
        unsigned char diff = 0;
        for (size_t i = 0; i < la; ++i)
            diff |= static_cast<unsigned char>(pa[i] ^ pb[i]);
        return diff == 0;
    }

    void setSocketTimeoutsMs(SocketType sock, int recvMs, int sendMs) noexcept {
    #if JUCE_WINDOWS
        DWORD rcv = (DWORD)std::max(0, recvMs);
        DWORD snd = (DWORD)std::max(0, sendMs);
        ::setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&rcv), sizeof(rcv));
        ::setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&snd), sizeof(snd));
    #else
        timeval rcv{ recvMs / 1000, (recvMs % 1000) * 1000 };
        timeval snd{ sendMs / 1000, (sendMs % 1000) * 1000 };
        ::setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &rcv, sizeof(rcv));
        ::setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &snd, sizeof(snd));
    #endif
    }

    bool isLastSocketErrorTimeout() noexcept {
    #if JUCE_WINDOWS
        const int err = ::WSAGetLastError();
        return err == WSAETIMEDOUT || err == WSAEWOULDBLOCK;
    #else
        return errno == EAGAIN || errno == EWOULDBLOCK;
    #endif
    }

    void setTcpNoDelay(SocketType sock) noexcept {
        // Disable Nagle coalescing: without this, small WebSocket frames (touch
        // events, pong replies) and interleaved JPEG frames can stall in the
        // send buffer for 40-200 ms, blowing the Live Plugin Mirror latency
        // budget (< 30 ms).
        int one = 1;
    #if JUCE_WINDOWS
        ::setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, (const char*)&one, sizeof(one));
    #else
        ::setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    #endif
    }

    bool sendAll(SocketType sock, const void* data, size_t len) noexcept {
        const char* ptr = static_cast<const char*>(data);
        size_t sentTotal = 0;
        while (sentTotal < len) {
            const int chunk = static_cast<int>(std::min<size_t>(len - sentTotal, 65536));
            const int n = ::send(sock, ptr + sentTotal, chunk, 0);
            if (n <= 0)
                return false;
            sentTotal += static_cast<size_t>(n);
        }
        return true;
    }

    bool recvExactWs(SocketType sock, void* dst, size_t len, const std::atomic<bool>& running, bool allowIdleTimeout) noexcept {
        uint8_t* ptr = static_cast<uint8_t*>(dst);
        size_t total = 0;
        int idleTimeouts = 0;
        constexpr int kMaxIdleTimeouts = 30; // 30 * 2s = 60s max silence (HUD pings every 5s)
        while (total < len) {
            if (!running.load(std::memory_order_acquire))
                return false;
            const int chunk = static_cast<int>(std::min<size_t>(len - total, 65536));
            const int n = ::recv(sock, reinterpret_cast<char*>(ptr + total), chunk, 0);
            if (n > 0) {
                total += static_cast<size_t>(n);
                idleTimeouts = 0;
                continue;
            }
            if (n < 0 && total == 0 && allowIdleTimeout && isLastSocketErrorTimeout()) {
                if (++idleTimeouts <= kMaxIdleTimeouts && running.load(std::memory_order_acquire))
                    continue;
            }
            return false;
        }
        return true;
    }

    juce::String extractHeaderValue(const juce::String& headers, const juce::String& headerName) {
        auto lines = juce::StringArray::fromLines(headers);
        const juce::String prefix = headerName + ":";
        for (int i = 1; i < lines.size(); ++i) {
            const juce::String line = lines[i].trimEnd();
            if (line.isEmpty())
                break;
            if (line.startsWithIgnoreCase(prefix))
                return line.substring(prefix.length()).trim();
        }
        return {};
    }

    juce::String extractQueryParam(const juce::String& query, const juce::String& paramName) {
        if (query.isEmpty())
            return {};
        auto pairs = juce::StringArray::fromTokens(query, "&", "");
        const juce::String prefix = paramName + "=";
        for (const auto& p : pairs) {
            if (p.startsWith(prefix))
                return juce::URL::removeEscapeChars(p.substring(prefix.length()));
        }
        return {};
    }

    juce::String extractCookieValue(const juce::String& headers, const juce::String& cookieName) {
        const juce::String cookieHeader = extractHeaderValue(headers, "Cookie");
        if (cookieHeader.isEmpty())
            return {};
        auto parts = juce::StringArray::fromTokens(cookieHeader, ";", "");
        const juce::String prefix = cookieName + "=";
        for (const auto& rawPart : parts) {
            const juce::String part = rawPart.trim();
            if (part.startsWith(prefix))
                return juce::URL::removeEscapeChars(part.substring(prefix.length()).trim());
        }
        return {};
    }

    bool readHttpRequest(SocketType sock,
                         juce::String& outMethod,
                         juce::String& outPath,
                         juce::String& outQuery,
                         juce::String& outHeaders,
                         juce::String& outBody,
                         int& outErrorStatus) {
        outErrorStatus = 0;
        std::string raw;
        raw.reserve(4096);
        char chunk[4096];
        size_t headerEndPos = std::string::npos;

        while (headerEndPos == std::string::npos) {
            if (raw.size() >= kMaxHttpHeaderBytes) {
                outErrorStatus = 431;
                return false;
            }
            const int n = ::recv(sock, chunk, (int)sizeof(chunk), 0);
            if (n <= 0)
                return false;
            raw.append(chunk, (size_t)n);
            headerEndPos = raw.find("\r\n\r\n");
        }

        const std::string headerPart = raw.substr(0, headerEndPos);
        outHeaders = juce::String::fromUTF8(headerPart.data(), (int)headerPart.size());

        int firstLineEnd = outHeaders.indexOf("\r\n");
        if (firstLineEnd < 0)
            firstLineEnd = outHeaders.indexOf("\n");
        const juce::String requestLine = (firstLineEnd >= 0) ? outHeaders.substring(0, firstLineEnd) : outHeaders;
        auto tokens = juce::StringArray::fromTokens(requestLine, " ", "");
        if (tokens.size() < 2) {
            outErrorStatus = 400;
            return false;
        }

        outMethod = tokens[0].toUpperCase();
        const juce::String rawTarget = tokens[1];
        const int qIdx = rawTarget.indexOfChar('?');
        if (qIdx >= 0) {
            outPath  = rawTarget.substring(0, qIdx);
            outQuery = rawTarget.substring(qIdx + 1);
            const int hashIdx = outQuery.indexOfChar('#');
            if (hashIdx >= 0)
                outQuery = outQuery.substring(0, hashIdx);
        } else {
            const int hashIdx = rawTarget.indexOfChar('#');
            outPath  = (hashIdx >= 0) ? rawTarget.substring(0, hashIdx) : rawTarget;
            outQuery = {};
        }

        size_t contentLength = 0;
        const juce::String clStr = extractHeaderValue(outHeaders, "Content-Length");
        if (clStr.isNotEmpty()) {
            for (int i = 0; i < clStr.length(); ++i) {
                if (!juce::CharacterFunctions::isDigit(clStr[i])) {
                    outErrorStatus = 400;
                    return false;
                }
            }
            const juce::int64 parsedLen = clStr.getLargeIntValue();
            if (parsedLen < 0 || static_cast<uint64_t>(parsedLen) > kMaxHttpBodyBytes) {
                outErrorStatus = 413;
                return false;
            }
            contentLength = static_cast<size_t>(parsedLen);
        }

        const size_t bodyStart = headerEndPos + 4;
        const size_t totalNeeded = bodyStart + contentLength;
        while (raw.size() < totalNeeded) {
            const size_t rem = totalNeeded - raw.size();
            const int toRead = static_cast<int>(std::min<size_t>(rem, sizeof(chunk)));
            const int n = ::recv(sock, chunk, toRead, 0);
            if (n <= 0)
                return false;
            raw.append(chunk, (size_t)n);
        }

        if (contentLength > 0)
            outBody = juce::String::fromUTF8(raw.data() + bodyStart, (int)contentLength);
        else
            outBody = {};

        return true;
    }

    bool tryJsonInt(const juce::var& v, int& outVal, int minVal, int maxVal) noexcept {
        if (v.isInt() || v.isInt64()) {
            const juce::int64 n = static_cast<juce::int64>(v);
            if (n < static_cast<juce::int64>(minVal) || n > static_cast<juce::int64>(maxVal))
                return false;
            outVal = static_cast<int>(n);
            return true;
        }
        if (v.isDouble()) {
            const double d = static_cast<double>(v);
            if (!std::isfinite(d) || d < static_cast<double>(minVal) || d > static_cast<double>(maxVal))
                return false;
            outVal = static_cast<int>(d);
            return true;
        }
        return false;
    }

    bool tryJsonFloat(const juce::var& v, float& outVal, float minVal, float maxVal) noexcept {
        if (v.isDouble() || v.isInt() || v.isInt64()) {
            const double d = static_cast<double>(v);
            if (!std::isfinite(d))
                return false;
            outVal = juce::jlimit(minVal, maxVal, static_cast<float>(d));
            return true;
        }
        return false;
    }

    bool isAllowedStaticExtension(const juce::String& path) noexcept {
        static const char* const kExts[] = {
            ".html", ".css", ".js", ".png", ".jpg", ".jpeg",
            ".svg", ".ico", ".woff", ".woff2", ".ttf"
        };
        for (const char* ext : kExts) {
            if (path.endsWithIgnoreCase(ext))
                return true;
        }
        return false;
    }

    std::vector<juce::File> getAllowedSongDirectories() {
        std::vector<juce::File> dirs;
        auto primary = Fanfare::RigLibrary::getSongsDirectory();
        if (primary != juce::File())
            dirs.push_back(primary);
        auto appData = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory);
        auto altDir = appData.getChildFile("OpenRig").getChildFile("songs");
        if (altDir.isDirectory() && altDir != primary)
            dirs.push_back(altDir);
        return dirs;
    }

    bool isValidSongCandidate(const juce::File& candidate, const std::vector<juce::File>& allowedDirs) {
        if (candidate == juce::File() || candidate.isSymbolicLink() || !candidate.existsAsFile())
            return false;
        if (!candidate.hasFileExtension("json") && !candidate.hasFileExtension("orsong"))
            return false;
        for (const auto& dir : allowedDirs) {
            if (dir.isDirectory() && candidate.isAChildOf(dir))
                return true;
        }
        return false;
    }

    juce::File resolveAllowedSongFile(const juce::String& rawInput) {
        const juce::String trimmed = rawInput.trim();
        if (trimmed.isEmpty() || trimmed.contains("..") || trimmed.containsChar('\0'))
            return {};

        const auto allowedDirs = getAllowedSongDirectories();
        if (allowedDirs.empty())
            return {};

        if (juce::File::isAbsolutePath(trimmed)) {
            juce::File candidate(trimmed);
            if (isValidSongCandidate(candidate, allowedDirs))
                return candidate;
            return {};
        }

        if (trimmed.containsChar(':') || trimmed.containsChar('\\') || trimmed.startsWithChar('/'))
            return {};

        for (const auto& dir : allowedDirs) {
            if (!dir.isDirectory())
                continue;
            juce::File direct = dir.getChildFile(trimmed);
            if (isValidSongCandidate(direct, allowedDirs))
                return direct;

            for (const char* ext : { ".json", ".orsong" }) {
                juce::Array<juce::File> found;
                dir.findChildFiles(found, juce::File::findFiles, true, trimmed + ext);
                for (const auto& f : found) {
                    if (isValidSongCandidate(f, allowedDirs))
                        return f;
                }
            }
        }
        return {};
    }
}

WebServer::WebServer(FanfareEngine& eng, IWebHost* host)
    : juce::Thread("OpenRigWebServerThread"),
      engine(eng),
      webHost(host),
      sessionToken(generateSessionTokenHex()) {
#if JUCE_WINDOWS
    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);
#endif

    // Live Plugin Mirror: capture -> JPEG -> binary WebSocket fan-out.
    // The sink runs on the streamer's encoder thread and only fans out to
    // subscribed clients; the audience probe keeps the capture pump idle while
    // nobody is watching.
    mirrorCapture = std::make_unique<PluginMirrorCapture>();
    mirrorStreamer = std::make_unique<PluginMirrorStreamer>(*mirrorCapture);
    auto life = lifetime;
    mirrorStreamer->setAudienceProbe([this, life]() {
        if (!life || !life->alive.load(std::memory_order_acquire))
            return false;
        return hasMirrorSubscribers();
    });
    mirrorStreamer->setFrameSink([this, life](const std::vector<uint8_t>& jpeg) {
        if (!life || !life->alive.load(std::memory_order_acquire))
            return;
        broadcastBinaryToMirrorSubscribers(jpeg.data(), jpeg.size());
    });
}

WebServer::~WebServer() {
    stopServer();
#if JUCE_WINDOWS
    WSACleanup();
#endif
}

juce::String WebServer::getLastBindError() const {
    std::lock_guard<std::mutex> lock(bindErrorMutex);
    return lastBindError;
}

bool WebServer::runOnMessageThreadSync(std::function<void()> fn, int timeoutMs) {
    auto* mm = juce::MessageManager::getInstanceWithoutCreating();
    if (mm == nullptr)
        return false;
    if (mm->isThisTheMessageThread()) {
        fn();
        return true;
    }
    struct SyncState {
        juce::WaitableEvent done;
        std::atomic<bool> completed{false};
    };
    auto st = std::make_shared<SyncState>();
    auto life = lifetime;
    juce::MessageManager::callAsync([this, life, st, fn = std::move(fn)]() {
        if (life && life->alive.load(std::memory_order_acquire)
            && serverRunning.load(std::memory_order_acquire)) {
            fn();
            st->completed.store(true, std::memory_order_release);
        }
        st->done.signal();
    });
    const int stepMs = 25;
    for (int elapsed = 0; elapsed < timeoutMs; elapsed += stepMs) {
        if (!serverRunning.load(std::memory_order_acquire))
            return false;
        if (st->done.wait(stepMs))
            return st->completed.load(std::memory_order_acquire);
    }
    return false;
}

bool WebServer::startServer(int port, BindScope scope) {
    if (serverRunning.load() || wsOutboundThread.joinable())
        stopServer();

    {
        std::lock_guard<std::mutex> lock(bindErrorMutex);
        lastBindError.clear();
    }

    serverPort = port;

    if (scope == BindScope::Off) {
        activeBindScope.store(BindScope::Off, std::memory_order_release);
        return true;
    }

    lifetime = std::make_shared<LifetimeState>();
    if (sessionToken.isEmpty())
        sessionToken = generateSessionTokenHex();

    SocketType sock = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCKET_VAL) {
        {
            std::lock_guard<std::mutex> lock(bindErrorMutex);
            lastBindError = "Failed to create TCP socket for port " + juce::String(port);
        }
        LOG_WARN("WebServer: " + getLastBindError());
        activeBindScope.store(BindScope::Off, std::memory_order_release);
        return false;
    }

    int opt = 1;
#if JUCE_WINDOWS
    ::setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));
#else
    ::setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
#endif

    sockaddr_in serverAddr{};
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_addr.s_addr = (scope == BindScope::Loopback) ? htonl(INADDR_LOOPBACK) : INADDR_ANY;
    serverAddr.sin_port = htons((u_short)port);

    if (::bind(sock, (struct sockaddr*)&serverAddr, sizeof(serverAddr)) < 0) {
        {
            std::lock_guard<std::mutex> lock(bindErrorMutex);
            lastBindError = "Port " + juce::String(port) + " is already in use or unavailable";
        }
        LOG_WARN("WebServer: " + getLastBindError());
        activeBindScope.store(BindScope::Off, std::memory_order_release);
        SOCKET_CLOSE(sock);
        return false;
    }

    if (::listen(sock, 16) < 0) {
        {
            std::lock_guard<std::mutex> lock(bindErrorMutex);
            lastBindError = "Port " + juce::String(port) + " failed to listen";
        }
        LOG_WARN("WebServer: " + getLastBindError());
        activeBindScope.store(BindScope::Off, std::memory_order_release);
        SOCKET_CLOSE(sock);
        return false;
    }

    listenSocket = (void*)(uintptr_t)sock;
    {
        std::lock_guard<std::mutex> lock(wsOutMutex);
        hasPendingMeter = false;
        pendingMeterFrame.clear();
        wsOutQueue.clear();
    }
    activeBindScope.store(scope, std::memory_order_release);
    serverRunning.store(true, std::memory_order_release);
    wsOutboundThread = std::thread(&WebServer::wsOutboundLoop, this);

    startThread(juce::Thread::Priority::normal);
    startTimerHz(30); // 30fps real-time VU and telemetry stream

    return true;
}

void WebServer::stopServer() {
    if (lifetime)
        lifetime->alive.store(false, std::memory_order_release);
    serverRunning.store(false, std::memory_order_release);
    activeBindScope.store(BindScope::Off, std::memory_order_release);
    wsOutCv.notify_all();
    stopTimer();
    stopPluginMirror();

    if (listenSocket != nullptr) {
        SocketType s = (SocketType)(uintptr_t)listenSocket;
        SOCKET_SHUTDOWN_BOTH(s);
        SOCKET_CLOSE(s);
        listenSocket = nullptr;
    }

    // Join accept thread first so no new client workers can be spawned
    stopThread(-1);

    // Unblock all active HTTP and WebSocket sockets via shutdown() ONLY.
    // Never call closesocket() here — each worker thread exclusively closes its own socket!
    {
        std::lock_guard<std::mutex> lock(clientSocketsMutex);
        for (void* client : activeHttpSockets) {
            SocketType cs = (SocketType)(uintptr_t)client;
            SOCKET_SHUTDOWN_BOTH(cs);
        }
        for (void* client : activeWsClients) {
            SocketType cs = (SocketType)(uintptr_t)client;
            SOCKET_SHUTDOWN_BOTH(cs);
        }
    }

    if (wsOutboundThread.joinable())
        wsOutboundThread.join();

    // Join all per-connection worker threads before returning (and before WSACleanup in ~WebServer)
    std::vector<std::shared_ptr<ClientWorker>> workersToJoin;
    {
        std::lock_guard<std::mutex> wlock(workersMutex);
        workersToJoin.swap(clientWorkers);
    }
    for (auto& w : workersToJoin) {
        if (w && w->thread.joinable())
            w->thread.join();
    }
}

juce::StringArray WebServer::getLocalIpAddresses() {
    juce::StringArray ips;
    auto addrs = juce::IPAddress::getAllAddresses();
    for (const auto& addr : addrs) {
        if (addr.isNull() || addr.isIPv6)
            continue;
        const uint8_t a = addr.address[0];
        const uint8_t b = addr.address[1];
        if (a == 0 || a == 127 || (a == 169 && b == 254) || a >= 224)
            continue;
        ips.addIfNotAlreadyThere(addr.toString());
    }
    if (ips.isEmpty()) {
        ips.add("127.0.0.1");
    }
    return ips;
}

juce::String WebServer::getPrimaryUrl(int port) {
    auto ips = getLocalIpAddresses();
    return "http://" + ips[0] + ":" + juce::String(port);
}

bool WebServer::isAuthorizedRequest(const juce::String& headers, const juce::String& query) const {
    if (sessionToken.isEmpty())
        return false;

    const juce::String hdrToken = extractHeaderValue(headers, "X-Fanfare-Token");
    if (hdrToken.isNotEmpty() && constantTimeEquals(hdrToken, sessionToken))
        return true;

    const juce::String qToken = extractQueryParam(query, "token");
    if (qToken.isNotEmpty() && constantTimeEquals(qToken, sessionToken))
        return true;

    const juce::String cookieToken = extractCookieValue(headers, "fanfare_session");
    if (cookieToken.isNotEmpty() && constantTimeEquals(cookieToken, sessionToken))
        return true;

    return false;
}

bool WebServer::isOriginAllowed(const juce::String& headers, bool requireOrigin) const {
    juce::String origin = extractHeaderValue(headers, "Origin");
    if (origin.isEmpty())
        return !requireOrigin;

    const juce::String host = extractHeaderValue(headers, "Host");
    if (host.isEmpty())
        return false;

    if (origin.startsWithIgnoreCase("http://"))
        origin = origin.substring(7);
    else if (origin.startsWithIgnoreCase("https://"))
        origin = origin.substring(8);
    else
        return false;

    const int slashIdx = origin.indexOfChar('/');
    if (slashIdx >= 0)
        origin = origin.substring(0, slashIdx);

    return origin.equalsIgnoreCase(host);
}

void WebServer::run() {
    SocketType lSock = (SocketType)(uintptr_t)listenSocket;

    while (!threadShouldExit() && serverRunning.load(std::memory_order_acquire)) {
        fd_set readSet;
        FD_ZERO(&readSet);
        FD_SET(lSock, &readSet);

        timeval tv{ 0, 100000 }; // 100ms timeout
        int ret = ::select((int)lSock + 1, &readSet, nullptr, nullptr, &tv);
        if (ret > 0 && FD_ISSET(lSock, &readSet)) {
            sockaddr_in clientAddr{};
#if JUCE_WINDOWS
            int clientLen = sizeof(clientAddr);
#else
            socklen_t clientLen = sizeof(clientAddr);
#endif
            SocketType clientSock = ::accept(lSock, (struct sockaddr*)&clientAddr, &clientLen);
            if (clientSock != INVALID_SOCKET_VAL) {
                if (!serverRunning.load(std::memory_order_acquire)) {
                    SOCKET_CLOSE(clientSock);
                    break;
                }

                setSocketTimeoutsMs(clientSock, 5000, 5000);
#if defined(SO_NOSIGPIPE)
                {
                    int noSigPipe = 1;
                    ::setsockopt(clientSock, SOL_SOCKET, SO_NOSIGPIPE, &noSigPipe, sizeof(noSigPipe));
                }
#endif
                void* handle = (void*)(uintptr_t)clientSock;
                {
                    std::lock_guard<std::mutex> lock(clientSocketsMutex);
                    activeHttpSockets.insert(handle);
                }

                auto worker = std::make_shared<ClientWorker>();
                {
                    std::lock_guard<std::mutex> wlock(workersMutex);
                    for (auto it = clientWorkers.begin(); it != clientWorkers.end(); ) {
                        if ((*it)->finished.load(std::memory_order_acquire)) {
                            if ((*it)->thread.joinable())
                                (*it)->thread.join();
                            it = clientWorkers.erase(it);
                        } else {
                            ++it;
                        }
                    }
                    worker->thread = std::thread([this, handle, worker]() {
                        try {
                            handleClientConnection(handle);
                        } catch (...) {
                            SocketType s = (SocketType)(uintptr_t)handle;
                            {
                                std::lock_guard<std::mutex> lock(clientSocketsMutex);
                                activeHttpSockets.erase(handle);
                                activeWsClients.erase(handle);
                            }
                            SOCKET_CLOSE(s);
                        }
                        worker->finished.store(true, std::memory_order_release);
                    });
                    clientWorkers.push_back(worker);
                }
            }
        }
    }
}

void WebServer::handleClientConnection(void* socketHandle) {
    SocketType sock = (SocketType)(uintptr_t)socketHandle;

    auto closeOwnedSocket = [&]() {
        {
            std::lock_guard<std::mutex> lock(clientSocketsMutex);
            activeHttpSockets.erase(socketHandle);
            activeWsClients.erase(socketHandle);
            activeWsClientCount.store((int)activeWsClients.size(), std::memory_order_release);
            mirrorSubscribers.erase(socketHandle);
            mirrorSubscriberCount.store((int)mirrorSubscribers.size(), std::memory_order_release);
        }
        SOCKET_CLOSE(sock);
    };

    juce::String method, path, query, headers, body;
    int parseErrorStatus = 0;
    if (!readHttpRequest(sock, method, path, query, headers, body, parseErrorStatus)) {
        if (parseErrorStatus == 413)
            sendHttpResponse(socketHandle, 413, "text/plain", "413 Payload Too Large");
        else if (parseErrorStatus == 431)
            sendHttpResponse(socketHandle, 431, "text/plain", "431 Request Header Fields Too Large");
        else if (parseErrorStatus == 400)
            sendHttpResponse(socketHandle, 400, "text/plain", "400 Bad Request");
        closeOwnedSocket();
        return;
    }

    // Check for WebSocket Upgrade
    const juce::String upgradeHdr = extractHeaderValue(headers, "Upgrade");
    if (upgradeHdr.equalsIgnoreCase("websocket")) {
        if (!isOriginAllowed(headers, true) || !isAuthorizedRequest(headers, query)) {
            sendHttpResponse(socketHandle, 403, "text/plain", "403 Forbidden");
            closeOwnedSocket();
            return;
        }

        const juce::String secKey = extractHeaderValue(headers, "Sec-WebSocket-Key");
        if (secKey.isNotEmpty() && handleWebSocketHandshake(socketHandle, secKey)) {
            setSocketTimeoutsMs(sock, 2000, 500);
            setTcpNoDelay(sock); // low-latency frame delivery for the mirror stream
            {
                std::lock_guard<std::mutex> lock(clientSocketsMutex);
                activeHttpSockets.erase(socketHandle);
                activeWsClients.insert(socketHandle);
                activeWsClientCount.store((int)activeWsClients.size(), std::memory_order_release);
            }
            handleWebSocketSession(socketHandle);
        }
        closeOwnedSocket();
        return;
    }

    // Standard HTTP Request
    handleHttpRequest(socketHandle, method, path, query, headers, body);
    closeOwnedSocket();
}

namespace Sha1Detail {
    inline uint32_t rol(uint32_t value, size_t bits) { return (value << bits) | (value >> (32 - bits)); }
    inline uint32_t blk(const uint32_t block[16], size_t i) {
        return rol(block[(i + 13) & 15] ^ block[(i + 8) & 15] ^ block[(i + 2) & 15] ^ block[i], 1);
    }
    inline void r0(const uint32_t block[16], uint32_t v, uint32_t &w, uint32_t x, uint32_t y, uint32_t &z, size_t i) {
        z += ((w & (x ^ y)) ^ y) + block[i] + 0x5a827999 + rol(v, 5);
        w = rol(w, 30);
    }
    inline void r1(uint32_t block[16], uint32_t v, uint32_t &w, uint32_t x, uint32_t y, uint32_t &z, size_t i) {
        block[i] = blk(block, i);
        z += ((w & (x ^ y)) ^ y) + block[i] + 0x5a827999 + rol(v, 5);
        w = rol(w, 30);
    }
    inline void r2(uint32_t block[16], uint32_t v, uint32_t &w, uint32_t x, uint32_t y, uint32_t &z, size_t i) {
        block[i] = blk(block, i);
        z += (w ^ x ^ y) + block[i] + 0x6ed9eba1 + rol(v, 5);
        w = rol(w, 30);
    }
    inline void r3(uint32_t block[16], uint32_t v, uint32_t &w, uint32_t x, uint32_t y, uint32_t &z, size_t i) {
        block[i] = blk(block, i);
        z += (((w | x) & y) | (w & x)) + block[i] + 0x8f1bbcdc + rol(v, 5);
        w = rol(w, 30);
    }
    inline void r4(uint32_t block[16], uint32_t v, uint32_t &w, uint32_t x, uint32_t y, uint32_t &z, size_t i) {
        block[i] = blk(block, i);
        z += (w ^ x ^ y) + block[i] + 0xca62c1d6 + rol(v, 5);
        w = rol(w, 30);
    }

    inline void transform(uint32_t digest[5], uint32_t block[16]) {
        uint32_t a = digest[0], b = digest[1], c = digest[2], d = digest[3], e = digest[4];
        r0(block, a, b, c, d, e, 0); r0(block, e, a, b, c, d, 1); r0(block, d, e, a, b, c, 2); r0(block, c, d, e, a, b, 3);
        r0(block, b, c, d, e, a, 4); r0(block, a, b, c, d, e, 5); r0(block, e, a, b, c, d, 6); r0(block, d, e, a, b, c, 7);
        r0(block, c, d, e, a, b, 8); r0(block, b, c, d, e, a, 9); r0(block, a, b, c, d, e, 10); r0(block, e, a, b, c, d, 11);
        r0(block, d, e, a, b, c, 12); r0(block, c, d, e, a, b, 13); r0(block, b, c, d, e, a, 14); r0(block, a, b, c, d, e, 15);
        r1(block, e, a, b, c, d, 0); r1(block, d, e, a, b, c, 1); r1(block, c, d, e, a, b, 2); r1(block, b, c, d, e, a, 3);
        r2(block, a, b, c, d, e, 4); r2(block, e, a, b, c, d, 5); r2(block, d, e, a, b, c, 6); r2(block, c, d, e, a, b, 7);
        r2(block, b, c, d, e, a, 8); r2(block, a, b, c, d, e, 9); r2(block, e, a, b, c, d, 10); r2(block, d, e, a, b, c, 11);
        r2(block, c, d, e, a, b, 12); r2(block, b, c, d, e, a, 13); r2(block, a, b, c, d, e, 14); r2(block, e, a, b, c, d, 15);
        r2(block, d, e, a, b, c, 0); r2(block, c, d, e, a, b, 1); r2(block, b, c, d, e, a, 2); r2(block, a, b, c, d, e, 3);
        r2(block, e, a, b, c, d, 4); r2(block, d, e, a, b, c, 5); r2(block, c, d, e, a, b, 6); r2(block, b, c, d, e, a, 7);
        r3(block, a, b, c, d, e, 8); r3(block, e, a, b, c, d, 9); r3(block, d, e, a, b, c, 10); r3(block, c, d, e, a, b, 11);
        r3(block, b, c, d, e, a, 12); r3(block, a, b, c, d, e, 13); r3(block, e, a, b, c, d, 14); r3(block, d, e, a, b, c, 15);
        r3(block, c, d, e, a, b, 0); r3(block, b, c, d, e, a, 1); r3(block, a, b, c, d, e, 2); r3(block, e, a, b, c, d, 3);
        r3(block, d, e, a, b, c, 4); r3(block, c, d, e, a, b, 5); r3(block, b, c, d, e, a, 6); r3(block, a, b, c, d, e, 7);
        r3(block, e, a, b, c, d, 8); r3(block, d, e, a, b, c, 9); r3(block, c, d, e, a, b, 10); r3(block, b, c, d, e, a, 11);
        r4(block, a, b, c, d, e, 12); r4(block, e, a, b, c, d, 13); r4(block, d, e, a, b, c, 14); r4(block, c, d, e, a, b, 15);
        r4(block, b, c, d, e, a, 0); r4(block, a, b, c, d, e, 1); r4(block, e, a, b, c, d, 2); r4(block, d, e, a, b, c, 3);
        r4(block, c, d, e, a, b, 4); r4(block, b, c, d, e, a, 5); r4(block, a, b, c, d, e, 6); r4(block, e, a, b, c, d, 7);
        r4(block, d, e, a, b, c, 8); r4(block, c, d, e, a, b, 9); r4(block, b, c, d, e, a, 10); r4(block, a, b, c, d, e, 11);
        r4(block, e, a, b, c, d, 12); r4(block, d, e, a, b, c, 13); r4(block, c, d, e, a, b, 14); r4(block, b, c, d, e, a, 15);
        digest[0] += a; digest[1] += b; digest[2] += c; digest[3] += d; digest[4] += e;
    }

    inline std::vector<uint8_t> compute(const std::string& input) {
        uint32_t digest[5] = { 0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476, 0xc3d2e1f0 };
        uint64_t bitLength = input.size() * 8;
        std::vector<uint8_t> data(input.begin(), input.end());
        data.push_back(0x80);
        while ((data.size() % 64) != 56) data.push_back(0x00);
        for (int i = 7; i >= 0; --i) data.push_back((uint8_t)((bitLength >> (i * 8)) & 0xFF));

        for (size_t chunk = 0; chunk < data.size(); chunk += 64) {
            uint32_t block[16];
            for (size_t i = 0; i < 16; ++i) {
                block[i] = ((uint32_t)data[chunk + i * 4] << 24) |
                           ((uint32_t)data[chunk + i * 4 + 1] << 16) |
                           ((uint32_t)data[chunk + i * 4 + 2] << 8) |
                           ((uint32_t)data[chunk + i * 4 + 3]);
            }
            transform(digest, block);
        }

        std::vector<uint8_t> result(20);
        for (size_t i = 0; i < 5; ++i) {
            result[i * 4]     = (uint8_t)((digest[i] >> 24) & 0xFF);
            result[i * 4 + 1] = (uint8_t)((digest[i] >> 16) & 0xFF);
            result[i * 4 + 2] = (uint8_t)((digest[i] >> 8) & 0xFF);
            result[i * 4 + 3] = (uint8_t)(digest[i] & 0xFF);
        }
        return result;
    }
}

bool WebServer::handleWebSocketHandshake(void* socketHandle, const juce::String& secWebSocketKey) {
    SocketType sock = (SocketType)(uintptr_t)socketHandle;
    juce::String combined = secWebSocketKey + WS_GUID;

    auto rawSha1 = Sha1Detail::compute(combined.toStdString());
    juce::String acceptKey = juce::Base64::toBase64(rawSha1.data(), rawSha1.size());

    juce::String response =
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: " + acceptKey + "\r\n"
        "\r\n";

    return sendAll(sock, response.toRawUTF8(), (size_t)response.getNumBytesAsUTF8());
}

void WebServer::handleWebSocketSession(void* socketHandle) {
    SocketType sock = (SocketType)(uintptr_t)socketHandle;

    try {
        // Send initial status packet to the newly connected client only
        auto life = lifetime;
        juce::MessageManager::callAsync([this, life, socketHandle]() {
            if (!life || !life->alive.load(std::memory_order_acquire))
                return;
            juce::DynamicObject::Ptr root = new juce::DynamicObject();
            root->setProperty("type", "status");
            root->setProperty("data", buildStatusJson());
            sendWebSocketTextToClient(socketHandle, juce::JSON::toString(juce::var(root.get())));
        });

        // Frame reader loop
        while (serverRunning.load(std::memory_order_acquire)) {
            uint8_t header[2];
            if (!recvExactWs(sock, header, 2, serverRunning, true))
                break;

            uint8_t opcode = header[0] & 0x0F;
            bool masked = (header[1] & 0x80) != 0;
            uint64_t payloadLen = header[1] & 0x7F;

            if (payloadLen == 126) {
                uint8_t ext[2];
                if (!recvExactWs(sock, ext, 2, serverRunning, false)) break;
                payloadLen = ((uint64_t)ext[0] << 8) | ext[1];
            } else if (payloadLen == 127) {
                uint8_t ext[8];
                if (!recvExactWs(sock, ext, 8, serverRunning, false)) break;
                payloadLen = 0;
                for (int i = 0; i < 8; ++i)
                    payloadLen = (payloadLen << 8) | ext[i];
            }

            if (payloadLen > kMaxWsPayloadBytes) {
                // Reject oversized / malformed frames before allocating
                break;
            }

            uint8_t maskKey[4] = { 0 };
            if (masked) {
                if (!recvExactWs(sock, maskKey, 4, serverRunning, false)) break;
            }

            std::vector<uint8_t> payload((size_t)payloadLen);
            if (payloadLen > 0) {
                if (!recvExactWs(sock, payload.data(), (size_t)payloadLen, serverRunning, false))
                    break;
            }

            if (masked) {
                for (size_t i = 0; i < payloadLen; ++i) {
                    payload[i] ^= maskKey[i % 4];
                }
            }

            if (opcode == 0x8) { // Close frame
                break;
            } else if (opcode == 0x9) { // Ping -> echo Pong with payload (capped <= 125 per RFC 6455)
                const size_t pongLen = std::min<size_t>(payload.size(), 125);
                std::vector<uint8_t> pongVec(2 + pongLen);
                pongVec[0] = 0x8A;
                pongVec[1] = static_cast<uint8_t>(pongLen);
                if (pongLen > 0)
                    std::memcpy(pongVec.data() + 2, payload.data(), pongLen);
                enqueueWsFrame(std::move(pongVec), socketHandle, false, false);
                continue;
            } else if (opcode == 0x1) { // Text frame
                juce::String textMsg = juce::String::fromUTF8((const char*)payload.data(), (int)payload.size());
                processIncomingWsMessage(socketHandle, textMsg);
            }
        }
    } catch (...) {
        // Never allow an unhandled exception to terminate a WS worker thread
    }
}

std::vector<uint8_t> WebServer::buildWsFrame(uint8_t finOpcode, const void* data, size_t size) {
    std::vector<uint8_t> frame;
    frame.reserve(size + 10);
    frame.push_back(finOpcode);

    if (size <= 125) {
        frame.push_back(static_cast<uint8_t>(size));
    } else if (size <= 65535) {
        frame.push_back(126);
        frame.push_back(static_cast<uint8_t>((size >> 8) & 0xFF));
        frame.push_back(static_cast<uint8_t>(size & 0xFF));
    } else {
        frame.push_back(127);
        for (int i = 7; i >= 0; --i)
            frame.push_back(static_cast<uint8_t>((size >> (i * 8)) & 0xFF));
    }

    if (data != nullptr && size > 0) {
        const auto* bytes = static_cast<const uint8_t*>(data);
        frame.insert(frame.end(), bytes, bytes + size);
    }
    return frame;
}

void WebServer::enqueueWsFrame(std::vector<uint8_t> frame, void* singleClient, bool isMeter, bool mirrorOnly) {
    if (!serverRunning.load(std::memory_order_acquire) || frame.empty())
        return;

    {
        std::lock_guard<std::mutex> lock(wsOutMutex);
        if (isMeter) {
            pendingMeterFrame = std::move(frame);
            hasPendingMeter = true;
        } else {
            while (wsOutQueue.size() >= 128)
                wsOutQueue.pop_front();
            OutboundWsItem item;
            item.frame = std::move(frame);
            item.singleClient = singleClient;
            item.mirrorOnly = mirrorOnly;
            wsOutQueue.push_back(std::move(item));
        }
    }
    wsOutCv.notify_one();
}

void WebServer::wsOutboundLoop() {
    for (;;) {
        OutboundWsItem item;
        {
            std::unique_lock<std::mutex> lock(wsOutMutex);
            wsOutCv.wait(lock, [this]() {
                return !serverRunning.load(std::memory_order_acquire)
                    || hasPendingMeter
                    || !wsOutQueue.empty();
            });
            if (!serverRunning.load(std::memory_order_acquire))
                break;

            if (!wsOutQueue.empty()) {
                item = std::move(wsOutQueue.front());
                wsOutQueue.pop_front();
            } else if (hasPendingMeter) {
                item.frame = std::move(pendingMeterFrame);
                hasPendingMeter = false;
            }
        }

        if (item.frame.empty())
            continue;

        std::vector<SocketType> targetsToSend;
        {
            std::lock_guard<std::mutex> lock(clientSocketsMutex);
            if (item.singleClient != nullptr) {
                if (activeWsClients.count(item.singleClient) != 0) {
                    targetsToSend.push_back((SocketType)(uintptr_t)item.singleClient);
                }
            } else {
                const auto& targets = item.mirrorOnly ? mirrorSubscribers : activeWsClients;
                targetsToSend.reserve(targets.size());
                for (void* client : targets) {
                    targetsToSend.push_back((SocketType)(uintptr_t)client);
                }
            }
        }

        for (SocketType cs : targetsToSend) {
            sendAll(cs, item.frame.data(), item.frame.size());
        }
    }
}

void WebServer::broadcastWebSocket(const juce::String& jsonMessage) {
    if (activeWsClientCount.load(std::memory_order_acquire) == 0)
        return;
    const size_t payloadLen = static_cast<size_t>(jsonMessage.getNumBytesAsUTF8());
    enqueueWsFrame(buildWsFrame(0x81, jsonMessage.toRawUTF8(), payloadLen), nullptr, false, false);
}

void WebServer::sendWebSocketTextToClient(void* socketHandle, const juce::String& jsonMessage) {
    if (socketHandle == nullptr || activeWsClientCount.load(std::memory_order_acquire) == 0)
        return;
    const size_t payloadLen = static_cast<size_t>(jsonMessage.getNumBytesAsUTF8());
    enqueueWsFrame(buildWsFrame(0x81, jsonMessage.toRawUTF8(), payloadLen), socketHandle, false, false);
}

void WebServer::broadcastBinaryToMirrorSubscribers(const void* data, size_t size) {
    if (data == nullptr || size == 0 || mirrorSubscriberCount.load(std::memory_order_acquire) == 0)
        return;
    enqueueWsFrame(buildWsFrame(0x82, data, size), nullptr, false, true);
}

bool WebServer::hasMirrorSubscribers() {
    // Lock-free: called from the capture timer (message thread) and the
    // encoder thread - must never block behind socket I/O.
    return mirrorSubscriberCount.load(std::memory_order_acquire) > 0;
}

struct WebServer::Dispatch {
    using WsHandler = void (*)(WebServer&, void*, const juce::var&, const juce::String&);
    using PostHandler = juce::String (*)(WebServer&, const juce::var&);

    static void selectSceneAsync(WebServer& s, int idx) {
        auto life = s.lifetime;
        WebServer* self = &s;
        juce::MessageManager::callAsync([self, life, idx]() {
            if (!life || !life->alive.load(std::memory_order_acquire))
                return;
            if (idx < 0 || idx >= self->engine.getNumScenes())
                return;
            if (self->webHost != nullptr) {
                self->webHost->recallSnapshot(idx);
            } else {
                self->engine.loadScene(idx);
                self->notifySceneChanged(idx, self->engine.getSceneName(idx));
            }
        });
    }

    static void stepSetlistAsync(WebServer& s, int delta) {
        auto life = s.lifetime;
        WebServer* self = &s;
        juce::MessageManager::callAsync([self, life, delta]() {
            if (!life || !life->alive.load(std::memory_order_acquire) || self->webHost == nullptr)
                return;
            auto& sm = Fanfare::SetlistManager::getInstance();
            const bool canStep = (delta < 0) ? sm.hasPrev() : sm.hasNext();
            if (!canStep)
                return;
            const int targetIdx = sm.getActiveIndex() + delta;
            const auto setups = sm.getSetupsSnapshot();
            if (targetIdx >= 0 && targetIdx < setups.size())
                self->webHost->loadRigFromFile(setups[targetIdx], targetIdx);
        });
    }

    static void sendSnapshotAsync(WebServer& s, void* socketHandle, const char* msgType,
                                  juce::var (WebServer::*builder)()) {
        auto life = s.lifetime;
        WebServer* self = &s;
        juce::String typeStr(msgType);
        juce::MessageManager::callAsync([self, life, socketHandle, typeStr, builder]() {
            if (!life || !life->alive.load(std::memory_order_acquire))
                return;
            juce::DynamicObject::Ptr root = new juce::DynamicObject();
            root->setProperty("type", typeStr);
            root->setProperty("data", (self->*builder)());
            self->sendWebSocketTextToClient(socketHandle, juce::JSON::toString(juce::var(root.get())));
        });
    }

    static juce::String mp3TransportAsync(WebServer& s, void (*action)(Mp3PlayerProcessor*)) {
        auto life = s.lifetime;
        WebServer* self = &s;
        juce::MessageManager::callAsync([self, life, action]() {
            if (!life || !life->alive.load(std::memory_order_acquire) || !self->serverRunning.load())
                return;
            if (auto* mp3 = self->engine.getMp3Player()) {
                action(mp3);
                self->notifyMp3Changed();
            }
        });
        return "{\"ok\":true}";
    }

    static void wsSelectScene(WebServer& s, void*, const juce::var& json, const juce::String&) {
        int idx = 0;
        if (tryJsonInt(json["index"], idx, 0, 1024))
            selectSceneAsync(s, idx);
    }

    static void wsPrevSetlist(WebServer& s, void*, const juce::var&, const juce::String&) {
        stepSetlistAsync(s, -1);
    }

    static void wsNextSetlist(WebServer& s, void*, const juce::var&, const juce::String&) {
        stepSetlistAsync(s, +1);
    }

    static void wsSelectSetlist(WebServer& s, void*, const juce::var& json, const juce::String&) {
        int targetIdx = 0;
        if (!tryJsonInt(json["index"], targetIdx, 0, 10000))
            return;
        auto life = s.lifetime;
        WebServer* self = &s;
        juce::MessageManager::callAsync([self, life, targetIdx]() {
            if (!life || !life->alive.load(std::memory_order_acquire) || self->webHost == nullptr)
                return;
            auto& sm = Fanfare::SetlistManager::getInstance();
            auto setups = sm.getSetupsSnapshot();
            if (targetIdx >= 0 && targetIdx < setups.size())
                self->webHost->loadRigFromFile(setups[targetIdx], targetIdx);
        });
    }

    static void wsLoadSong(WebServer& s, void*, const juce::var& json, const juce::String&) {
        juce::String pathOrName = json["path"].toString();
        auto life = s.lifetime;
        WebServer* self = &s;
        juce::MessageManager::callAsync([self, life, pathOrName]() {
            if (!life || !life->alive.load(std::memory_order_acquire) || self->webHost == nullptr)
                return;
            juce::File f = resolveAllowedSongFile(pathOrName);
            if (f.existsAsFile())
                self->webHost->loadRigFromFile(f);
        });
    }

    static void wsQueueSong(WebServer& s, void*, const juce::var& json, const juce::String&) {
        juce::String pathOrName = json["path"].toString();
        auto life = s.lifetime;
        juce::MessageManager::callAsync([life, pathOrName]() {
            if (!life || !life->alive.load(std::memory_order_acquire))
                return;
            juce::File f = resolveAllowedSongFile(pathOrName);
            if (f.existsAsFile()) {
                auto& sm = Fanfare::SetlistManager::getInstance();
                sm.addSetup(f);
            }
        });
    }

    static void wsToggleFavorite(WebServer& s, void*, const juce::var& json, const juce::String&) {
        juce::String pathOrName = json["path"].toString();
        auto life = s.lifetime;
        juce::MessageManager::callAsync([life, pathOrName]() {
            if (!life || !life->alive.load(std::memory_order_acquire))
                return;
            const juce::File validatedSong = resolveAllowedSongFile(pathOrName);
            if (!validatedSong.existsAsFile())
                return;
            const juce::String canonicalPath = validatedSong.getFullPathName();
            const juce::String baseName = validatedSong.getFileNameWithoutExtension();

            auto appData = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory);
            for (const auto& appName : { "Fanfare", "OpenRig" }) {
                auto favFile = appData.getChildFile(appName).getChildFile("library_favorites.txt");
                std::set<juce::String> favs;
                if (favFile.existsAsFile()) {
                    juce::StringArray lines;
                    favFile.readLines(lines);
                    for (const auto& l : lines)
                        if (l.trim().isNotEmpty())
                            favs.insert(l.trim());
                }
                const bool wasFav = (favs.count(canonicalPath) > 0 || favs.count(baseName) > 0);
                if (wasFav) {
                    favs.erase(canonicalPath);
                    favs.erase(baseName);
                } else {
                    favs.insert(canonicalPath);
                }

                juce::String outText;
                for (const auto& favPath : favs)
                    outText += favPath + "\n";
                favFile.getParentDirectory().createDirectory();
                juce::TemporaryFile tmp(favFile);
                if (tmp.getFile().replaceWithText(outText))
                    tmp.overwriteTargetFileWithTemporary();
            }
        });
    }

    static void wsPanic(WebServer& s, void*, const juce::var&, const juce::String&) {
        s.engine.triggerPanic();
    }

    static void wsSetFader(WebServer& s, void*, const juce::var& json, const juce::String&) {
        juce::String fader = json["fader"].toString();
        float val = 0.0f;
        if (!tryJsonFloat(json["value"], val, 0.0f, 1.0f))
            return;
        auto life = s.lifetime;
        WebServer* self = &s;
        juce::MessageManager::callAsync([self, life, fader, val]() {
            if (!life || !life->alive.load(std::memory_order_acquire))
                return;
            if (fader == "masterFoh") self->engine.setFohMasterLevel(val);
            else if (fader == "masterIem") self->engine.setIemMasterLevel(val);
        });
    }

    static void wsSetSlotFader(WebServer& s, void*, const juce::var& json, const juce::String&) {
        int slotIdx = 0;
        float val = 0.0f;
        if (!tryJsonInt(json["slot"], slotIdx, 0, 128))
            return;
        if (!tryJsonFloat(json["value"], val, 0.0f, 1.0f))
            return;
        juce::String ch = json["channel"].toString();
        auto life = s.lifetime;
        WebServer* self = &s;
        juce::MessageManager::callAsync([self, life, slotIdx, ch, val]() {
            if (!life || !life->alive.load(std::memory_order_acquire))
                return;
            if (slotIdx < 0 || slotIdx >= self->engine.getNumSlots())
                return;
            if (auto* slot = self->engine.getSlot(slotIdx)) {
                if (ch == "foh") slot->setFohLevel(val);
                else if (ch == "iem") slot->setIemOffset(val * 2.0f);
            }
        });
    }

    static void wsSetSlotMute(WebServer& s, void*, const juce::var& json, const juce::String&) {
        int slotIdx = 0;
        if (!tryJsonInt(json["slot"], slotIdx, 0, 128))
            return;
        juce::String ch = json["channel"].toString();
        bool enabled = static_cast<bool>(json["enabled"]);
        auto life = s.lifetime;
        WebServer* self = &s;
        juce::MessageManager::callAsync([self, life, slotIdx, ch, enabled]() {
            if (!life || !life->alive.load(std::memory_order_acquire))
                return;
            if (slotIdx < 0 || slotIdx >= self->engine.getNumSlots())
                return;
            if (auto* slot = self->engine.getSlot(slotIdx)) {
                if (ch == "foh") slot->setFohEnabled(enabled);
                else if (ch == "iem") slot->setIemEnabled(enabled);
            }
        });
    }

    static void wsSetNotepadTab(WebServer& s, void*, const juce::var& json, const juce::String&) {
        int idx = 0;
        if (!tryJsonInt(json["index"], idx, 0, 15))
            return;
        auto life = s.lifetime;
        WebServer* self = &s;
        juce::MessageManager::callAsync([self, life, idx]() {
            if (!life || !life->alive.load(std::memory_order_acquire))
                return;
            const int maxTab = std::max(0, (int)self->engine.getNotes().size() - 1);
            self->engine.setActiveNoteTabIndex(juce::jlimit(0, maxTab, idx));
            self->engine.broadcastNotesChanged();
        });
    }

    static void wsSetNotepadFontSize(WebServer& s, void*, const juce::var& json, const juce::String&) {
        float sz = 20.0f;
        if (!tryJsonFloat(json["size"], sz, 10.0f, 72.0f))
            return;
        auto life = s.lifetime;
        WebServer* self = &s;
        juce::MessageManager::callAsync([self, life, sz]() {
            if (!life || !life->alive.load(std::memory_order_acquire))
                return;
            self->engine.setNoteFontSize(sz);
            self->engine.broadcastNotesChanged();
        });
    }

    static void wsSetNotepadMonospace(WebServer& s, void*, const juce::var& json, const juce::String&) {
        bool mono = static_cast<bool>(json["isMonospace"]);
        auto life = s.lifetime;
        WebServer* self = &s;
        juce::MessageManager::callAsync([self, life, mono]() {
            if (!life || !life->alive.load(std::memory_order_acquire))
                return;
            self->engine.setNoteIsMonospace(mono);
            self->engine.broadcastNotesChanged();
        });
    }

    static void wsUpdateNotepadContent(WebServer& s, void*, const juce::var& json, const juce::String&) {
        int idx = 0;
        if (!tryJsonInt(json["index"], idx, 0, 15))
            return;
        juce::String content = json["content"].toString();
        auto life = s.lifetime;
        WebServer* self = &s;
        juce::MessageManager::callAsync([self, life, idx, content]() {
            if (!life || !life->alive.load(std::memory_order_acquire))
                return;
            auto notes = self->engine.getNotes();
            if (idx >= 0 && idx < (int)notes.size()) {
                notes[idx].content = content;
                self->engine.setNotes(notes);
                self->engine.broadcastNotesChanged();
            }
        });
    }

    static void wsGetStatus(WebServer& s, void* sock, const juce::var&, const juce::String&) {
        sendSnapshotAsync(s, sock, "status", &WebServer::buildStatusJson);
    }

    static void wsGetNotepad(WebServer& s, void* sock, const juce::var&, const juce::String&) {
        sendSnapshotAsync(s, sock, "notepad", &WebServer::buildNotepadJson);
    }

    static void wsGetMixer(WebServer& s, void* sock, const juce::var&, const juce::String&) {
        sendSnapshotAsync(s, sock, "mixer", &WebServer::buildMixerJson);
    }

    static void wsMirrorSubscribe(WebServer& s, void* sock, const juce::var&, const juce::String&) {
        s.handleMirrorSubscribe(sock);
    }

    static void wsMirrorUnsubscribe(WebServer& s, void* sock, const juce::var&, const juce::String&) {
        s.handleMirrorUnsubscribe(sock);
    }

    static void wsPluginTouch(WebServer& s, void*, const juce::var&, const juce::String& messageText) {
        // Reverse touch input: {"type":"plugin_touch","action":"down","x":420,"y":180}
        PluginMirrorInputHandler::TouchEvent evt;
        if (!PluginMirrorInputHandler::parseTouchMessage(messageText, evt))
            return;
        s.mirrorInput.inject(evt, s.mirrorCapture->getFrameInfo());
    }

    static void wsGetMirrorStatus(WebServer& s, void* sock, const juce::var&, const juce::String&) {
        s.sendWebSocketTextToClient(sock, s.getPluginMirrorStatusJson());
    }

    static void wsPing(WebServer& s, void* sock, const juce::var& json, const juce::String&) {
        juce::DynamicObject::Ptr root = new juce::DynamicObject();
        root->setProperty("type", "pong");
        root->setProperty("time", json["time"]);
        if (json.hasProperty("cid")) root->setProperty("cid", json["cid"]);
        s.sendWebSocketTextToClient(sock, juce::JSON::toString(juce::var(root.get())));
    }

    static juce::String postSelectScene(WebServer& s, const juce::var& jsonPayload) {
        int idx = 0;
        if (!jsonPayload.isObject() || !tryJsonInt(jsonPayload["index"], idx, 0, 1024))
            return "{\"ok\":false,\"error\":\"Invalid scene index\"}";
        selectSceneAsync(s, idx);
        return "{\"ok\":true}";
    }

    static juce::String postPanic(WebServer& s, const juce::var&) {
        s.engine.triggerPanic();
        return "{\"ok\":true}";
    }

    static juce::String postMp3Play(WebServer& s, const juce::var&) {
        return mp3TransportAsync(s, [](Mp3PlayerProcessor* mp3) { mp3->play(); });
    }

    static juce::String postMp3Pause(WebServer& s, const juce::var&) {
        return mp3TransportAsync(s, [](Mp3PlayerProcessor* mp3) { mp3->pause(); });
    }

    static juce::String postMp3Next(WebServer& s, const juce::var&) {
        return mp3TransportAsync(s, [](Mp3PlayerProcessor* mp3) { mp3->nextTrack(true); });
    }

    static juce::String postMp3Prev(WebServer& s, const juce::var&) {
        return mp3TransportAsync(s, [](Mp3PlayerProcessor* mp3) { mp3->prevTrack(); });
    }

    static juce::String postMp3Bank(WebServer& s, const juce::var& jsonPayload) {
        int bankIdx = 0;
        if (!jsonPayload.isObject() || !tryJsonInt(jsonPayload["index"], bankIdx, 0, 256))
            return "{\"ok\":false,\"error\":\"Invalid bank index\"}";
        auto life = s.lifetime;
        WebServer* self = &s;
        juce::MessageManager::callAsync([self, life, bankIdx]() {
            if (!life || !life->alive.load(std::memory_order_acquire) || !self->serverRunning.load()) return;
            if (auto* mp3 = self->engine.getMp3Player()) {
                mp3->loadBank(bankIdx, true);
                self->notifyMp3Changed();
            }
        });
        return "{\"ok\":true}";
    }

    static juce::String postMp3Track(WebServer& s, const juce::var& jsonPayload) {
        int trkIdx = 0;
        if (!jsonPayload.isObject() || !tryJsonInt(jsonPayload["index"], trkIdx, 0, 100000))
            return "{\"ok\":false,\"error\":\"Invalid track index\"}";
        auto life = s.lifetime;
        WebServer* self = &s;
        juce::MessageManager::callAsync([self, life, trkIdx]() {
            if (!life || !life->alive.load(std::memory_order_acquire) || !self->serverRunning.load()) return;
            if (auto* mp3 = self->engine.getMp3Player()) {
                mp3->playTrack(trkIdx, true);
                self->notifyMp3Changed();
            }
        });
        return "{\"ok\":true}";
    }

    static juce::String postMp3Gain(WebServer& s, const juce::var& jsonPayload) {
        float g = 1.0f;
        if (!jsonPayload.isObject() || !tryJsonFloat(jsonPayload["gain"], g, 0.0f, 2.0f))
            return "{\"ok\":false,\"error\":\"Invalid gain\"}";
        if (auto* mp3 = s.engine.getMp3Player()) {
            mp3->setGain(g);
            s.notifyMp3Changed();
        }
        return "{\"ok\":true}";
    }

    static juce::String postMp3AutoDj(WebServer& s, const juce::var& jsonPayload) {
        if (!jsonPayload.isObject())
            return "{\"ok\":false,\"error\":\"Invalid payload\"}";
        bool a = static_cast<bool>(jsonPayload["enabled"]);
        if (auto* mp3 = s.engine.getMp3Player()) {
            mp3->setAutoDjEnabled(a);
            s.notifyMp3Changed();
        }
        return "{\"ok\":true}";
    }

    static juce::String postMp3Download(WebServer& s, const juce::var& jsonPayload) {
        if (!jsonPayload.isObject())
            return "{\"ok\":false,\"error\":\"Invalid payload\"}";
        juce::String url = jsonPayload["url"].toString().trim();
        bool playNow = (bool)jsonPayload.getProperty("playNow", false);
        if (!YoutubeDownloadManager::isValidDownloadUrl(url))
            return "{\"ok\":false,\"error\":\"Invalid URL: only http:// and https:// URLs are permitted\"}";
        if (url.isEmpty())
            return "{\"ok\":false,\"error\":\"Empty URL\"}";

        auto life = s.lifetime;
        WebServer* self = &s;
        YoutubeDownloadManager::getInstance().queueDownload(url, playNow,
            [self, life, playNow](bool success, const juce::File& file, const juce::String& /*title*/, const juce::String& /*err*/) {
                if (!life || !life->alive.load(std::memory_order_acquire) || !self->serverRunning.load()) return;
                if (success && file.existsAsFile()) {
                    if (auto* mp3 = self->engine.getMp3Player()) {
                        mp3->addFile(file);
                        if (playNow) {
                            int newIdx = (int)mp3->getPlaylistSnapshot().size() - 1;
                            mp3->playTrack(newIdx, true);
                        }
                        self->notifyMp3Changed();
                    }
                }
            },
            [self, life](const juce::String& status, float /*progress*/) {
                if (!life || !life->alive.load(std::memory_order_acquire) || !self->serverRunning.load()) return;
                juce::DynamicObject::Ptr root = new juce::DynamicObject();
                root->setProperty("type", "mp3_download_status");
                root->setProperty("status", status);
                self->broadcastWebSocket(juce::JSON::toString(juce::var(root.get())));
            }
        );
        return "{\"ok\":true,\"message\":\"Download queued\"}";
    }
};

void WebServer::processIncomingWsMessage(void* socketHandle, const juce::String& messageText) {
    auto json = juce::JSON::parse(messageText);
    if (!json.isObject()) return;

    const std::string typeKey = json["type"].toString().toStdString();
    static const std::unordered_map<std::string, Dispatch::WsHandler> kWsHandlers = {
        { "select_scene",          &Dispatch::wsSelectScene },
        { "prev_setlist",          &Dispatch::wsPrevSetlist },
        { "next_setlist",          &Dispatch::wsNextSetlist },
        { "select_setlist",        &Dispatch::wsSelectSetlist },
        { "load_song",             &Dispatch::wsLoadSong },
        { "queue_song",            &Dispatch::wsQueueSong },
        { "toggle_favorite",       &Dispatch::wsToggleFavorite },
        { "panic",                 &Dispatch::wsPanic },
        { "set_fader",             &Dispatch::wsSetFader },
        { "set_slot_fader",        &Dispatch::wsSetSlotFader },
        { "set_slot_mute",         &Dispatch::wsSetSlotMute },
        { "set_notepad_tab",       &Dispatch::wsSetNotepadTab },
        { "set_notepad_font_size", &Dispatch::wsSetNotepadFontSize },
        { "set_notepad_monospace", &Dispatch::wsSetNotepadMonospace },
        { "update_notepad_content",&Dispatch::wsUpdateNotepadContent },
        { "get_status",            &Dispatch::wsGetStatus },
        { "get_notepad",           &Dispatch::wsGetNotepad },
        { "get_mixer",             &Dispatch::wsGetMixer },
        { "mirror_subscribe",      &Dispatch::wsMirrorSubscribe },
        { "mirror_unsubscribe",    &Dispatch::wsMirrorUnsubscribe },
        { "plugin_touch",          &Dispatch::wsPluginTouch },
        { "get_mirror_status",     &Dispatch::wsGetMirrorStatus },
        { "ping",                  &Dispatch::wsPing },
    };

    const auto it = kWsHandlers.find(typeKey);
    if (it != kWsHandlers.end())
        it->second(*this, socketHandle, json, messageText);
}

void WebServer::timerCallback() {
    if (activeWsClientCount.load(std::memory_order_acquire) == 0)
        return;

    // 30Hz high-frequency VU and telemetry broadcast (calibrated logarithmic dB scale)
    juce::DynamicObject::Ptr root = new juce::DynamicObject();
    root->setProperty("type", "meter");
    root->setProperty("fohL", FanfareLog::amplitudeToLogScale(engine.getFohPeakL()));
    root->setProperty("fohR", FanfareLog::amplitudeToLogScale(engine.getFohPeakR()));
    root->setProperty("iemL", FanfareLog::amplitudeToLogScale(engine.getIemPeakL()));
    root->setProperty("iemR", FanfareLog::amplitudeToLogScale(engine.getIemPeakR()));
    root->setProperty("cpu", webHost ? (float)webHost->getCpuUsage() : 0.0f);
    if (++telemetryThrottleCount >= 15) {
        telemetryThrottleCount = 0;
        root->setProperty("underruns", webHost ? webHost->getAudioUnderruns() : 0);
        const bool therm = webHost ? webHost->isThermalWarningActive() : false;
        root->setProperty("thermalAlert", therm);
        root->setProperty("cpuTempC", webHost ? webHost->getCpuTemperatureCelsius() : -1);
        if (therm && webHost != nullptr)
            root->setProperty("thermalText", webHost->getThermalBadgeText());
    }

    const juce::String meterJson = juce::JSON::toString(juce::var(root.get()));
    const size_t payloadLen = static_cast<size_t>(meterJson.getNumBytesAsUTF8());
    enqueueWsFrame(buildWsFrame(0x81, meterJson.toRawUTF8(), payloadLen), nullptr, true, false);
}

void WebServer::notifySceneChanged(int sceneIndex, const juce::String& sceneName) {
    juce::DynamicObject::Ptr root = new juce::DynamicObject();
    root->setProperty("type", "scene_changed");
    root->setProperty("sceneIndex", sceneIndex);
    root->setProperty("sceneName", sceneName);
    broadcastWebSocket(juce::JSON::toString(juce::var(root.get())));
}

void WebServer::notifySetlistChanged() {
    juce::DynamicObject::Ptr root = new juce::DynamicObject();
    root->setProperty("type", "setlist_changed");
    root->setProperty("status", buildStatusJson());
    broadcastWebSocket(juce::JSON::toString(juce::var(root.get())));
}

void WebServer::notifyNotesChanged() {
    juce::DynamicObject::Ptr root = new juce::DynamicObject();
    root->setProperty("type", "notepad");
    root->setProperty("data", buildNotepadJson());
    broadcastWebSocket(juce::JSON::toString(juce::var(root.get())));
}

void WebServer::notifyMixerChanged() {
    juce::DynamicObject::Ptr root = new juce::DynamicObject();
    root->setProperty("type", "mixer");
    root->setProperty("data", buildMixerJson());
    broadcastWebSocket(juce::JSON::toString(juce::var(root.get())));
}

void WebServer::notifyMp3Changed() {
    juce::DynamicObject::Ptr root = new juce::DynamicObject();
    root->setProperty("type", "mp3");
    root->setProperty("data", buildMp3Json());
    broadcastWebSocket(juce::JSON::toString(juce::var(root.get())));
}

// =====================================================================
// Live Plugin Mirror (prototype)
// =====================================================================

void WebServer::registerPluginMirrorSource(juce::Component* editorContent,
                                           void* nativeWindowHandle,
                                           const juce::String& name) {
    PluginMirrorCapture::Target target;
    if (nativeWindowHandle != nullptr) {
        // Native capture is preferred: VST3 editors hosting their own child
        // windows do not paint into createComponentSnapshot.
        target.kind = PluginMirrorCapture::Target::Kind::NativeWindow;
        target.nativeWindowHandle = nativeWindowHandle;
    } else if (editorContent != nullptr) {
        target.kind = PluginMirrorCapture::Target::Kind::JuceComponent;
        target.component = editorContent;
    }
    target.name = name;

    if (mirrorCapture != nullptr)
        mirrorCapture->setTarget(target);

    // Geometry may have changed - refresh subscribed clients.
    if (hasMirrorSubscribers())
        broadcastWebSocket(juce::JSON::toString(buildMirrorInfoJson()));
}

void WebServer::unregisterPluginMirrorSource(juce::Component* editorContent,
                                             void* nativeWindowHandle) {
    if (mirrorCapture == nullptr)
        return;

    const auto cur = mirrorCapture->getTarget();
    const bool wildcardClear =
        (editorContent == nullptr && nativeWindowHandle == nullptr);
    const bool matchesNative =
        (cur.kind == PluginMirrorCapture::Target::Kind::NativeWindow &&
         nativeWindowHandle != nullptr &&
         cur.nativeWindowHandle == nativeWindowHandle);
    const bool matchesComponent =
        (cur.kind == PluginMirrorCapture::Target::Kind::JuceComponent &&
         editorContent != nullptr &&
         cur.component.getComponent() == editorContent);

    if (wildcardClear || matchesNative || matchesComponent) {
        stopPluginMirror();
        mirrorCapture->clearTarget();
    }
}

bool WebServer::startPluginMirror(int targetFps, float jpegQuality) {
    if (mirrorStreamer == nullptr || mirrorCapture == nullptr || !mirrorCapture->hasTarget())
        return false;
    mirrorStreamer->start(targetFps, jpegQuality);
    if (hasMirrorSubscribers())
        broadcastWebSocket(juce::JSON::toString(buildMirrorInfoJson()));
    return true;
}

void WebServer::stopPluginMirror() {
    if (mirrorStreamer != nullptr) {
        mirrorStreamer->setFrameSink(nullptr);
        mirrorStreamer->setAudienceProbe(nullptr);
        mirrorStreamer->stop();
    }
    std::lock_guard<std::mutex> lock(clientSocketsMutex);
    mirrorSubscribers.clear();
    mirrorSubscriberCount.store(0, std::memory_order_release);
}

bool WebServer::isPluginMirrorActive() const {
    return mirrorStreamer != nullptr && mirrorStreamer->isStreaming();
}

void WebServer::handleMirrorSubscribe(void* socketHandle) {
    {
        std::lock_guard<std::mutex> lock(clientSocketsMutex);
        mirrorSubscribers.insert(socketHandle);
        mirrorSubscriberCount.store((int)mirrorSubscribers.size(), std::memory_order_release);
    }

    // Announce stream geometry so the client can map touch coordinates.
    sendWebSocketTextToClient(socketHandle, juce::JSON::toString(buildMirrorInfoJson()));

    // Lazy start on the message thread (juce::Timer affinity).
    auto life = lifetime;
    juce::MessageManager::callAsync([this, life]() {
        if (!life || !life->alive.load(std::memory_order_acquire))
            return;
        if (!isPluginMirrorActive() && mirrorCapture != nullptr && mirrorCapture->hasTarget())
            startPluginMirror();
    });
}

void WebServer::handleMirrorUnsubscribe(void* socketHandle) {
    std::lock_guard<std::mutex> lock(clientSocketsMutex);
    mirrorSubscribers.erase(socketHandle);
    mirrorSubscriberCount.store((int)mirrorSubscribers.size(), std::memory_order_release);
}

juce::var WebServer::buildMirrorInfoJson() {
    const auto frame = mirrorCapture != nullptr ? mirrorCapture->getFrameInfo()
                                                : PluginMirrorCapture::FrameInfo();
    const auto target = mirrorCapture != nullptr ? mirrorCapture->getTarget()
                                                 : PluginMirrorCapture::Target();

    juce::DynamicObject::Ptr root = new juce::DynamicObject();
    root->setProperty("type", "mirror_info");
    root->setProperty("active", isPluginMirrorActive());
    root->setProperty("target", target.name);
    root->setProperty("width", frame.frameWidth);
    root->setProperty("height", frame.frameHeight);
    root->setProperty("fps", mirrorStreamer != nullptr ? mirrorStreamer->getTargetFps() : 0);
    root->setProperty("jpegQuality", mirrorStreamer != nullptr ? (double)mirrorStreamer->getJpegQuality() : 0.0);
    return juce::var(root.get());
}

juce::String WebServer::getPluginMirrorStatusJson() {
    const auto frame = mirrorCapture != nullptr ? mirrorCapture->getFrameInfo()
                                                : PluginMirrorCapture::FrameInfo();
    const auto target = mirrorCapture != nullptr ? mirrorCapture->getTarget()
                                                 : PluginMirrorCapture::Target();

    juce::DynamicObject::Ptr root = new juce::DynamicObject();
    root->setProperty("type", "mirror_status");
    root->setProperty("active", isPluginMirrorActive());
    root->setProperty("target", target.name);
    root->setProperty("width", frame.frameWidth);
    root->setProperty("height", frame.frameHeight);

    if (mirrorStreamer != nullptr) {
        auto& stats = mirrorStreamer->getStats();
        root->setProperty("fps", mirrorStreamer->getTargetFps());
        root->setProperty("measuredFps", stats.measuredFps.load());
        root->setProperty("jpegQuality", (double)mirrorStreamer->getJpegQuality());
        root->setProperty("framesCaptured", stats.framesCaptured.load());
        root->setProperty("framesEncoded", stats.framesEncoded.load());
        root->setProperty("framesSent", stats.framesSent.load());
        root->setProperty("framesDropped", stats.framesDropped.load());
        root->setProperty("jpegBytes", (int)stats.lastJpegBytes.load());
        root->setProperty("encodeMs", stats.lastEncodeMs.load());
    }
    root->setProperty("subscribers", mirrorSubscriberCount.load(std::memory_order_acquire));
    return juce::JSON::toString(juce::var(root.get()));
}

// REST API and Static Request Router
bool WebServer::handleHttpRequest(void* socketHandle,
                                  const juce::String& method,
                                  const juce::String& path,
                                  const juce::String& query,
                                  const juce::String& headers,
                                  const juce::String& body) {
    if (method == "OPTIONS") {
        if (!isOriginAllowed(headers, false)) {
            sendHttpResponse(socketHandle, 403, "text/plain", "403 Forbidden");
            return false;
        }
        sendHttpResponse(socketHandle, 204, "text/plain", "");
        return true;
    }

    if (path.startsWith("/api/")) {
        if (!isOriginAllowed(headers, false)) {
            sendHttpResponse(socketHandle, 403, "application/json", "{\"ok\":false,\"error\":\"Forbidden origin\"}");
            return false;
        }
        if (!isAuthorizedRequest(headers, query)) {
            sendHttpResponse(socketHandle, 401, "application/json", "{\"ok\":false,\"error\":\"Unauthorized\"}");
            return false;
        }

        if (method == "GET") {
            std::function<juce::var()> builder;
            if (path == "/api/status")       builder = [this]() { return buildStatusJson(); };
            else if (path == "/api/scenes")  builder = [this]() { return buildScenesJson(); };
            else if (path == "/api/setlist") builder = [this]() { return buildSetlistJson(); };
            else if (path == "/api/notepad") builder = [this]() { return buildNotepadJson(); };
            else if (path == "/api/mixer")   builder = [this]() { return buildMixerJson(); };
            else if (path == "/api/mp3")     builder = [this]() { return buildMp3Json(); };

            if (builder) {
                juce::String jsonOut;
                const bool ok = runOnMessageThreadSync([&]() {
                    jsonOut = juce::JSON::toString(builder());
                });
                if (!ok) {
                    sendHttpResponse(socketHandle, 503, "application/json",
                                     "{\"ok\":false,\"error\":\"Server shutting down or busy\"}");
                    return false;
                }
                sendHttpResponse(socketHandle, 200, "application/json", jsonOut);
                return true;
            }
            sendHttpResponse(socketHandle, 404, "application/json", "{\"ok\":false,\"error\":\"Not found\"}");
            return false;
        } else if (method == "POST") {
            auto payload = juce::JSON::parse(body);
            juce::String res = handleApiPost(path, payload);
            sendHttpResponse(socketHandle, 200, "application/json", res);
            return true;
        }

        sendHttpResponse(socketHandle, 405, "application/json", "{\"ok\":false,\"error\":\"Method not allowed\"}");
        return false;
    }

    if (method != "GET" && method != "HEAD") {
        sendHttpResponse(socketHandle, 405, "text/plain", "405 Method Not Allowed");
        return false;
    }

    // Issue SameSite=Strict HttpOnly session cookie when the initial HUD URL includes ?token=<sessionToken>
    juce::String extraHeader;
    const juce::String qToken = extractQueryParam(query, "token");
    if (qToken.isNotEmpty() && constantTimeEquals(qToken, sessionToken)) {
        extraHeader = "Set-Cookie: fanfare_session=" + sessionToken + "; HttpOnly; SameSite=Strict; Path=/\r\n";
    }

    // Static File Serving
    return serveStaticFile(socketHandle, path, extraHeader);
}

juce::String WebServer::handleApiPost(const juce::String& path, const juce::var& jsonPayload) {
    static const std::unordered_map<std::string, Dispatch::PostHandler> kPostHandlers = {
        { "/api/scenes/select", &Dispatch::postSelectScene },
        { "/api/panic",         &Dispatch::postPanic },
        { "/api/mp3/play",      &Dispatch::postMp3Play },
        { "/api/mp3/pause",     &Dispatch::postMp3Pause },
        { "/api/mp3/next",      &Dispatch::postMp3Next },
        { "/api/mp3/prev",      &Dispatch::postMp3Prev },
        { "/api/mp3/bank",      &Dispatch::postMp3Bank },
        { "/api/mp3/track",     &Dispatch::postMp3Track },
        { "/api/mp3/gain",      &Dispatch::postMp3Gain },
        { "/api/mp3/autodj",    &Dispatch::postMp3AutoDj },
        { "/api/mp3/download",  &Dispatch::postMp3Download },
    };

    const auto it = kPostHandlers.find(path.toStdString());
    if (it != kPostHandlers.end())
        return it->second(*this, jsonPayload);
    return "{\"ok\":false,\"error\":\"endpoint not found\"}";
}

juce::var WebServer::buildStatusJson() {
    juce::DynamicObject::Ptr obj = new juce::DynamicObject();
    obj->setProperty("ok", true);
    obj->setProperty("hostVersion", OPENRIG_VERSION_STRING);

    auto& sm = Fanfare::SetlistManager::getInstance();
    juce::String activeName = sm.getActiveFile().existsAsFile() ? sm.getActiveFile().getFileNameWithoutExtension() : "DEFAULT SONG";
    obj->setProperty("activePreset", activeName);

    int activeSceneIdx = 0;
    const auto sceneNames = engine.getSceneNamesSnapshot(activeSceneIdx);
    obj->setProperty("activeScene", activeSceneIdx);
    obj->setProperty("numScenes", (int)sceneNames.size());

    juce::Array<juce::var> sceneArr;
    for (size_t i = 0; i < sceneNames.size(); ++i) {
        juce::DynamicObject::Ptr scObj = new juce::DynamicObject();
        scObj->setProperty("index", (int)i);
        scObj->setProperty("name", sceneNames[i]);
        sceneArr.add(juce::var(scObj.get()));
    }
    obj->setProperty("scenes", sceneArr);

    auto setupsSnap = sm.getSetupsSnapshot();
    juce::Array<juce::var> setlistArr;
    for (const auto& f : setupsSnap) {
        setlistArr.add(f.getFileNameWithoutExtension());
    }
    obj->setProperty("setlist", setlistArr);
    obj->setProperty("activeSetlistIndex", sm.getActiveIndex());
    obj->setProperty("isPreloaded", sm.isPreloaded());
    obj->setProperty("isPreloading", sm.isPreloading());
    obj->setProperty("preloadName", sm.getPreloadSetupName());

    int preloadIdx = -1;
    juce::File nextFile = sm.getNextPreloadTarget();
    for (int i = 0; i < setupsSnap.size(); ++i) {
        if (setupsSnap[i] == nextFile) {
            preloadIdx = i;
            break;
        }
    }
    obj->setProperty("preloadIndex", preloadIdx);

    // Slots array for the active song/setup
    juce::Array<juce::var> slotsArr;
    for (int i = 0; i < engine.getNumSlots(); ++i) {
        auto* slot = engine.getSlot(i);
        juce::DynamicObject::Ptr slObj = new juce::DynamicObject();
        slObj->setProperty("index", i);
        slObj->setProperty("name", slot->getName());
        slObj->setProperty("fohLevel", slot->getFohLevel());
        slObj->setProperty("iemLevel", juce::jlimit(0.0f, 1.0f, slot->getIemOffset() * 0.5f));
        slObj->setProperty("iemOffset", slot->getIemOffset());
        slObj->setProperty("fohEnabled", slot->isFohEnabled());
        slObj->setProperty("iemEnabled", slot->isIemEnabled());
        slObj->setProperty("bypassed", slot->isBypassed());
        slObj->setProperty("lowNote", slot->getLowNote());
        slObj->setProperty("highNote", slot->getHighNote());
        slotsArr.add(juce::var(slObj.get()));
    }
    obj->setProperty("slots", slotsArr);

    // Quick notes preview for active song
    auto notes = engine.getNotes();
    if (!notes.empty()) {
        obj->setProperty("quickNotes", notes[0].content);
        obj->setProperty("quickNotesTitle", notes[0].title);
    } else {
        obj->setProperty("quickNotes", "");
        obj->setProperty("quickNotesTitle", "Notes");
    }

    obj->setProperty("cpuLoad", webHost ? webHost->getCpuUsage() : 0.0);
    obj->setProperty("ramMb", (int)(FanfareLog::getMemoryStats().workingSetBytes / (1024 * 1024)));
    obj->setProperty("underruns", webHost ? webHost->getAudioUnderruns() : 0);
    obj->setProperty("thermalAlert", webHost ? webHost->isThermalWarningActive() : false);
    obj->setProperty("cpuTempC", webHost ? webHost->getCpuTemperatureCelsius() : -1);
    obj->setProperty("thermalText", webHost ? webHost->getThermalBadgeText() : juce::String("THERMAL"));
    obj->setProperty("librarySongs", buildSongsLibraryJson());

    return juce::var(obj.get());
}

juce::var WebServer::buildScenesJson() {
    return buildStatusJson()["scenes"];
}

juce::var WebServer::buildSongsLibraryJson() {
    juce::Array<juce::var> songsArr;
    auto appData = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory);

    const std::vector<juce::File> searchDirs = getAllowedSongDirectories();

    // Read favorites from both Fanfare and OpenRig
    std::set<juce::String> favorites;
    for (const auto& appName : { "Fanfare", "OpenRig" }) {
        auto favFile = appData.getChildFile(appName).getChildFile("library_favorites.txt");
        if (favFile.existsAsFile()) {
            juce::StringArray lines;
            favFile.readLines(lines);
            for (const auto& l : lines) {
                if (l.trim().isNotEmpty()) {
                    favorites.insert(l.trim());
                    favorites.insert(juce::File(l.trim()).getFileNameWithoutExtension());
                }
            }
        }
    }

    std::set<juce::String> seenNames;
    juce::Array<juce::File> found;

    for (const auto& songsDir : searchDirs) {
        if (!songsDir.isDirectory()) continue;
        juce::Array<juce::File> dirFound;
        songsDir.findChildFiles(dirFound, juce::File::findFiles, true, "*.json");
        juce::Array<juce::File> dirFoundOrsong;
        songsDir.findChildFiles(dirFoundOrsong, juce::File::findFiles, true, "*.orsong");
        for (const auto& f : dirFoundOrsong) dirFound.addIfNotAlreadyThere(f);

        for (const auto& f : dirFound) {
            juce::String fn = f.getFileNameWithoutExtension().toLowerCase();
            if (seenNames.count(fn) == 0) {
                seenNames.insert(fn);
                found.add(f);
            }
        }
    }

    std::sort(found.begin(), found.end(), [](const juce::File& a, const juce::File& b) {
        return a.getFileName().compareIgnoreCase(b.getFileName()) < 0;
    });

    for (const auto& f : found) {
        juce::DynamicObject::Ptr sObj = new juce::DynamicObject();
        juce::String fileName = f.getFileNameWithoutExtension();
        juce::String fullPath = f.getFullPathName();

        juce::String catName = "GENERAL";
        auto parent = f.getParentDirectory();
        if (parent.getFileName().equalsIgnoreCase("Core Setups")) {
            catName = "CORE SETUPS";
        } else if (!parent.getFileName().equalsIgnoreCase("songs")) {
            catName = parent.getFileName().toUpperCase();
        }

        bool isFav = (favorites.count(fullPath) > 0 || favorites.count(fileName) > 0);

        juce::Array<juce::var> tagsArr;
        juce::String lowerName = fileName.toLowerCase();

        if (isFav) tagsArr.add("FAVORITES");
        if (catName != "GENERAL") tagsArr.add(catName);

        if (lowerName.contains("piano") || lowerName.contains("keys") || lowerName.contains("grand") || lowerName.contains("giant") || lowerName.contains("gentleman") || lowerName.contains("maverick") || lowerName.contains("noire") || lowerName.contains("wurli") || lowerName.contains("rhodes") || lowerName.contains("pianoverse"))
            tagsArr.add("PIANO");

        if (lowerName.contains("b3") || lowerName.contains("organ") || lowerName.contains("blue3") || lowerName.contains("leslie"))
            tagsArr.add("B3 ORGAN");

        if (lowerName.contains("pad") || lowerName.contains("synth") || lowerName.contains("lead") || lowerName.contains("zenology") || lowerName.contains("juno") || lowerName.contains("5080") || lowerName.contains("wolf") || lowerName.contains("beds") || lowerName.contains("rebell"))
            tagsArr.add("SYNTH / PAD");

        if (lowerName.contains("brass") || lowerName.contains("horn") || lowerName.contains("sax") || lowerName.contains("trumpet") || lowerName.contains("centerfold"))
            tagsArr.add("BRASS / HORNS");

        if (lowerName.contains("solo"))
            tagsArr.add("SOLO");

        if (lowerName.contains("cello") || lowerName.contains("string") || lowerName.contains("violin"))
            tagsArr.add("STRINGS");

        if (lowerName.contains("snap") || lowerName.contains("clap") || lowerName.contains("drum") || lowerName.contains("perc") || lowerName.contains("marimba"))
            tagsArr.add("PERCUSSION");

        sObj->setProperty("name", fileName);
        sObj->setProperty("category", catName);
        sObj->setProperty("isFavorite", isFav);
        sObj->setProperty("tags", tagsArr);
        sObj->setProperty("path", fullPath);
        sObj->setProperty("fullPath", fullPath);
        songsArr.add(juce::var(sObj.get()));
    }

    return juce::var(songsArr);
}

juce::var WebServer::buildSetlistJson() {
    juce::DynamicObject::Ptr obj = new juce::DynamicObject();
    auto& sm = Fanfare::SetlistManager::getInstance();
    obj->setProperty("activeIndex", sm.getActiveIndex());
    juce::Array<juce::var> setups;
    for (const auto& f : sm.getSetupsSnapshot()) setups.add(f.getFileNameWithoutExtension());
    obj->setProperty("setups", setups);
    obj->setProperty("isPreloaded", sm.isPreloaded());
    obj->setProperty("preloadName", sm.getPreloadSetupName());
    return juce::var(obj.get());
}

juce::var WebServer::buildNotepadJson() {
    juce::DynamicObject::Ptr obj = new juce::DynamicObject();
    juce::Array<juce::var> tabsArr;
    for (const auto& t : engine.getNotes()) {
        juce::DynamicObject::Ptr tabObj = new juce::DynamicObject();
        tabObj->setProperty("title", t.title);
        tabObj->setProperty("content", t.content);
        tabsArr.add(juce::var(tabObj.get()));
    }
    obj->setProperty("tabs", tabsArr);
    obj->setProperty("activeTabIndex", engine.getActiveNoteTabIndex());
    obj->setProperty("fontSize", engine.getNoteFontSize());
    obj->setProperty("isMonospace", engine.getNoteIsMonospace());
    return juce::var(obj.get());
}

juce::var WebServer::buildMixerJson() {
    juce::DynamicObject::Ptr obj = new juce::DynamicObject();
    obj->setProperty("masterFoh", engine.getFohMasterLevel());
    obj->setProperty("masterIem", engine.getIemMasterLevel());

    juce::Array<juce::var> slotsArr;
    for (int i = 0; i < engine.getNumSlots(); ++i) {
        auto* slot = engine.getSlot(i);
        juce::DynamicObject::Ptr slObj = new juce::DynamicObject();
        slObj->setProperty("index", i);
        slObj->setProperty("name", slot->getName());
        slObj->setProperty("fohLevel", slot->getFohLevel());
        slObj->setProperty("iemLevel", juce::jlimit(0.0f, 1.0f, slot->getIemOffset() * 0.5f));
        slObj->setProperty("iemOffset", slot->getIemOffset());
        slObj->setProperty("fohEnabled", slot->isFohEnabled());
        slObj->setProperty("iemEnabled", slot->isIemEnabled());
        slObj->setProperty("bypassed", slot->isBypassed());
        slObj->setProperty("lowNote", slot->getLowNote());
        slObj->setProperty("highNote", slot->getHighNote());
        slotsArr.add(juce::var(slObj.get()));
    }
    obj->setProperty("slots", slotsArr);
    return juce::var(obj.get());
}

juce::var WebServer::buildMp3Json() {
    juce::DynamicObject::Ptr obj = new juce::DynamicObject();
    auto* mp3 = engine.getMp3Player();
    if (!mp3) {
        obj->setProperty("available", false);
        return juce::var(obj.get());
    }

    obj->setProperty("available", true);
    obj->setProperty("isPlaying", mp3->isPlaying());
    obj->setProperty("gain", (double)mp3->getGain());
    obj->setProperty("position", mp3->getPosition());
    obj->setProperty("length", mp3->getLength());
    obj->setProperty("activeDeck", mp3->getActiveDeckIndex());
    obj->setProperty("isCrossfading", mp3->getIsCrossfading());
    obj->setProperty("autoDj", mp3->isAutoDjEnabled());
    obj->setProperty("crossfadeSec", (double)mp3->getCrossfadeDuration());
    obj->setProperty("activeBank", mp3->getActiveBankIndex());
    obj->setProperty("currentTrackIndex", mp3->getCurrentTrackIndex());

    int curIdx = mp3->getCurrentTrackIndex();
    const auto playlist = mp3->getPlaylistSnapshot();
    if (curIdx >= 0 && curIdx < (int)playlist.size()) {
        obj->setProperty("currentTitle", playlist[curIdx].title);
    } else {
        obj->setProperty("currentTitle", "No track loaded");
    }

    juce::Array<juce::var> banksArr;
    for (const auto& b : mp3->getBanksSnapshot()) {
        juce::DynamicObject::Ptr bo = new juce::DynamicObject();
        bo->setProperty("index", b.index);
        bo->setProperty("name", b.name);
        bo->setProperty("numTracks", (int)b.tracks.size());
        banksArr.add(juce::var(bo.get()));
    }
    obj->setProperty("banks", banksArr);

    juce::Array<juce::var> tracksArr;
    for (int i = 0; i < (int)playlist.size(); ++i) {
        juce::DynamicObject::Ptr to = new juce::DynamicObject();
        to->setProperty("index", i);
        to->setProperty("title", playlist[i].title);
        to->setProperty("duration", playlist[i].durationSeconds);
        tracksArr.add(juce::var(to.get()));
    }
    obj->setProperty("tracks", tracksArr);

    return juce::var(obj.get());
}

bool WebServer::serveStaticFile(void* socketHandle, const juce::String& relativePath, const juce::String& extraHeader) {
    juce::String cleanPath = juce::URL::removeEscapeChars(relativePath.trim());
    if (cleanPath.containsChar('?')) cleanPath = cleanPath.upToFirstOccurrenceOf("?", false, false);
    if (cleanPath.containsChar('#')) cleanPath = cleanPath.upToFirstOccurrenceOf("#", false, false);
    if (cleanPath == "/" || cleanPath.isEmpty()) {
        cleanPath = "/index.html";
    }

    juce::String staticExtraHeader = extraHeader;
    if (!staticExtraHeader.containsIgnoreCase("Cache-Control:")) {
        if (cleanPath.startsWithIgnoreCase("/assets-readonly/")
            || cleanPath.equalsIgnoreCase("/Fanfare_Icon.png")) {
            staticExtraHeader << "Cache-Control: public, max-age=86400\r\n";
        } else if (cleanPath == "/style.css" || cleanPath == "/app.js"
                   || cleanPath.equalsIgnoreCase("/manual.html")) {
            staticExtraHeader << "Cache-Control: public, max-age=60\r\n";
        }
    }

    // Strict traversal & absolute-path rejection (C1 + M6 + CRLF guard)
    if (cleanPath.contains("..")
        || cleanPath.containsChar(':')
        || cleanPath.containsChar('\\')
        || cleanPath.containsChar('\0')
        || cleanPath.containsChar('\r')
        || cleanPath.containsChar('\n')
        || cleanPath.startsWith("//")
        || !cleanPath.startsWithChar('/')) {
        sendHttpResponse(socketHandle, 404, "text/plain", "404 Not Found");
        return false;
    }

    const juce::String relFile = cleanPath.substring(1);
    if (relFile.isEmpty()
        || relFile.startsWithChar('/')
        || relFile.startsWithChar('\\')
        || juce::File::isAbsolutePath(relFile)
        || !isAllowedStaticExtension(cleanPath)) {
        sendHttpResponse(socketHandle, 404, "text/plain", "404 Not Found");
        return false;
    }

    auto exeDir = juce::File::getSpecialLocation(juce::File::currentExecutableFile).getParentDirectory();
    auto cwdDir = juce::File::getCurrentWorkingDirectory();

    auto tryServeFromDirs = [&](const juce::Array<juce::File>& dirs) -> bool {
        for (const auto& dir : dirs) {
            if (!dir.isDirectory())
                continue;
            juce::File targetFile = dir.getChildFile(relFile);
            if (!targetFile.isSymbolicLink()
                && targetFile.existsAsFile()
                && targetFile.isAChildOf(dir)) {
                juce::MemoryBlock block;
                if (targetFile.loadFileAsData(block)) {
                    sendHttpDataResponse(socketHandle, 200, getMimeType(cleanPath), block.getData(), block.getSize(), staticExtraHeader);
                    return true;
                }
            }
        }
        return false;
    };

    // 1. HUD Static Asset Directories FIRST (packaged Web/static or relative workspace Source/Web/static)
    // Skip manual.html and assets-readonly/* here so the Offline Stage Manual resolves from docsDirs!
    const bool isManualAsset = cleanPath.equalsIgnoreCase("/manual.html") ||
                               cleanPath.startsWithIgnoreCase("/assets-readonly/");
    if (!isManualAsset) {
        juce::Array<juce::File> hudStaticDirs;
        hudStaticDirs.add(exeDir.getChildFile("Web/static"));
        hudStaticDirs.add(exeDir.getChildFile("static"));
        hudStaticDirs.add(exeDir.getChildFile("Source/Web/static"));
#if JUCE_DEBUG
        hudStaticDirs.add(cwdDir.getChildFile("Source/Web/static"));
#endif

        if (tryServeFromDirs(hudStaticDirs))
            return true;
    }

    // 2. Compiled-in HUD Production Fallback (never shadowed by docs/index.html!)
    if (cleanPath == "/index.html") {
        sendHttpDataResponse(socketHandle, 200, "text/html; charset=utf-8", EmbeddedWebAssets::INDEX_HTML_DATA, EmbeddedWebAssets::INDEX_HTML_DATA_LEN, staticExtraHeader);
        return true;
    } else if (cleanPath == "/style.css") {
        sendHttpDataResponse(socketHandle, 200, "text/css; charset=utf-8", EmbeddedWebAssets::STYLE_CSS_DATA, EmbeddedWebAssets::STYLE_CSS_DATA_LEN, staticExtraHeader);
        return true;
    } else if (cleanPath == "/app.js") {
        sendHttpDataResponse(socketHandle, 200, "application/javascript; charset=utf-8", EmbeddedWebAssets::APP_JS_DATA, EmbeddedWebAssets::APP_JS_DATA_LEN, staticExtraHeader);
        return true;
    } else if (cleanPath == "/plugin_mirror.html") {
        sendHttpDataResponse(socketHandle, 200, "text/html; charset=utf-8", EmbeddedWebAssets::PLUGIN_MIRROR_HTML_DATA, EmbeddedWebAssets::PLUGIN_MIRROR_HTML_DATA_LEN, staticExtraHeader);
        return true;
    }

    // 3. Offline Stage Manual & Supporting Assets (/manual.html, /assets-readonly/*, screenshots inside docs/)
    juce::Array<juce::File> docsDirs;
    docsDirs.add(exeDir.getChildFile("docs"));
#if JUCE_DEBUG
    docsDirs.add(cwdDir.getChildFile("docs"));
#endif
    docsDirs.add(FanfareConstants::getAppDirectory().getChildFile("docs"));

    if (tryServeFromDirs(docsDirs))
        return true;

    // Also allow manual.html inside packaged Web/static or right beside Fanfare.exe (exact filename only)
    if (cleanPath.equalsIgnoreCase("/manual.html")) {
        juce::Array<juce::File> fallbackManualDirs;
        fallbackManualDirs.add(exeDir.getChildFile("Web/static"));
        fallbackManualDirs.add(exeDir.getChildFile("static"));
        if (tryServeFromDirs(fallbackManualDirs))
            return true;

        juce::File directManual = exeDir.getChildFile("manual.html");
        if (!directManual.isSymbolicLink() && directManual.existsAsFile()) {
            juce::MemoryBlock block;
            if (directManual.loadFileAsData(block)) {
                sendHttpDataResponse(socketHandle, 200, "text/html; charset=utf-8", block.getData(), block.getSize(), staticExtraHeader);
                return true;
            }
        }
    }

    // 4. Compiled-in Offline Stage Manual Fallback (works from standalone Fanfare.exe with no docs/ folder!)
    for (const auto& asset : kEmbeddedManualAssets) {
        if (cleanPath.equalsIgnoreCase(asset.path)) {
            sendHttpDataResponse(socketHandle, 200, asset.mime, asset.data, asset.len, staticExtraHeader);
            return true;
        }
    }

    // 5. For standalone Fanfare.exe without local docs/, redirect manual screenshots/webfonts
    // to GitHub Pages when online (fails gracefully via alt text when offline on stage).
    if (cleanPath.endsWithIgnoreCase(".png")
        || cleanPath.endsWithIgnoreCase(".jpg")
        || cleanPath.endsWithIgnoreCase(".jpeg")
        || cleanPath.startsWithIgnoreCase("/assets-readonly/webfonts/")) {
        const juce::String redirectHdr =
            extraHeader + "Location: https://openrig-host.github.io/fanfare" + cleanPath + "\r\n";
        sendHttpResponse(socketHandle, 302, "text/plain", "", redirectHdr);
        return true;
    }

    sendHttpResponse(socketHandle, 404, "text/plain", "404 Not Found");
    return false;
}

juce::String WebServer::getMimeType(const juce::String& filePath) {
    if (filePath.endsWithIgnoreCase(".html")) return "text/html; charset=utf-8";
    if (filePath.endsWithIgnoreCase(".css")) return "text/css; charset=utf-8";
    if (filePath.endsWithIgnoreCase(".js")) return "application/javascript; charset=utf-8";
    if (filePath.endsWithIgnoreCase(".png")) return "image/png";
    if (filePath.endsWithIgnoreCase(".svg")) return "image/svg+xml";
    if (filePath.endsWithIgnoreCase(".ico")) return "image/x-icon";
    if (filePath.endsWithIgnoreCase(".woff2")) return "font/woff2";
    if (filePath.endsWithIgnoreCase(".woff")) return "font/woff";
    if (filePath.endsWithIgnoreCase(".ttf")) return "font/ttf";
    if (filePath.endsWithIgnoreCase(".jpg") || filePath.endsWithIgnoreCase(".jpeg")) return "image/jpeg";
    return "application/octet-stream";
}

void WebServer::sendHttpResponse(void* socketHandle, int statusCode, const juce::String& contentType, const juce::String& body, const juce::String& extraHeader) {
    sendHttpDataResponse(socketHandle, statusCode, contentType, body.toRawUTF8(), (size_t)body.getNumBytesAsUTF8(), extraHeader);
}

void WebServer::sendHttpDataResponse(void* socketHandle, int statusCode, const juce::String& contentType, const void* data, size_t dataSize, const juce::String& extraHeader) {
    SocketType sock = (SocketType)(uintptr_t)socketHandle;
    const char* statusText = "Error";
    switch (statusCode) {
        case 200: statusText = "OK"; break;
        case 204: statusText = "No Content"; break;
        case 302: statusText = "Found"; break;
        case 400: statusText = "Bad Request"; break;
        case 401: statusText = "Unauthorized"; break;
        case 403: statusText = "Forbidden"; break;
        case 404: statusText = "Not Found"; break;
        case 405: statusText = "Method Not Allowed"; break;
        case 413: statusText = "Payload Too Large"; break;
        case 431: statusText = "Request Header Fields Too Large"; break;
        case 503: statusText = "Service Unavailable"; break;
        default: break;
    }

    std::ostringstream ss;
    ss << "HTTP/1.1 " << statusCode << " " << statusText << "\r\n";
    ss << "Content-Type: " << contentType.toStdString() << "\r\n";
    ss << "Content-Length: " << dataSize << "\r\n";
    if (!extraHeader.containsIgnoreCase("Cache-Control:"))
        ss << "Cache-Control: no-store, no-cache, must-revalidate\r\n";
    ss << "X-Content-Type-Options: nosniff\r\n";
    if (extraHeader.isNotEmpty())
        ss << extraHeader.toStdString();
    ss << "Connection: close\r\n\r\n";

    std::string headerStr = ss.str();
    if (!sendAll(sock, headerStr.data(), headerStr.size()))
        return;
    if (dataSize > 0 && data != nullptr) {
        sendAll(sock, data, dataSize);
    }
}

juce::String WebServer::getEmbeddedHtml() {
    return juce::String::createStringFromData(EmbeddedWebAssets::INDEX_HTML_DATA, (int)EmbeddedWebAssets::INDEX_HTML_DATA_LEN);
}

juce::String WebServer::getEmbeddedCss() {
    return juce::String::createStringFromData(EmbeddedWebAssets::STYLE_CSS_DATA, (int)EmbeddedWebAssets::STYLE_CSS_DATA_LEN);
}

juce::String WebServer::getEmbeddedJs() {
    return juce::String::createStringFromData(EmbeddedWebAssets::APP_JS_DATA, (int)EmbeddedWebAssets::APP_JS_DATA_LEN);
}

} // namespace OpenRig

namespace Fanfare {

juce::File ensureOfflineManualExtracted() {
    const juce::File exeDir =
        juce::File::getSpecialLocation(juce::File::currentExecutableFile).getParentDirectory();
    const juce::File docsManual = exeDir.getChildFile("docs").getChildFile("manual.html");
    if (docsManual.existsAsFile())
        return docsManual;

    const juce::File cacheDir =
        FanfareConstants::getAppDirectory().getChildFile("manual_cache").getChildFile(OPENRIG_VERSION_STRING);
    if (!cacheDir.exists())
        cacheDir.createDirectory();

    for (const auto& asset : kEmbeddedManualAssets) {
        juce::String rel(asset.path);
        while (rel.startsWithChar('/'))
            rel = rel.substring(1);
        juce::File target = cacheDir.getChildFile(rel);
        target.getParentDirectory().createDirectory();
        if (!target.existsAsFile() || target.getSize() != (juce::int64)asset.len)
            target.replaceWithData(asset.data, asset.len);
    }

    return cacheDir.getChildFile("manual.html");
}

} // namespace Fanfare
