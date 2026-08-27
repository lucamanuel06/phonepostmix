#include "TestSupport.h"

#include "core/HttpMessage.h"
#include "core/StreamServer.h"

#include <chrono>
#include <thread>

namespace
{

constexpr int testPort = 34871;

/** A deliberately dumb WebSocket client, written against the RFC rather than against the
    server, so a bug that is symmetrical in both would still be caught.
*/
class TestClient
{
public:
    bool connectTo (int port)
    {
        return socket.connect ("127.0.0.1", port, 2000);
    }

    bool sendRaw (const std::string& text)
    {
        return socket.write (text.data(), static_cast<int> (text.size())) == static_cast<int> (text.size());
    }

    /** Reads until `predicate(buffer)` is happy or the timeout expires. */
    bool readUntil (const std::function<bool (const std::string&)>& predicate, int timeoutMs = 3000)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds (timeoutMs);

        while (std::chrono::steady_clock::now() < deadline)
        {
            if (predicate (buffer))
                return true;

            if (socket.waitUntilReady (true, 50) > 0)
            {
                char chunk[4096];
                const auto read = socket.read (chunk, sizeof (chunk), false);

                if (read <= 0)
                    return predicate (buffer);

                buffer.append (chunk, static_cast<size_t> (read));
            }
        }

        return predicate (buffer);
    }

    bool performHandshake (int port, const std::string& path = "/ws")
    {
        if (! connectTo (port))
            return false;

        const auto request = "GET " + path + " HTTP/1.1\r\n"
                             "Host: 127.0.0.1\r\n"
                             "Upgrade: websocket\r\n"
                             "Connection: Upgrade\r\n"
                             "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                             "Sec-WebSocket-Version: 13\r\n\r\n";

        if (! sendRaw (request))
            return false;

        if (! readUntil ([] (const std::string& b) { return b.find ("\r\n\r\n") != std::string::npos; }))
            return false;

        const auto headEnd = buffer.find ("\r\n\r\n");
        const auto head = buffer.substr (0, headEnd);
        buffer.erase (0, headEnd + 4);

        return head.find ("101 Switching Protocols") != std::string::npos
            && head.find ("Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=") != std::string::npos;
    }

    /** Sends a masked client frame, as RFC 6455 requires. */
    bool sendFrame (uint8_t opcode, const std::string& payload)
    {
        std::string f;
        f.push_back (static_cast<char> (0x80 | opcode));
        f.push_back (static_cast<char> (0x80 | payload.size()));

        const char mask[4] = { 0x01, 0x02, 0x03, 0x04 };
        f.append (mask, 4);

        for (size_t i = 0; i < payload.size(); ++i)
            f.push_back (static_cast<char> (payload[i] ^ mask[i % 4]));

        return sendRaw (f);
    }

    std::string& raw() { return buffer; }
    void close() { socket.close(); }

private:
    juce::StreamingSocket socket;
    std::string buffer;
};

struct RecordingListener : ppm::StreamServer::Listener
{
    void streamClientConnected (int) override    { connects.fetch_add (1); }
    void streamClientDisconnected (int) override { disconnects.fetch_add (1); }

    void streamTextMessageReceived (int, const std::string& message) override
    {
        std::lock_guard<std::mutex> lock (mutex);
        received.push_back (message);
    }

    std::vector<std::string> snapshot()
    {
        std::lock_guard<std::mutex> lock (mutex);
        return received;
    }

    std::atomic<int> connects { 0 };
    std::atomic<int> disconnects { 0 };
    std::mutex mutex;
    std::vector<std::string> received;
};

ppm::StreamServer::AssetProvider testAssets()
{
    return [] (const std::string& path) -> std::optional<ppm::StreamServer::Asset>
    {
        static constexpr const char* indexHtml = "<!doctype html><title>ok</title>";

        if (path == "/" || path == "/index.html")
            return ppm::StreamServer::Asset { indexHtml, static_cast<int> (std::strlen (indexHtml)),
                                              "text/html; charset=utf-8" };

        return std::nullopt;
    };
}

/** Spins until `predicate` holds or the timeout expires, without a fixed sleep. */
bool waitFor (const std::function<bool()>& predicate, int timeoutMs = 3000)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds (timeoutMs);

    while (std::chrono::steady_clock::now() < deadline)
    {
        if (predicate())
            return true;

        std::this_thread::sleep_for (std::chrono::milliseconds (5));
    }

    return predicate();
}

} // namespace

PPM_TEST (serverServesAStaticAssetAndClosesTheConnection)
{
    ppm::StreamServer server;
    PPM_CHECK (server.start (testPort, testAssets()));

    TestClient client;
    PPM_CHECK (client.connectTo (server.getPort()));
    PPM_CHECK (client.sendRaw ("GET / HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n"));
    PPM_CHECK (client.readUntil ([] (const std::string& b) { return b.find ("<title>ok</title>") != std::string::npos; }));
    PPM_CHECK (client.raw().find ("200 OK") != std::string::npos);
    PPM_CHECK (client.raw().find ("Content-Type: text/html") != std::string::npos);
}

PPM_TEST (serverReturns404ForAnUnknownPath)
{
    ppm::StreamServer server;
    PPM_CHECK (server.start (testPort + 1, testAssets()));

    TestClient client;
    PPM_CHECK (client.connectTo (server.getPort()));
    PPM_CHECK (client.sendRaw ("GET /nope HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n"));
    PPM_CHECK (client.readUntil ([] (const std::string& b) { return b.find ("404") != std::string::npos; }));
}

