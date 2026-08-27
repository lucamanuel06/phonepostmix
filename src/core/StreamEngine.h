#pragma once

#include "core/AudioRingBuffer.h"
#include "core/StreamServer.h"
#include "core/WirePacket.h"

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <vector>

namespace ppm
{

/** Ties the audio thread, the ring buffer, the packetiser and the server together.

    This is the only class the plugin processor talks to. It owns:
      - the ring buffer the audio thread writes into;
      - a streaming thread that drains it, packetises and broadcasts;
      - the HTTP/WebSocket server;
      - the session state the receiver page and the plugin editor both display.

    Everything the audio thread touches is either atomic or the ring buffer. Nothing else
    on this class may be called from `processBlock`.
*/
class StreamEngine final : private juce::Thread,
                           private StreamServer::Listener
{
public:
    struct Settings
    {
        wire::Format format = wire::Format::pcm16;
        int framesPerPacket = 512;
        int preferredPort = 17520;
        int targetLatencyMs = 120;
    };

    /** A connected listener, as far as the plugin editor is concerned. */
    struct ClientInfo
    {
        int id = 0;
        juce::String userAgent;
        juce::String audioPath;      ///< "worklet" or "spn", as reported by the page
        int bufferMs = 0;
        int targetMs = 0;
        int underruns = 0;
        double driftPpm = 0.0;
        bool playing = false;
    };

    StreamEngine();
    ~StreamEngine() override;

    //== Message thread ==========================================================
    /** Binds a port and starts accepting listeners. Returns false if no port was free. */
    bool start();

    /** Stops the server, disconnects listeners and joins the streaming thread. */
    void stop();

    bool isRunning() const noexcept { return serverRunning.load(); }

    /** Applies new settings. Safe to call while running; bumps the config epoch. */
    void setSettings (const Settings& newSettings);
    Settings getSettings() const;

    /** The URL to put in the QR code, including the pairing token fragment. */
    juce::String getListenUrl() const;

    /** Every IPv4 address the machine has, best guess first. Empty while stopped. */
    juce::StringArray getCandidateAddresses() const;

    /** Chooses which of `getCandidateAddresses` the URL should use. */
    void setPreferredAddress (const juce::String& address);
    juce::String getPreferredAddress() const;

    int getPort() const noexcept          { return server.getPort(); }
    int getNumClients() const             { return server.getNumClients(); }
    int getOverrunCount() const noexcept  { return ring.getOverrunCount(); }
    int getDropCount() const noexcept     { return server.getDropCount(); }

    std::vector<ClientInfo> getClients() const;

    /** Peak level of the last audio seen, per channel, for the editor's meter. */
    float getPeakLevel (int channel) const noexcept;

    //== Audio thread ============================================================
    /** Allocates buffers for the given format. Called from `prepareToPlay`. */
    void prepare (double sampleRate, int maximumBlockSize, int numChannels);

    /** Frees buffers. Called from `releaseResources`. */
    void release();

    /** Copies a block out to the network. Real-time safe; never blocks. */
    void pushAudio (const juce::AudioBuffer<float>& buffer) noexcept;

    /** Tells the receiver whether the host's transport is rolling.

        Real-time safe: it stores a flag. The streaming thread notices the change and is
        the one that tells the listeners, because broadcasting takes a lock.
    */
    void setHostPlaying (bool isPlaying) noexcept { hostPlaying.store (isPlaying, std::memory_order_relaxed); }

private:
    void run() override;

    void streamClientConnected (int clientId) override;
    void streamClientDisconnected (int clientId) override;
    void streamTextMessageReceived (int clientId, const std::string& message) override;

    std::optional<StreamServer::Asset> resolveAsset (const std::string& path) const;
    bool authorizeUpgrade (const std::string& target) const;

    juce::String makeSessionJson (const juce::String& type) const;
    void broadcastConfig();
    void refreshAddresses();

    //== Owned by the message thread =============================================
    StreamServer server;
    juce::String token;
    juce::String preferredAddress;
    juce::StringArray addresses;
    mutable std::mutex stateMutex;
    std::vector<ClientInfo> clients;
    Settings settings;

    //== Shared with the audio thread ============================================
    AudioRingBuffer ring;
    std::atomic<double> currentSampleRate { 48000.0 };
    std::atomic<int> currentChannels { 2 };
    std::atomic<uint16_t> configEpoch { 0 };
    std::atomic<bool> serverRunning { false };
    std::atomic<bool> hostPlaying { false };
    std::atomic<bool> discontinuityPending { true };
    std::atomic<float> peakLevels[2] { { 0.0f }, { 0.0f } };

    //== Owned by the streaming thread ===========================================
    std::vector<float> scratch;
    std::vector<uint8_t> packet;
    uint32_t sequence = 0;
    uint64_t sampleClock = 0;
    uint32_t lastPacketTimeMs = 0;
    bool lastHostPlaying = false;

    void sendPacket (const wire::PacketInfo& info, const float* interleaved);

    JUCE_DECLARE_NON_COPYABLE (StreamEngine)
};

} // namespace ppm
