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
#include "../FanfareEngine.h"
#include "../MainComponent.h"
#include "../SetlistManager.h"
#include "../YoutubeDownloadManager.h"
#include <sstream>
#include <random>
#include <cmath>
#include <cctype>
#include <cstring>

#if JUCE_WINDOWS
  typedef SOCKET SocketType;
  #define INVALID_SOCKET_VAL INVALID_SOCKET
  #define SOCKET_CLOSE(s) closesocket(s)
  #define SOCKET_SHUTDOWN_BOTH(s) ::shutdown(s, SD_BOTH)
#else
  #include <sys/types.h>
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <unistd.h>
  #include <fcntl.h>
  #include <errno.h>
  typedef int SocketType;
  #define INVALID_SOCKET_VAL -1
  #define SOCKET_CLOSE(s) close(s)
  #define SOCKET_SHUTDOWN_BOTH(s) ::shutdown(s, SHUT_RDWR)
#endif

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

WebServer::WebServer(FanfareEngine& eng, MainComponent* mc)
    : juce::Thread("OpenRigWebServerThread"),
      engine(eng),
      mainComponent(mc),
      sessionToken(generateSessionTokenHex()) {
#if JUCE_WINDOWS
    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);
#endif
}

WebServer::~WebServer() {
    stopServer();
#if JUCE_WINDOWS
    WSACleanup();
#endif
}

bool WebServer::startServer(int port) {
    if (serverRunning.load())
        stopServer();

    lifetime = std::make_shared<LifetimeState>();
    if (sessionToken.isEmpty())
        sessionToken = generateSessionTokenHex();

    serverPort = port;

    SocketType sock = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCKET_VAL) {
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
    serverAddr.sin_addr.s_addr = INADDR_ANY;
    serverAddr.sin_port = htons((u_short)port);

    if (::bind(sock, (struct sockaddr*)&serverAddr, sizeof(serverAddr)) < 0) {
        SOCKET_CLOSE(sock);
        return false;
    }

    if (::listen(sock, 16) < 0) {
        SOCKET_CLOSE(sock);
        return false;
    }

    listenSocket = (void*)(uintptr_t)sock;
    serverRunning.store(true, std::memory_order_release);

    startThread(juce::Thread::Priority::normal);
    startTimerHz(30); // 30fps real-time VU and telemetry stream

    return true;
}

void WebServer::stopServer() {
    if (lifetime)
        lifetime->alive.store(false, std::memory_order_release);
    serverRunning.store(false, std::memory_order_release);
    stopTimer();

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
            setSocketTimeoutsMs(sock, 2000, 5000);
            {
                std::lock_guard<std::mutex> lock(clientSocketsMutex);
                activeHttpSockets.erase(socketHandle);
                activeWsClients.insert(socketHandle);
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
        // Send initial status packet upon connect
        juce::DynamicObject::Ptr root = new juce::DynamicObject();
        root->setProperty("type", "status");
        root->setProperty("data", buildStatusJson());
        juce::String initMsg = juce::JSON::toString(juce::var(root.get()));
        broadcastWebSocket(initMsg);

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
                uint8_t pongFrame[127];
                pongFrame[0] = 0x8A;
                pongFrame[1] = static_cast<uint8_t>(pongLen);
                if (pongLen > 0)
                    std::memcpy(pongFrame + 2, payload.data(), pongLen);
                if (!sendAll(sock, pongFrame, 2 + pongLen))
                    break;
                continue;
            } else if (opcode == 0x1) { // Text frame
                juce::String textMsg = juce::String::fromUTF8((const char*)payload.data(), (int)payload.size());
                processIncomingWsMessage(textMsg);
            }
        }
    } catch (...) {
        // Never allow an unhandled exception to terminate a WS worker thread
    }
}

