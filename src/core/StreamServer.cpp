#include "core/StreamServer.h"

#include "core/HttpMessage.h"

#include <algorithm>
#include <cstring>

namespace ppm
{

namespace
{
constexpr int socketPollMs        = 50;    ///< how long a read blocks before re-checking the exit flag
constexpr int acceptPollMs        = 100;   ///< how long the acceptor waits before re-checking the exit flag
constexpr int writeReadyMs        = 100;   ///< how long a send waits for the socket to become writable
constexpr int writeDeadlineMs     = 5000;  ///< after this long unable to send, the client is treated as gone
constexpr int maxRequestHeadBytes = 16384; ///< a browser request head that exceeds this is not one we want
} // namespace

//==============================================================================
/** One accepted TCP connection, owning its socket and its own thread.

    A connection starts as HTTP. It either answers with a static asset and closes, or
    upgrades to WebSocket and lives until either side hangs up.
*/
class StreamServer::Connection final : private juce::Thread
{
public:
    Connection (StreamServer& ownerIn, std::unique_ptr<juce::StreamingSocket> socketIn, int idIn)
        : juce::Thread ("PhonePostMix connection"),
          owner (ownerIn),
          socket (std::move (socketIn)),
          clientId (idIn)
    {
    }

    ~Connection() override
    {
        disconnect();
    }

    void begin() { startThread (juce::Thread::Priority::normal); }

    /** Unblocks the connection thread and waits for it to finish. Safe to call twice. */
    void disconnect()
    {
        signalThreadShouldExit();

        // Closing the socket is what actually interrupts a blocked read; the exit flag
        // alone would leave this thread parked until the peer happened to send something.
        if (socket != nullptr)
            socket->close();

        stopThread (2000);
    }

    int getId() const noexcept { return clientId; }
    bool isWebSocket() const noexcept { return upgraded.load(); }

    /** True once the connection thread has run to completion and the socket is closed. */
    bool isFinished() const noexcept { return finished.load(); }

    /** Queues an already-serialised frame. Called from the streaming thread. */
    void enqueue (const std::shared_ptr<const std::vector<uint8_t>>& frame, bool droppable)
    {
        if (! upgraded.load())
            return;

        std::lock_guard<std::mutex> lock (queueMutex);

        queue.push_back ({ frame, droppable });
        queuedBytes += frame->size();

        // Backpressure: a phone on a congested access point stops draining, and the queue
        // would otherwise grow without bound and turn into latency. Drop the *oldest*
        // audio, never the newest — a listener wants to hear what is happening now, and a
        // stale backlog is worse than a gap. Control frames are never dropped.
        while (queuedBytes > owner.queueLimitBytes && queue.size() > 1)
        {
            const auto it = std::find_if (queue.begin(), queue.end(),
                                          [] (const Outgoing& o) { return o.droppable; });

            if (it == queue.end())
                break;

            queuedBytes -= it->frame->size();
            queue.erase (it);
            owner.drops.fetch_add (1, std::memory_order_relaxed);
        }
    }

private:
    struct Outgoing
    {
        std::shared_ptr<const std::vector<uint8_t>> frame;
        bool droppable = true;
    };

    void run() override
    {
        if (! handleHttp())
        {
            finish();
            return;
        }

        while (! threadShouldExit())
        {
            if (! drainQueue())
                break;

            if (! pumpIncoming())
                break;
        }

        finish();
    }

    void finish()
    {
        if (upgraded.exchange (false))
            if (owner.listener != nullptr)
                owner.listener->streamClientDisconnected (clientId);

        if (socket != nullptr)
            socket->close();

        finished.store (true);
    }

    /** Reads the request head, then either serves an asset or performs the upgrade. */
    bool handleHttp()
    {
        std::string head;

        while (! threadShouldExit())
        {
            char chunk[2048];
            const auto ready = socket->waitUntilReady (true, socketPollMs);

            if (ready < 0)
                return false;

            if (ready == 0)
                continue;

            const auto read = socket->read (chunk, sizeof (chunk), false);

            if (read <= 0)
                return false;

            head.append (chunk, static_cast<size_t> (read));

            if (head.find ("\r\n\r\n") != std::string::npos)
                break;

            if (head.size() > maxRequestHeadBytes)
                return false;
        }

        const auto request = http::parseRequest (head);

        if (! request.has_value())
            return false;

        if (request->isWebSocketUpgrade())
        {
            if (owner.upgradeAuthorizer && ! owner.upgradeAuthorizer (request->target))
            {
                static constexpr const char* body = "Forbidden";
                const auto denial = http::makeResponseHead (403, "Forbidden", "text/plain; charset=utf-8",
                                                            static_cast<long long> (std::strlen (body)),
                                                            { "Connection: close" });
                writeAll (denial.data(), denial.size());
                writeAll (body, std::strlen (body));
                return false;
            }

            return performUpgrade (*request);
        }

        serveAsset (*request);
        return false;   // static responses close the connection
    }

