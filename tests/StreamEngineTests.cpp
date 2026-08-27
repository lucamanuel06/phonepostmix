#include "TestSupport.h"

#include "core/StreamEngine.h"
#include "core/WirePacket.h"

#include <chrono>
#include <thread>

namespace
{

/** Waits for a condition without a fixed sleep, so the suite stays fast when things work. */
bool eventually (const std::function<bool()>& predicate, int timeoutMs = 4000)
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

juce::String tokenFromUrl (const juce::String& url)
{
    return url.fromFirstOccurrenceOf ("#t=", false, false);
}

/** Minimal WebSocket client, enough to complete a handshake and read binary frames. */
class EngineClient
{
public:
    bool open (int port, const juce::String& target)
    {
        if (! socket.connect ("127.0.0.1", port, 2000))
            return false;

        const auto request = "GET " + target.toStdString() + " HTTP/1.1\r\n"
                             "Host: 127.0.0.1\r\n"
                             "Upgrade: websocket\r\n"
                             "Connection: Upgrade\r\n"
                             "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                             "Sec-WebSocket-Version: 13\r\n\r\n";

        if (socket.write (request.data(), static_cast<int> (request.size())) < 0)
            return false;

        while (buffer.find ("\r\n\r\n") == std::string::npos)
            if (! pump (2000))
                return false;

        const auto headEnd = buffer.find ("\r\n\r\n");
        head = buffer.substr (0, headEnd);
        buffer.erase (0, headEnd + 4);

        return head.find ("101 Switching Protocols") != std::string::npos;
    }

    const std::string& getResponseHead() const { return head; }

    /** Reads until a binary frame of at least `minPayload` bytes is available. */
    bool waitForBinaryFrame (size_t minPayload, std::vector<uint8_t>& payload, int timeoutMs = 4000)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds (timeoutMs);

        while (std::chrono::steady_clock::now() < deadline)
        {
            if (extractBinaryFrame (payload) && payload.size() >= minPayload)
                return true;

            pump (50);
        }

        return false;
    }

    void close() { socket.close(); }

private:
    bool pump (int timeoutMs)
    {
        if (socket.waitUntilReady (true, timeoutMs) <= 0)
            return false;

        char chunk[8192];
        const auto read = socket.read (chunk, sizeof (chunk), false);

        if (read <= 0)
            return false;

        buffer.append (chunk, static_cast<size_t> (read));
        return true;
    }

    /** Server frames are never masked and never fragmented, so this stays simple. */
    bool extractBinaryFrame (std::vector<uint8_t>& payload)
    {
        while (buffer.size() >= 2)
        {
            const auto* p = reinterpret_cast<const uint8_t*> (buffer.data());
            const auto opcode = p[0] & 0x0f;
            size_t headerSize = 2;
            uint64_t size = p[1] & 0x7f;

            if (size == 126)
            {
                if (buffer.size() < 4) return false;
                size = (uint64_t (p[2]) << 8) | p[3];
                headerSize = 4;
            }
            else if (size == 127)
            {
                if (buffer.size() < 10) return false;
                size = 0;
                for (int i = 0; i < 8; ++i) size = (size << 8) | p[2 + i];
                headerSize = 10;
            }

            if (buffer.size() < headerSize + size)
                return false;

            if (opcode == 0x2)
            {
                payload.assign (buffer.begin() + static_cast<long> (headerSize),
                                buffer.begin() + static_cast<long> (headerSize + size));
                buffer.erase (0, headerSize + static_cast<size_t> (size));
                return true;
            }

            buffer.erase (0, headerSize + static_cast<size_t> (size));   // skip text/control
        }

        return false;
    }

    juce::StreamingSocket socket;
    std::string buffer, head;
};

void pushSilence (ppm::StreamEngine& engine, int blocks, int blockSize)
{
    juce::AudioBuffer<float> block (2, blockSize);

    for (int i = 0; i < blocks; ++i)
    {
        for (int ch = 0; ch < 2; ++ch)
            juce::FloatVectorOperations::fill (block.getWritePointer (ch), 0.25f, blockSize);

        engine.pushAudio (block);
        std::this_thread::sleep_for (std::chrono::milliseconds (2));
    }
}

} // namespace