PPM_TEST (serverUpgradesAndBroadcastsBinaryFrames)
{
    RecordingListener listener;
    ppm::StreamServer server;
    PPM_CHECK (server.start (testPort + 2, testAssets(), &listener));

    TestClient client;
    PPM_CHECK (client.performHandshake (server.getPort()));
    PPM_CHECK (waitFor ([&] { return server.getNumClients() == 1; }));
    PPM_CHECK_EQ (listener.connects.load(), 1);

    const std::vector<uint8_t> payload { 0xde, 0xad, 0xbe, 0xef };
    server.broadcastBinary (payload.data(), payload.size());

    PPM_CHECK (client.readUntil ([] (const std::string& b) { return b.size() >= 6; }));

    const auto& raw = client.raw();
    PPM_CHECK (raw.size() >= 6);

    if (raw.size() >= 6)
    {
        PPM_CHECK_EQ (int (static_cast<uint8_t> (raw[0])), 0x82);  // FIN + binary
        PPM_CHECK_EQ (int (static_cast<uint8_t> (raw[1])), 4);     // unmasked, 4 bytes
        PPM_CHECK_EQ (int (static_cast<uint8_t> (raw[2])), 0xde);
        PPM_CHECK_EQ (int (static_cast<uint8_t> (raw[5])), 0xef);
    }
}

PPM_TEST (serverForwardsClientTextMessagesToTheListener)
{
    RecordingListener listener;
    ppm::StreamServer server;
    PPM_CHECK (server.start (testPort + 3, testAssets(), &listener));

    TestClient client;
    PPM_CHECK (client.performHandshake (server.getPort()));
    PPM_CHECK (waitFor ([&] { return server.getNumClients() == 1; }));

    PPM_CHECK (client.sendFrame (0x1, "{\"type\":\"hello\"}"));
    PPM_CHECK (waitFor ([&] { return ! listener.snapshot().empty(); }));

    const auto messages = listener.snapshot();
    PPM_CHECK (! messages.empty() && messages.front() == "{\"type\":\"hello\"}");
}

PPM_TEST (serverAnswersPingsWithPongs)
{
    ppm::StreamServer server;
    PPM_CHECK (server.start (testPort + 4, testAssets()));

    TestClient client;
    PPM_CHECK (client.performHandshake (server.getPort()));
    PPM_CHECK (waitFor ([&] { return server.getNumClients() == 1; }));

    PPM_CHECK (client.sendFrame (0x9, "hi"));
    PPM_CHECK (client.readUntil ([] (const std::string& b)
                                 { return b.size() >= 2 && static_cast<uint8_t> (b[0]) == 0x8A; }));
}

PPM_TEST (serverNoticesAClientDisconnecting)
{
    RecordingListener listener;
    ppm::StreamServer server;
    PPM_CHECK (server.start (testPort + 5, testAssets(), &listener));

    {
        TestClient client;
        PPM_CHECK (client.performHandshake (server.getPort()));
        PPM_CHECK (waitFor ([&] { return server.getNumClients() == 1; }));
        client.close();
    }

    PPM_CHECK (waitFor ([&] { return listener.disconnects.load() == 1; }));
    PPM_CHECK (waitFor ([&] { return server.getNumClients() == 0; }));
}

PPM_TEST (serverFallsBackToTheNextFreePort)
{
    ppm::StreamServer first, second;

    PPM_CHECK (first.start (testPort + 6, testAssets()));
    PPM_CHECK (second.start (testPort + 6, testAssets()));

    // A second plugin instance must not fail to start just because the first one owns the
    // preferred port.
    PPM_CHECK (first.getPort() != second.getPort());
    PPM_CHECK (second.getPort() > first.getPort());
}

PPM_TEST (serverStopsCleanlyWhileAClientIsConnected)
{
    ppm::StreamServer server;
    PPM_CHECK (server.start (testPort + 8, testAssets()));

    TestClient client;
    PPM_CHECK (client.performHandshake (server.getPort()));
    PPM_CHECK (waitFor ([&] { return server.getNumClients() == 1; }));

    // The interesting part is that this returns at all: a blocking accept and a blocking
    // read both have to be interrupted for stop() to finish.
    const auto start = std::chrono::steady_clock::now();
    server.stop();
    const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds> (
                               std::chrono::steady_clock::now() - start).count();

    PPM_CHECK (elapsedMs < 2000);
    PPM_CHECK (! server.isRunning());
    PPM_CHECK_EQ (server.getNumClients(), 0);
}

PPM_TEST (serverDropsOldestFramesWhenAClientStopsReading)
{
    ppm::StreamServer server;
    server.setPerClientQueueLimit (64 * 1024);
    PPM_CHECK (server.start (testPort + 9, testAssets()));

    TestClient client;
    PPM_CHECK (client.performHandshake (server.getPort()));
    PPM_CHECK (waitFor ([&] { return server.getNumClients() == 1; }));

    // The client never reads, so the server's socket buffer fills and the queue grows
    // until backpressure kicks in. Without the drop policy this would grow without bound.
    const std::vector<uint8_t> block (4096, 0x5a);

    for (int i = 0; i < 2000; ++i)
        server.broadcastBinary (block.data(), block.size());

    PPM_CHECK (waitFor ([&] { return server.getDropCount() > 0; }, 5000));
}