    bool performUpgrade (const http::Request& request)
    {
        const auto accept = websocket::makeAcceptKey (request.header ("sec-websocket-key"));

        const auto response = http::makeResponseHead (101, "Switching Protocols", {}, -1,
                                                      { "Upgrade: websocket",
                                                        "Connection: Upgrade",
                                                        "Sec-WebSocket-Accept: " + accept });

        if (! writeAll (response.data(), response.size()))
            return false;

        upgraded.store (true);

        if (owner.listener != nullptr)
            owner.listener->streamClientConnected (clientId);

        return true;
    }

    void serveAsset (const http::Request& request)
    {
        const auto asset = owner.assetProvider ? owner.assetProvider (request.path) : std::nullopt;

        if (! asset.has_value())
        {
            static constexpr const char* body = "Not found";
            const auto head = http::makeResponseHead (404, "Not Found", "text/plain; charset=utf-8",
                                                      static_cast<long long> (std::strlen (body)),
                                                      { "Connection: close" });
            writeAll (head.data(), head.size());
            writeAll (body, std::strlen (body));
            return;
        }

        const auto head = http::makeResponseHead (200, "OK", asset->mimeType, asset->size,
                                                  { "Connection: close",
                                                    // The page is regenerated by every
                                                    // build; a cached copy from an older
                                                    // plugin version would silently
                                                    // mismatch the wire protocol.
                                                    "Cache-Control: no-store" });

        if (writeAll (head.data(), head.size()))
            writeAll (asset->data, static_cast<size_t> (asset->size));
    }

    /** Sends everything currently queued. Returns false if the socket died. */
    bool drainQueue()
    {
        for (;;)
        {
            Outgoing next;

            {
                std::lock_guard<std::mutex> lock (queueMutex);

                if (queue.empty())
                    return true;

                next = queue.front();
                queue.pop_front();
                queuedBytes -= next.frame->size();
            }

            if (! writeAll (reinterpret_cast<const char*> (next.frame->data()), next.frame->size()))
                return false;
        }
    }

    /** Reads whatever the client sent and answers pings and closes. */
    bool pumpIncoming()
    {
        const auto ready = socket->waitUntilReady (true, socketPollMs);

        if (ready < 0)
            return false;

        if (ready > 0)
        {
            uint8_t chunk[2048];
            const auto read = socket->read (chunk, sizeof (chunk), false);

            if (read <= 0)
                return false;

            parser.push (chunk, static_cast<size_t> (read));
        }

        while (auto frame = parser.nextFrame())
        {
            switch (frame->opcode)
            {
                case websocket::Opcode::close:
                    return false;

                case websocket::Opcode::ping:
                {
                    auto pong = std::make_shared<std::vector<uint8_t>>();
                    websocket::writeFrame (*pong, websocket::Opcode::pong,
                                           frame->payload.data(), frame->payload.size());
                    enqueue (pong, false);
                    break;
                }

                case websocket::Opcode::text:
                    if (owner.listener != nullptr)
                        owner.listener->streamTextMessageReceived (
                            clientId, std::string (frame->payload.begin(), frame->payload.end()));
                    break;

                case websocket::Opcode::binary:
                case websocket::Opcode::pong:
                case websocket::Opcode::continuation:
                default:
                    break;
            }
        }

        return ! parser.hasError();
    }

    bool writeAll (const char* data, size_t size)
    {
        size_t sent = 0;
        const auto deadline = juce::Time::getMillisecondCounter() + writeDeadlineMs;

        while (sent < size)
        {
            if (threadShouldExit())
                return false;

            // A peer that has gone away without closing (phone out of range, laptop lid
            // shut) never becomes writable again. Without a deadline this thread would sit
            // here until the plugin is unloaded.
            if (juce::Time::getMillisecondCounter() > deadline)
                return false;

            // StreamingSocket::write() blocks once the kernel send buffer is full, which
            // is exactly what happens when TCP starts retransmitting on a weak Wi-Fi
            // link. Blocking there would stop this thread draining its queue, so wait for
            // writability first and give up on the frame if the peer has stalled — the
            // queue's drop policy is what keeps latency bounded from there.
            const auto ready = socket->waitUntilReady (false, writeReadyMs);

            if (ready < 0)
                return false;

            if (ready == 0)
                continue;

            const auto written = socket->write (data + sent, static_cast<int> (size - sent));

            if (written <= 0)
                return false;

            sent += static_cast<size_t> (written);
        }

        return true;
    }

    StreamServer& owner;
    std::unique_ptr<juce::StreamingSocket> socket;
    const int clientId;

    websocket::FrameParser parser;
    std::atomic<bool> upgraded { false };
    std::atomic<bool> finished { false };

    std::mutex queueMutex;
    std::deque<Outgoing> queue;
    size_t queuedBytes = 0;
};

//==============================================================================
/** Blocks in `accept` and spawns a Connection per incoming socket. */
class StreamServer::Acceptor final : private juce::Thread
{
public:
    explicit Acceptor (StreamServer& ownerIn)
        : juce::Thread ("PhonePostMix acceptor"), owner (ownerIn)
    {
    }