void WebServer::broadcastWebSocket(const juce::String& jsonMessage) {
    std::lock_guard<std::mutex> lock(clientSocketsMutex);
    if (activeWsClients.empty()) return;

    size_t payloadLen = (size_t)jsonMessage.getNumBytesAsUTF8();
    std::vector<uint8_t> frame;
    frame.reserve(payloadLen + 10);

    frame.push_back(0x81); // FIN + Text opcode

    if (payloadLen <= 125) {
        frame.push_back((uint8_t)payloadLen);
    } else if (payloadLen <= 65535) {
        frame.push_back(126);
        frame.push_back((uint8_t)((payloadLen >> 8) & 0xFF));
        frame.push_back((uint8_t)(payloadLen & 0xFF));
    } else {
        frame.push_back(127);
        for (int i = 7; i >= 0; --i)
            frame.push_back((uint8_t)((payloadLen >> (i * 8)) & 0xFF));
    }

    const char* rawUtf8 = jsonMessage.toRawUTF8();
    frame.insert(frame.end(), rawUtf8, rawUtf8 + payloadLen);

    for (void* client : activeWsClients) {
        SocketType cs = (SocketType)(uintptr_t)client;
        sendAll(cs, frame.data(), frame.size());
    }
}

void WebServer::processIncomingWsMessage(const juce::String& messageText) {
    auto json = juce::JSON::parse(messageText);
    if (!json.isObject()) return;

    juce::String type = json["type"].toString();
    auto life = lifetime;

    if (type == "select_scene") {
        int idx = 0;
        if (!tryJsonInt(json["index"], idx, 0, 1024))
            return;
        juce::MessageManager::callAsync([this, life, idx]() {
            if (!life || !life->alive.load(std::memory_order_acquire))
                return;
            if (idx < 0 || idx >= engine.getNumScenes())
                return;
            engine.saveCurrentStateToScene(engine.getCurrentSceneIndex());
            engine.loadScene(idx);
            notifySceneChanged(idx, engine.getSceneName(idx));
            if (mainComponent != nullptr)
                mainComponent->updateStripCollapseStates();
        });
    } else if (type == "prev_setlist") {
        juce::MessageManager::callAsync([this, life]() {
            if (!life || !life->alive.load(std::memory_order_acquire))
                return;
            auto& sm = Fanfare::SetlistManager::getInstance();
            if (sm.hasPrev() && mainComponent != nullptr) {
                int prevIdx = sm.getActiveIndex() - 1;
                auto setups = sm.getSetupsSnapshot();
                if (prevIdx >= 0 && prevIdx < setups.size())
                    mainComponent->loadRigFromFile(setups[prevIdx], prevIdx);
            }
        });
    } else if (type == "next_setlist") {
        juce::MessageManager::callAsync([this, life]() {
            if (!life || !life->alive.load(std::memory_order_acquire))
                return;
            auto& sm = Fanfare::SetlistManager::getInstance();
            if (sm.hasNext() && mainComponent != nullptr) {
                int nextIdx = sm.getActiveIndex() + 1;
                auto setups = sm.getSetupsSnapshot();
                if (nextIdx >= 0 && nextIdx < setups.size())
                    mainComponent->loadRigFromFile(setups[nextIdx], nextIdx);
            }
        });
    } else if (type == "select_setlist") {
        int targetIdx = 0;
        if (!tryJsonInt(json["index"], targetIdx, 0, 10000))
            return;
        juce::MessageManager::callAsync([this, life, targetIdx]() {
            if (!life || !life->alive.load(std::memory_order_acquire))
                return;
            auto& sm = Fanfare::SetlistManager::getInstance();
            auto setups = sm.getSetupsSnapshot();
            if (targetIdx >= 0 && targetIdx < setups.size() && mainComponent != nullptr)
                mainComponent->loadRigFromFile(setups[targetIdx], targetIdx);
        });
    } else if (type == "load_song") {
        juce::String pathOrName = json["path"].toString();
        juce::MessageManager::callAsync([this, life, pathOrName]() {
            if (!life || !life->alive.load(std::memory_order_acquire) || mainComponent == nullptr)
                return;
            juce::File f = resolveAllowedSongFile(pathOrName);
            if (f.existsAsFile())
                mainComponent->loadRigFromFile(f);
        });
    } else if (type == "queue_song") {
        juce::String pathOrName = json["path"].toString();
        juce::MessageManager::callAsync([life, pathOrName]() {
            if (!life || !life->alive.load(std::memory_order_acquire))
                return;
            juce::File f = resolveAllowedSongFile(pathOrName);
            if (f.existsAsFile()) {
                auto& sm = Fanfare::SetlistManager::getInstance();
                sm.addSetup(f);
            }
        });
    } else if (type == "toggle_favorite") {
        juce::String pathOrName = json["path"].toString();
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
    } else if (type == "panic") {
        engine.triggerPanic();
    } else if (type == "set_fader") {
        juce::String fader = json["fader"].toString();
        float val = 0.0f;
        if (!tryJsonFloat(json["value"], val, 0.0f, 1.0f))
            return;
        if (fader == "masterFoh") engine.setFohMasterLevel(val);
        else if (fader == "masterIem") engine.setIemMasterLevel(val);
    } else if (type == "set_slot_fader") {
        int slotIdx = 0;
        float val = 0.0f;
        if (!tryJsonInt(json["slot"], slotIdx, 0, engine.getNumSlots() - 1))
            return;
        if (!tryJsonFloat(json["value"], val, 0.0f, 1.0f))
            return;
        juce::String ch = json["channel"].toString();
        if (auto* slot = engine.getSlot(slotIdx)) {
            if (ch == "foh") slot->setFohLevel(val);
            else if (ch == "iem") slot->setIemOffset(val * 2.0f);
        }
    } else if (type == "set_slot_mute") {
        int slotIdx = 0;
        if (!tryJsonInt(json["slot"], slotIdx, 0, engine.getNumSlots() - 1))
            return;
        juce::String ch = json["channel"].toString();
        bool enabled = static_cast<bool>(json["enabled"]);
        if (auto* slot = engine.getSlot(slotIdx)) {
            if (ch == "foh") slot->setFohEnabled(enabled);
            else if (ch == "iem") slot->setIemEnabled(enabled);
        }
    } else if (type == "set_notepad_tab") {
        int idx = 0;
        if (!tryJsonInt(json["index"], idx, 0, 15))
            return;
        const int maxTab = std::max(0, (int)engine.getNotes().size() - 1);
        engine.setActiveNoteTabIndex(juce::jlimit(0, maxTab, idx));
        engine.broadcastNotesChanged();
    } else if (type == "set_notepad_font_size") {
        float sz = 20.0f;
        if (!tryJsonFloat(json["size"], sz, 10.0f, 72.0f))
            return;
        engine.setNoteFontSize(sz);
        engine.broadcastNotesChanged();
    } else if (type == "set_notepad_monospace") {
        bool mono = static_cast<bool>(json["isMonospace"]);
        engine.setNoteIsMonospace(mono);
        engine.broadcastNotesChanged();
    } else if (type == "update_notepad_content") {
        int idx = 0;
        if (!tryJsonInt(json["index"], idx, 0, 15))
            return;
        juce::String content = json["content"].toString();
        auto notes = engine.getNotes();
        if (idx >= 0 && idx < (int)notes.size()) {
            notes[idx].content = content;
            engine.setNotes(notes);
            engine.broadcastNotesChanged();
        }
    } else if (type == "get_status") {
        juce::DynamicObject::Ptr root = new juce::DynamicObject();
        root->setProperty("type", "status");
        root->setProperty("data", buildStatusJson());
        broadcastWebSocket(juce::JSON::toString(juce::var(root.get())));
    } else if (type == "get_notepad") {
        juce::DynamicObject::Ptr root = new juce::DynamicObject();
        root->setProperty("type", "notepad");
        root->setProperty("data", buildNotepadJson());
        broadcastWebSocket(juce::JSON::toString(juce::var(root.get())));
    } else if (type == "get_mixer") {
        juce::DynamicObject::Ptr root = new juce::DynamicObject();
        root->setProperty("type", "mixer");
        root->setProperty("data", buildMixerJson());
        broadcastWebSocket(juce::JSON::toString(juce::var(root.get())));
    } else if (type == "ping") {
        juce::DynamicObject::Ptr root = new juce::DynamicObject();
        root->setProperty("type", "pong");
        root->setProperty("time", json["time"]);
        if (json.hasProperty("cid")) root->setProperty("cid", json["cid"]);
        broadcastWebSocket(juce::JSON::toString(juce::var(root.get())));
    }
}