PPM_TEST (engineServesTheReceiverPageFromBinaryData)
{
    ppm::StreamEngine engine;
    ppm::StreamEngine::Settings settings;
    settings.preferredPort = 35120;
    engine.setSettings (settings);

    PPM_CHECK (engine.start());

    juce::StreamingSocket client;
    PPM_CHECK (client.connect ("127.0.0.1", engine.getPort(), 2000));

    const std::string request = "GET / HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n";
    client.write (request.data(), static_cast<int> (request.size()));

    std::string response;
    for (int i = 0; i < 40 && response.find ("</html>") == std::string::npos; ++i)
    {
        if (client.waitUntilReady (true, 100) <= 0)
            continue;

        char chunk[8192];
        const auto read = client.read (chunk, sizeof (chunk), false);

        if (read <= 0)
            break;

        response.append (chunk, static_cast<size_t> (read));
    }

    PPM_CHECK (response.find ("200 OK") != std::string::npos);
    PPM_CHECK (response.find ("PhonePostMix") != std::string::npos);
    PPM_CHECK (response.find ("text/html") != std::string::npos);
}

PPM_TEST (engineRejectsAWebSocketUpgradeWithoutTheToken)
{
    ppm::StreamEngine engine;
    ppm::StreamEngine::Settings settings;
    settings.preferredPort = 35130;
    engine.setSettings (settings);

    PPM_CHECK (engine.start());

    // Anyone else on the same Wi-Fi can find the port. They may have the page; they may
    // not have the audio.
    EngineClient noToken;
    PPM_CHECK (! noToken.open (engine.getPort(), "/ws"));
    PPM_CHECK (noToken.getResponseHead().find ("403") != std::string::npos);

    EngineClient wrongToken;
    PPM_CHECK (! wrongToken.open (engine.getPort(), "/ws?t=deadbeef"));
}

PPM_TEST (engineStreamsPacketsThatMatchTheDeclaredFormat)
{
    ppm::StreamEngine engine;
    ppm::StreamEngine::Settings settings;
    settings.preferredPort = 35140;
    settings.framesPerPacket = 256;
    settings.format = ppm::wire::Format::pcm16;
    engine.setSettings (settings);

    engine.prepare (48000.0, 512, 2);
    PPM_CHECK (engine.start());
    engine.setHostPlaying (true);

    const auto url = engine.getListenUrl();
    PPM_CHECK (url.isNotEmpty());

    EngineClient client;
    PPM_CHECK (client.open (engine.getPort(), "/ws?t=" + tokenFromUrl (url)));
    PPM_CHECK (eventually ([&] { return engine.getNumClients() == 1; }));

    std::thread producer ([&] { pushSilence (engine, 200, 512); });

    std::vector<uint8_t> payload;
    const auto got = client.waitForBinaryFrame (ppm::wire::headerBytes, payload);

    producer.join();
    PPM_CHECK (got);

    if (got)
    {
        ppm::wire::PacketInfo info;
        PPM_CHECK (ppm::wire::readHeader (payload.data(), payload.size(), info));
        PPM_CHECK (info.format == ppm::wire::Format::pcm16);
        PPM_CHECK_EQ (int (info.channels), 2);
        PPM_CHECK_EQ (int (info.frames), 256);
        PPM_CHECK_EQ (int (info.sampleRate), 48000);
        PPM_CHECK_EQ (int (payload.size()), ppm::wire::packetSize (info));

        // The very first packet of a stream must be marked so the receiver flushes
        // whatever it had from a previous session.
        PPM_CHECK ((info.flags & ppm::wire::Flags::discontinuity) != 0);

        // The producer wrote 0.25f into both channels; at pcm16 that is 8192.
        const auto sample = static_cast<int16_t> (
            static_cast<uint16_t> (payload[ppm::wire::headerBytes])
            | (static_cast<uint16_t> (payload[ppm::wire::headerBytes + 1]) << 8));

        PPM_CHECK (std::abs (int (sample) - 8192) <= 1);
    }
}

PPM_TEST (engineUrlCarriesTheTokenInTheFragment)
{
    ppm::StreamEngine engine;
    ppm::StreamEngine::Settings settings;
    settings.preferredPort = 35150;
    engine.setSettings (settings);

    PPM_CHECK (engine.start());

    const auto url = engine.getListenUrl();

    // The token lives in the fragment so it never reaches a server log or a Referer
    // header; the page moves it onto the WebSocket query string itself.
    PPM_CHECK (url.startsWith ("http://"));
    PPM_CHECK (url.contains (":" + juce::String (engine.getPort()) + "/#t="));
    PPM_CHECK (tokenFromUrl (url).length() >= 16);
}

PPM_TEST (engineDoesNothingWhileStopped)
{
    ppm::StreamEngine engine;
    engine.prepare (48000.0, 512, 2);

    // pushAudio must be safe and free before start() and after stop(), because the audio
    // thread keeps calling it regardless of what the user has clicked.
    juce::AudioBuffer<float> block (2, 512);
    block.clear();

    for (int i = 0; i < 100; ++i)
        engine.pushAudio (block);

    PPM_CHECK_EQ (engine.getNumClients(), 0);
    PPM_CHECK (engine.getListenUrl().isEmpty());
    PPM_CHECK (! engine.isRunning());
}
