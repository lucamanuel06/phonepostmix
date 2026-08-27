#include "core/StreamEngine.h"

#include "core/HttpMessage.h"

#include <BinaryData.h>

#include <algorithm>

namespace ppm
{

namespace
{

/** How long the streaming thread sleeps when the ring has less than a packet ready.

    Short enough that a 512-frame packet at 48 kHz (10.7 ms) is never delayed noticeably,
    long enough that the thread is not a busy loop. The audio thread deliberately does not
    signal this thread: waking a WaitableEvent is a system call, and system calls are not
    allowed in processBlock.
*/
constexpr int pollIntervalMs = 2;

/** Peak meters decay by this factor per block, so the editor's bars fall smoothly. */
constexpr float meterDecay = 0.85f;

juce::String makeToken()
{
    // Not a secret worth protecting cryptographically — it exists so that someone else on
    // the same Wi-Fi who guesses the port gets the page but not the audio. 128 bits of
    // JUCE's Random is far beyond what that threat deserves.
    juce::Random random (juce::Time::getHighResolutionTicks());

    juce::String out;
    for (int i = 0; i < 4; ++i)
        out << juce::String::toHexString (random.nextInt());

    return out.removeCharacters (" ");
}

/** True for addresses that are useless as a "point your phone here" target. */
bool isUsefulAddress (const juce::IPAddress& address)
{
    if (address.isIPv6 || address.isNull())
        return false;

    const auto text = address.toString();

    // 127.x is the machine itself; 169.254.x is a link-local address handed out when DHCP
    // failed, which means the network is broken and the phone will never reach it.
    return ! text.startsWith ("127.") && ! text.startsWith ("169.254.");
}

/** Ranks addresses so the one a phone is most likely to reach comes first. */
int addressPriority (const juce::String& address)
{
    if (address.startsWith ("192.168."))                  return 0;   // home and studio Wi-Fi
    if (address.startsWith ("10."))                       return 1;   // larger networks, and some VPNs
    if (address.startsWith ("172."))                      return 2;   // often a Docker or VM bridge
    return 3;
}

} // namespace

//==============================================================================
StreamEngine::StreamEngine()
    : juce::Thread ("PhonePostMix streamer")
{
    token = makeToken();
}

StreamEngine::~StreamEngine()
{
    stop();
}

//==============================================================================
bool StreamEngine::start()
{
    if (serverRunning.load())
        return true;

    refreshAddresses();

    server.setUpgradeAuthorizer ([this] (const std::string& target) { return authorizeUpgrade (target); });

    const auto started = server.start (getSettings().preferredPort,
                                       [this] (const std::string& path) { return resolveAsset (path); },
                                       this);

    if (! started)
        return false;

    serverRunning.store (true);
    discontinuityPending.store (true);
    startThread (juce::Thread::Priority::high);

    return true;
}

void StreamEngine::stop()
{
    if (! serverRunning.exchange (false))
        return;

    // The streaming thread only touches the server through broadcast calls, so it has to
    // be gone before the server is torn down.
    signalThreadShouldExit();
    stopThread (2000);
    server.stop();

    std::lock_guard<std::mutex> lock (stateMutex);
    clients.clear();
}

void StreamEngine::setSettings (const Settings& newSettings)
{
    bool formatChanged = false;

    {
        std::lock_guard<std::mutex> lock (stateMutex);

        formatChanged = newSettings.format != settings.format
                     || newSettings.framesPerPacket != settings.framesPerPacket;

        settings = newSettings;
    }

    if (formatChanged)
    {
        configEpoch.fetch_add (1);
        discontinuityPending.store (true);
        broadcastConfig();
    }
}

StreamEngine::Settings StreamEngine::getSettings() const
{
    std::lock_guard<std::mutex> lock (stateMutex);
    return settings;
}

juce::String StreamEngine::getListenUrl() const
{
    const auto address = getPreferredAddress();

    if (address.isEmpty() || ! serverRunning.load())
        return {};

    // The token goes in the fragment so it never reaches a server log or a Referer
    // header; the page reads it and puts it on the WebSocket query string itself.
    return "http://" + address + ":" + juce::String (server.getPort()) + "/#t=" + token;
}

juce::StringArray StreamEngine::getCandidateAddresses() const
{
    std::lock_guard<std::mutex> lock (stateMutex);
    return addresses;
}

void StreamEngine::setPreferredAddress (const juce::String& address)
{
    std::lock_guard<std::mutex> lock (stateMutex);
    preferredAddress = address;
}

juce::String StreamEngine::getPreferredAddress() const
{
    std::lock_guard<std::mutex> lock (stateMutex);

    if (preferredAddress.isNotEmpty() && addresses.contains (preferredAddress))
        return preferredAddress;

    return addresses.isEmpty() ? juce::String() : addresses[0];
}

std::vector<StreamEngine::ClientInfo> StreamEngine::getClients() const
{
    std::lock_guard<std::mutex> lock (stateMutex);
    return clients;
}

float StreamEngine::getPeakLevel (int channel) const noexcept
{
    return channel >= 0 && channel < 2 ? peakLevels[channel].load (std::memory_order_relaxed) : 0.0f;
}

void StreamEngine::refreshAddresses()
{
    juce::Array<juce::IPAddress> all;
    juce::IPAddress::findAllAddresses (all, false);

    juce::StringArray found;

    for (const auto& address : all)
        if (isUsefulAddress (address))
            found.addIfNotAlreadyThere (address.toString());

    juce::StringArray sorted (found);
    std::stable_sort (sorted.strings.begin(), sorted.strings.end(),
                      [] (const juce::String& a, const juce::String& b)
                      { return addressPriority (a) < addressPriority (b); });

    std::lock_guard<std::mutex> lock (stateMutex);
    addresses = sorted;
}

//==============================================================================
void StreamEngine::prepare (double sampleRate, int maximumBlockSize, int numChannels)
{
    currentSampleRate.store (sampleRate);
    currentChannels.store (juce::jlimit (1, 2, numChannels));

    // Half a second of headroom, and never less than four host blocks, so a host that
    // hands out unusually large buffers cannot overrun the ring on the very first call.
    const auto capacity = juce::jmax (static_cast<int> (sampleRate * 0.5),
                                      maximumBlockSize * 4,
                                      8192);

    ring.prepare (currentChannels.load(), capacity);

    configEpoch.fetch_add (1);
    discontinuityPending.store (true);
    broadcastConfig();
}

void StreamEngine::release()
{
    ring.release();
}

void StreamEngine::pushAudio (const juce::AudioBuffer<float>& buffer) noexcept
{
    if (! serverRunning.load (std::memory_order_relaxed))
        return;

    const auto numChannels = juce::jmin (2, buffer.getNumChannels());

    for (int channel = 0; channel < 2; ++channel)
    {
        const auto source = juce::jmin (channel, numChannels - 1);
        const auto magnitude = source >= 0 ? buffer.getMagnitude (source, 0, buffer.getNumSamples()) : 0.0f;
        const auto previous = peakLevels[channel].load (std::memory_order_relaxed) * meterDecay;

        peakLevels[channel].store (juce::jmax (magnitude, previous), std::memory_order_relaxed);
    }

    ring.write (buffer);
}

//==============================================================================
void StreamEngine::run()
{
    while (! threadShouldExit())
    {
        const auto active = getSettings();
        const auto channels = static_cast<uint8_t> (currentChannels.load());
        const auto frames = static_cast<uint16_t> (active.framesPerPacket);

        if (ring.getNumReady() < frames || ring.getNumChannels() <= 0)
        {
            wait (pollIntervalMs);
            continue;
        }

        const auto numSamples = static_cast<int> (frames) * static_cast<int> (channels);

        if (static_cast<int> (scratch.size()) < numSamples)
            scratch.resize (static_cast<size_t> (numSamples));

        const auto framesRead = ring.read (scratch.data(), frames);

        if (framesRead < frames)
            continue;   // another thread drained it between the check and the read

        wire::PacketInfo info;
        info.format       = active.format;
        info.channels     = channels;
        info.sequence     = sequence++;
        info.sampleRate   = static_cast<uint32_t> (currentSampleRate.load());
        info.frames       = frames;
        info.configEpoch  = configEpoch.load();
        info.senderTimeMs = static_cast<uint32_t> (juce::Time::getMillisecondCounter());

        if (discontinuityPending.exchange (false))
        {
            sampleClock = 0;
            info.flags |= wire::Flags::discontinuity | wire::Flags::configChanged;
        }

        info.sampleClock = sampleClock;
        sampleClock += frames;

        if (! hostPlaying.load())
            info.flags |= wire::Flags::silence;

        const auto size = static_cast<size_t> (wire::packetSize (info));

        if (packet.size() != size)
            packet.resize (size);

        wire::writeHeader (packet.data(), info);
        wire::writeSamples (packet.data() + wire::headerBytes, scratch.data(), numSamples, info.format);

        server.broadcastBinary (packet.data(), packet.size());
    }
}

//==============================================================================
std::optional<StreamServer::Asset> StreamEngine::resolveAsset (const std::string& path) const
{
    const auto file = path == "/" ? std::string ("index.html") : path.substr (1);

    for (int i = 0; i < BinaryData::namedResourceListSize; ++i)
    {
        if (file != BinaryData::originalFilenames[i])
            continue;

        int size = 0;

        if (const auto* data = BinaryData::getNamedResource (BinaryData::namedResourceList[i], size))
            return StreamServer::Asset { data, size, http::mimeTypeForPath (file) };
    }

    return std::nullopt;
}

bool StreamEngine::authorizeUpgrade (const std::string& target) const
{
    const juce::String text (target);
    const auto query = text.fromFirstOccurrenceOf ("?", false, false);

    for (const auto& pair : juce::StringArray::fromTokens (query, "&", ""))
        if (pair.upToFirstOccurrenceOf ("=", false, false) == "t")
            return pair.fromFirstOccurrenceOf ("=", false, false) == token;

    return false;
}

//==============================================================================
juce::String StreamEngine::makeSessionJson (const juce::String& type) const
{
    const auto active = getSettings();

    auto* object = new juce::DynamicObject();
    object->setProperty ("type", type);
    object->setProperty ("protocol", static_cast<int> (wire::protocolVersion));
    object->setProperty ("sender", juce::String ("PhonePostMix ") + PPM_VERSION);
    object->setProperty ("headerBytes", wire::headerBytes);
    object->setProperty ("format", juce::String (wire::formatName (active.format)));
    object->setProperty ("sampleRate", static_cast<int> (currentSampleRate.load()));
    object->setProperty ("channels", currentChannels.load());
    object->setProperty ("framesPerPacket", active.framesPerPacket);
    object->setProperty ("configEpoch", static_cast<int> (configEpoch.load()));
    object->setProperty ("suggestedTargetLatencyMs", active.targetLatencyMs);
    object->setProperty ("hostPlaying", hostPlaying.load());

    return juce::JSON::toString (juce::var (object), true);
}

void StreamEngine::broadcastConfig()
{
    if (serverRunning.load())
        server.broadcastText (makeSessionJson ("config").toStdString());
}

//==============================================================================
void StreamEngine::streamClientConnected (int clientId)
{
    {
        std::lock_guard<std::mutex> lock (stateMutex);
        clients.push_back (ClientInfo { clientId, {}, {}, 0, 0, 0, 0.0, false });
    }

    server.broadcastText (makeSessionJson ("hello").toStdString());
}

void StreamEngine::streamClientDisconnected (int clientId)
{
    std::lock_guard<std::mutex> lock (stateMutex);

    clients.erase (std::remove_if (clients.begin(), clients.end(),
                                   [clientId] (const ClientInfo& c) { return c.id == clientId; }),
                   clients.end());
}

void StreamEngine::streamTextMessageReceived (int clientId, const std::string& message)
{
    const auto parsed = juce::JSON::parse (juce::String (message));

    if (! parsed.isObject())
        return;

    const auto type = parsed.getProperty ("type", {}).toString();

    std::lock_guard<std::mutex> lock (stateMutex);

    const auto entry = std::find_if (clients.begin(), clients.end(),
                                     [clientId] (const ClientInfo& c) { return c.id == clientId; });

    if (entry == clients.end())
        return;

    // Unknown message types and unknown keys are ignored rather than rejected. That is
    // the whole forward-compatibility story for this protocol, and it costs nothing.
    if (type == "ready")
    {
        entry->userAgent = parsed.getProperty ("ua", {}).toString();
        entry->audioPath = parsed.getProperty ("path", {}).toString();
    }
    else if (type == "stat")
    {
        entry->bufferMs  = static_cast<int> (parsed.getProperty ("bufferMs", 0));
        entry->targetMs  = static_cast<int> (parsed.getProperty ("targetMs", 0));
        entry->underruns = static_cast<int> (parsed.getProperty ("underruns", 0));
        entry->driftPpm  = static_cast<double> (parsed.getProperty ("driftPpm", 0.0));
        entry->playing   = static_cast<bool> (parsed.getProperty ("playing", false));
    }
}

} // namespace ppm