void WebServer::timerCallback() {
    // 30Hz high-frequency VU and telemetry broadcast (calibrated logarithmic dB scale)
    juce::DynamicObject::Ptr root = new juce::DynamicObject();
    root->setProperty("type", "meter");
    root->setProperty("fohL", FanfareLog::amplitudeToLogScale(engine.getFohPeakL()));
    root->setProperty("fohR", FanfareLog::amplitudeToLogScale(engine.getFohPeakR()));
    root->setProperty("iemL", FanfareLog::amplitudeToLogScale(engine.getIemPeakL()));
    root->setProperty("iemR", FanfareLog::amplitudeToLogScale(engine.getIemPeakR()));
    root->setProperty("cpu", mainComponent ? (float)mainComponent->getCpuUsage() : 0.0f);
    if (++telemetryThrottleCount >= 15) {
        telemetryThrottleCount = 0;
        root->setProperty("underruns", mainComponent ? mainComponent->getAudioUnderruns() : 0);
        const bool therm = mainComponent ? mainComponent->isThermalWarningActive() : false;
        root->setProperty("thermalAlert", therm);
        root->setProperty("cpuTempC", mainComponent ? mainComponent->getCpuTemperatureCelsius() : -1);
        if (therm && mainComponent != nullptr)
            root->setProperty("thermalText", mainComponent->getThermalBadgeText());
    }

    broadcastWebSocket(juce::JSON::toString(juce::var(root.get())));
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
            if (path == "/api/status") {
                sendHttpResponse(socketHandle, 200, "application/json", juce::JSON::toString(buildStatusJson()));
                return true;
            } else if (path == "/api/scenes") {
                sendHttpResponse(socketHandle, 200, "application/json", juce::JSON::toString(buildScenesJson()));
                return true;
            } else if (path == "/api/setlist") {
                sendHttpResponse(socketHandle, 200, "application/json", juce::JSON::toString(buildSetlistJson()));
                return true;
            } else if (path == "/api/notepad") {
                sendHttpResponse(socketHandle, 200, "application/json", juce::JSON::toString(buildNotepadJson()));
                return true;
            } else if (path == "/api/mixer") {
                sendHttpResponse(socketHandle, 200, "application/json", juce::JSON::toString(buildMixerJson()));
                return true;
            } else if (path == "/api/mp3") {
                sendHttpResponse(socketHandle, 200, "application/json", juce::JSON::toString(buildMp3Json()));
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
    auto life = lifetime;

    if (path == "/api/scenes/select") {
        int idx = 0;
        if (!jsonPayload.isObject() || !tryJsonInt(jsonPayload["index"], idx, 0, 1024))
            return "{\"ok\":false,\"error\":\"Invalid scene index\"}";
        juce::MessageManager::callAsync([this, life, idx]() {
            if (!life || !life->alive.load(std::memory_order_acquire))
                return;
            if (idx < 0 || idx >= engine.getNumScenes())
                return;
            engine.saveCurrentStateToScene(engine.getCurrentSceneIndex());
            engine.loadScene(idx);
            notifySceneChanged(idx, engine.getSceneName(idx));
            if (mainComponent != nullptr)
                mainComponent->updateStripCollapseStates();
        });
        return "{\"ok\":true}";
    } else if (path == "/api/panic") {
        engine.triggerPanic();
        return "{\"ok\":true}";
    } else if (path == "/api/mp3/play") {
        juce::MessageManager::callAsync([this, life]() {
            if (!life || !life->alive.load(std::memory_order_acquire) || !serverRunning.load()) return;
            if (auto* mp3 = engine.getMp3Player()) {
                mp3->play();
                notifyMp3Changed();
            }
        });
        return "{\"ok\":true}";
    } else if (path == "/api/mp3/pause") {
        juce::MessageManager::callAsync([this, life]() {
            if (!life || !life->alive.load(std::memory_order_acquire) || !serverRunning.load()) return;
            if (auto* mp3 = engine.getMp3Player()) {
                mp3->pause();
                notifyMp3Changed();
            }
        });
        return "{\"ok\":true}";
    } else if (path == "/api/mp3/next") {
        juce::MessageManager::callAsync([this, life]() {
            if (!life || !life->alive.load(std::memory_order_acquire) || !serverRunning.load()) return;
            if (auto* mp3 = engine.getMp3Player()) {
                mp3->nextTrack(true);
                notifyMp3Changed();
            }
        });
        return "{\"ok\":true}";
    } else if (path == "/api/mp3/prev") {
        juce::MessageManager::callAsync([this, life]() {
            if (!life || !life->alive.load(std::memory_order_acquire) || !serverRunning.load()) return;
            if (auto* mp3 = engine.getMp3Player()) {
                mp3->prevTrack();
                notifyMp3Changed();
            }
        });
        return "{\"ok\":true}";
    } else if (path == "/api/mp3/bank") {
        int bankIdx = 0;
        if (!jsonPayload.isObject() || !tryJsonInt(jsonPayload["index"], bankIdx, 0, 256))
            return "{\"ok\":false,\"error\":\"Invalid bank index\"}";
        juce::MessageManager::callAsync([this, life, bankIdx]() {
            if (!life || !life->alive.load(std::memory_order_acquire) || !serverRunning.load()) return;
            if (auto* mp3 = engine.getMp3Player()) {
                mp3->loadBank(bankIdx, true);
                notifyMp3Changed();
            }
        });
        return "{\"ok\":true}";
    } else if (path == "/api/mp3/track") {
        int trkIdx = 0;
        if (!jsonPayload.isObject() || !tryJsonInt(jsonPayload["index"], trkIdx, 0, 100000))
            return "{\"ok\":false,\"error\":\"Invalid track index\"}";
        juce::MessageManager::callAsync([this, life, trkIdx]() {
            if (!life || !life->alive.load(std::memory_order_acquire) || !serverRunning.load()) return;
            if (auto* mp3 = engine.getMp3Player()) {
                mp3->playTrack(trkIdx, true);
                notifyMp3Changed();
            }
        });
        return "{\"ok\":true}";
    } else if (path == "/api/mp3/gain") {
        float g = 1.0f;
        if (!jsonPayload.isObject() || !tryJsonFloat(jsonPayload["gain"], g, 0.0f, 2.0f))
            return "{\"ok\":false,\"error\":\"Invalid gain\"}";
        if (auto* mp3 = engine.getMp3Player()) {
            mp3->setGain(g);
            notifyMp3Changed();
        }
        return "{\"ok\":true}";
    } else if (path == "/api/mp3/autodj") {
        if (!jsonPayload.isObject())
            return "{\"ok\":false,\"error\":\"Invalid payload\"}";
        bool a = static_cast<bool>(jsonPayload["enabled"]);
        if (auto* mp3 = engine.getMp3Player()) {
            mp3->setAutoDjEnabled(a);
            notifyMp3Changed();
        }
        return "{\"ok\":true}";
    } else if (path == "/api/mp3/download") {
        if (!jsonPayload.isObject())
            return "{\"ok\":false,\"error\":\"Invalid payload\"}";
        juce::String url = jsonPayload["url"].toString().trim();
        bool playNow = (bool)jsonPayload.getProperty("playNow", false);
        if (url.isNotEmpty()) {
            YoutubeDownloadManager::getInstance().queueDownload(url, playNow,
                [this, life, playNow](bool success, const juce::File& file, const juce::String& /*title*/, const juce::String& /*err*/) {
                    if (!life || !life->alive.load(std::memory_order_acquire) || !serverRunning.load()) return;
                    if (success && file.existsAsFile()) {
                        if (auto* mp3 = engine.getMp3Player()) {
                            mp3->addFile(file);
                            if (playNow) {
                                int newIdx = (int)mp3->getPlaylistSnapshot().size() - 1;
                                mp3->playTrack(newIdx, true);
                            }
                            notifyMp3Changed();
                        }
                    }
                },
                [this, life](const juce::String& status, float /*progress*/) {
                    if (!life || !life->alive.load(std::memory_order_acquire) || !serverRunning.load()) return;
                    juce::DynamicObject::Ptr root = new juce::DynamicObject();
                    root->setProperty("type", "mp3_download_status");
                    root->setProperty("status", status);
                    broadcastWebSocket(juce::JSON::toString(juce::var(root.get())));
                }
            );
            return "{\"ok\":true,\"message\":\"Download queued\"}";
        }
        return "{\"ok\":false,\"error\":\"Empty URL\"}";
    }
    return "{\"ok\":false,\"error\":\"endpoint not found\"}";
}

juce::var WebServer::buildStatusJson() {
    juce::DynamicObject::Ptr obj = new juce::DynamicObject();
    obj->setProperty("ok", true);

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

    obj->setProperty("cpuLoad", mainComponent ? mainComponent->getCpuUsage() : 0.0);
    obj->setProperty("ramMb", (int)(FanfareLog::getMemoryStats().workingSetBytes / (1024 * 1024)));
    obj->setProperty("underruns", mainComponent ? mainComponent->getAudioUnderruns() : 0);
    obj->setProperty("thermalAlert", mainComponent ? mainComponent->isThermalWarningActive() : false);
    obj->setProperty("cpuTempC", mainComponent ? mainComponent->getCpuTemperatureCelsius() : -1);
    obj->setProperty("thermalText", mainComponent ? mainComponent->getThermalBadgeText() : juce::String("THERMAL"));
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

    // Strict traversal & absolute-path rejection (C1 + M6)
    if (cleanPath.contains("..")
        || cleanPath.containsChar(':')
        || cleanPath.containsChar('\\')
        || cleanPath.containsChar('\0')
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
                    sendHttpDataResponse(socketHandle, 200, getMimeType(cleanPath), block.getData(), block.getSize(), extraHeader);
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
        hudStaticDirs.add(cwdDir.getChildFile("Source/Web/static"));

        if (tryServeFromDirs(hudStaticDirs))
            return true;
    }

    // 2. Compiled-in HUD Production Fallback (never shadowed by docs/index.html!)
    if (cleanPath == "/index.html") {
        sendHttpDataResponse(socketHandle, 200, "text/html; charset=utf-8", EmbeddedWebAssets::INDEX_HTML_DATA, EmbeddedWebAssets::INDEX_HTML_DATA_LEN, extraHeader);
        return true;
    } else if (cleanPath == "/style.css") {
        sendHttpDataResponse(socketHandle, 200, "text/css; charset=utf-8", EmbeddedWebAssets::STYLE_CSS_DATA, EmbeddedWebAssets::STYLE_CSS_DATA_LEN, extraHeader);
        return true;
    } else if (cleanPath == "/app.js") {
        sendHttpDataResponse(socketHandle, 200, "application/javascript; charset=utf-8", EmbeddedWebAssets::APP_JS_DATA, EmbeddedWebAssets::APP_JS_DATA_LEN, extraHeader);
        return true;
    }

    // 3. Offline Stage Manual & Supporting Assets (/manual.html, /assets-readonly/*, screenshots inside docs/)
    juce::Array<juce::File> docsDirs;
    docsDirs.add(exeDir.getChildFile("docs"));
    docsDirs.add(cwdDir.getChildFile("docs"));

    if (tryServeFromDirs(docsDirs))
        return true;

    // Also allow manual.html inside packaged Web/static if docs/ folder is absent
    if (cleanPath.equalsIgnoreCase("/manual.html")) {
        juce::Array<juce::File> fallbackManualDirs;
        fallbackManualDirs.add(exeDir.getChildFile("Web/static"));
        fallbackManualDirs.add(exeDir.getChildFile("static"));
        if (tryServeFromDirs(fallbackManualDirs))
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
        case 400: statusText = "Bad Request"; break;
        case 401: statusText = "Unauthorized"; break;
        case 403: statusText = "Forbidden"; break;
        case 404: statusText = "Not Found"; break;
        case 405: statusText = "Method Not Allowed"; break;
        case 413: statusText = "Payload Too Large"; break;
        case 431: statusText = "Request Header Fields Too Large"; break;
        default: break;
    }

    std::ostringstream ss;
    ss << "HTTP/1.1 " << statusCode << " " << statusText << "\r\n";
    ss << "Content-Type: " << contentType.toStdString() << "\r\n";
    ss << "Content-Length: " << dataSize << "\r\n";
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
