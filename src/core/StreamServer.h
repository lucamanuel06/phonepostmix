#pragma once

#include "core/WebSocketProtocol.h"

#include <juce_core/juce_core.h>

#include <atomic>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace ppm
{

/** An HTTP + WebSocket server small enough to live inside an audio plugin.

    It does exactly two things: serve a handful of static assets over HTTP, and hold open
    WebSocket connections that audio packets are broadcast down. There is no TLS, no
    keep-alive, no routing and no request body handling, because none of those are needed
    to hand a browser one page and then push bytes at it.

    Threading:
      - one acceptor thread blocks in `waitForNextConnection`;
      - each accepted connection gets its own thread, which owns that socket exclusively;
      - `broadcastBinary`/`broadcastText` may be called from any thread except the audio
        thread — they take a lock and copy, so they are not real-time safe.

    Shutdown is the part that traps people: a blocking `accept` cannot be interrupted by a
    flag. `stop()` closes the listening socket, which makes `waitForNextConnection` return
    immediately, and closes each client socket, which makes its read return. Only then are
    the threads joined.
*/
class StreamServer
{
public:
    /** A static asset held in memory, typically pointing straight at BinaryData. */
    struct Asset
    {
        const char* data = nullptr;
        int size = 0;
        std::string mimeType;
    };

    /** Resolves a request path such as "/" or "/app.js" to an asset, or nullopt for 404. */
    using AssetProvider = std::function<std::optional<Asset> (const std::string& path)>;

    /** Decides whether a WebSocket upgrade may proceed, given the raw request target.

        Called with the full target including the query string, e.g. "/ws?t=abc123".
        Returning false answers 403 and closes. Used to check the pairing token, so that
        anyone else on the network who finds the port gets the page but not the audio.
        If unset, every upgrade is accepted.
    */
    using UpgradeAuthorizer = std::function<bool (const std::string& target)>;

    /** Called on a connection's own thread. Keep these handlers short and non-blocking. */
    struct Listener
    {
        virtual ~Listener() = default;
        virtual void streamClientConnected (int /*clientId*/) {}
        virtual void streamClientDisconnected (int /*clientId*/) {}
        virtual void streamTextMessageReceived (int /*clientId*/, const std::string& /*message*/) {}
    };

    StreamServer();
    ~StreamServer();

    /** Binds a port and starts accepting.

        Tries `preferredPort` first, then the next `portSearchRange` ports, because a
        second plugin instance (or a stale socket from a crashed host) will already own
        the preferred one. Returns false only if the whole range is taken.
    */
    bool start (int preferredPort, AssetProvider provider, Listener* listener = nullptr);

    /** Installs the upgrade check. Call before `start`. */
    void setUpgradeAuthorizer (UpgradeAuthorizer authorizer) { upgradeAuthorizer = std::move (authorizer); }

    /** Stops accepting, disconnects every client and joins all threads. Idempotent. */
    void stop();

    bool isRunning() const noexcept   { return running.load(); }
    int getPort() const noexcept      { return boundPort.load(); }
    int getNumClients() const;

    /** Queues a binary frame for every connected WebSocket client. */
    void broadcastBinary (const void* data, size_t size);

    /** Queues a text frame for every connected WebSocket client. */
    void broadcastText (const std::string& text);

    /** Total frames dropped across all clients because a client could not keep up. */
    int getDropCount() const noexcept { return drops.load(); }

    /** How many queued bytes a single client may accumulate before frames are dropped. */
    void setPerClientQueueLimit (size_t bytes) { queueLimitBytes = bytes; }

private:
    class Connection;
    class Acceptor;

    std::unique_ptr<Acceptor> acceptor;
    std::unique_ptr<juce::StreamingSocket> listeningSocket;

    mutable std::mutex connectionsMutex;
    std::vector<std::shared_ptr<Connection>> connections;

    AssetProvider assetProvider;
    UpgradeAuthorizer upgradeAuthorizer;
    Listener* listener = nullptr;

    std::atomic<bool> running { false };
    std::atomic<int> boundPort { 0 };
    std::atomic<int> drops { 0 };
    std::atomic<int> nextClientId { 1 };
    size_t queueLimitBytes = 2u << 20;

    JUCE_DECLARE_NON_COPYABLE (StreamServer)
};

} // namespace ppm