    ~Acceptor() override { stopThread (2000); }

    void begin() { startThread (juce::Thread::Priority::normal); }
    void requestStop() { signalThreadShouldExit(); }
    void join() { stopThread (2000); }

private:
    void run() override
    {
        while (! threadShouldExit())
        {
            // Poll rather than block in accept().
            //
            // juce::StreamingSocket::waitForNextConnection() is a bare accept() with no
            // timeout. JUCE unblocks it from close() by connecting to 127.0.0.1 on the
            // listening port, which has two problems: it only works if the listener is
            // bound to INADDR_ANY, and the socket that self-connect produces is accepted
            // and then leaked, because waitForNextConnection() has already seen
            // connected == false and returns nullptr without closing it. A host that
            // instantiates the plugin hundreds of times during validation would leak a
            // file descriptor per teardown.
            //
            // waitUntilReady() is select()-based and works on a listening socket, where
            // readable means "a connection is pending". Polling it means shutdown costs
            // at most one poll interval and never depends on the self-connect at all.
            const auto ready = owner.listeningSocket->waitUntilReady (true, acceptPollMs);

            if (ready < 0)
                break;

            if (ready == 0)
                continue;

            std::unique_ptr<juce::StreamingSocket> accepted { owner.listeningSocket->waitForNextConnection() };

            if (accepted == nullptr)
                continue;

            auto connection = std::make_shared<Connection> (owner, std::move (accepted),
                                                            owner.nextClientId.fetch_add (1));

            {
                std::lock_guard<std::mutex> lock (owner.connectionsMutex);

                // Reap connections whose threads have already finished, so a long session
                // does not accumulate dead entries.
                owner.connections.erase (std::remove_if (owner.connections.begin(),
                                                         owner.connections.end(),
                                                         [] (const std::shared_ptr<Connection>& c)
                                                         { return c->isFinished(); }),
                                         owner.connections.end());

                owner.connections.push_back (connection);
            }

            connection->begin();
        }
    }

    StreamServer& owner;
};

//==============================================================================
StreamServer::StreamServer() = default;

StreamServer::~StreamServer()
{
    stop();
}

bool StreamServer::start (int preferredPort, AssetProvider provider, Listener* listenerIn)
{
    stop();

    assetProvider = std::move (provider);
    listener = listenerIn;

    listeningSocket = std::make_unique<juce::StreamingSocket>();

    constexpr int portSearchRange = 20;

    for (int offset = 0; offset < portSearchRange; ++offset)
    {
        // Bind to every interface: the phone connects over the LAN, so localhost-only
        // would defeat the point.
        if (listeningSocket->createListener (preferredPort + offset, "0.0.0.0"))
        {
            boundPort.store (preferredPort + offset);
            break;
        }
    }

    if (boundPort.load() == 0)
    {
        listeningSocket.reset();
        return false;
    }

    running.store (true);
    acceptor = std::make_unique<Acceptor> (*this);
    acceptor->begin();

    return true;
}

void StreamServer::stop()
{
    if (! running.exchange (false) && acceptor == nullptr)
        return;

    // Order matters, and it is the opposite of the obvious one. The acceptor polls with a
    // timeout, so signalling and joining it first costs at most one poll interval and
    // leaves the listening socket untouched. Closing the listener while the acceptor is
    // still alive would trigger JUCE's loopback self-connect hack and leak the descriptor
    // it produces.
    if (acceptor != nullptr)
    {
        acceptor->requestStop();
        acceptor->join();
        acceptor.reset();
    }

    if (listeningSocket != nullptr)
        listeningSocket->close();

    std::vector<std::shared_ptr<Connection>> toClose;

    {
        std::lock_guard<std::mutex> lock (connectionsMutex);
        toClose.swap (connections);
    }

    for (auto& connection : toClose)
        connection->disconnect();

    toClose.clear();
    listeningSocket.reset();
    boundPort.store (0);
}

int StreamServer::getNumClients() const
{
    std::lock_guard<std::mutex> lock (connectionsMutex);

    return static_cast<int> (std::count_if (connections.begin(), connections.end(),
                                            [] (const std::shared_ptr<Connection>& c)
                                            { return c->isWebSocket(); }));
}

void StreamServer::broadcastBinary (const void* data, size_t size)
{
    auto frame = std::make_shared<std::vector<uint8_t>>();
    frame->reserve (size + 10);
    websocket::writeFrame (*frame, websocket::Opcode::binary, data, size);

    std::lock_guard<std::mutex> lock (connectionsMutex);

    for (auto& connection : connections)
        connection->enqueue (frame, true);
}

void StreamServer::broadcastText (const std::string& text)
{
    auto frame = std::make_shared<std::vector<uint8_t>>();
    websocket::writeTextFrame (*frame, text);

    std::lock_guard<std::mutex> lock (connectionsMutex);

    for (auto& connection : connections)
        connection->enqueue (frame, false);
}

} // namespace ppm
