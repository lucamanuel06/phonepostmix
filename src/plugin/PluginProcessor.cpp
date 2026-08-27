#include "plugin/PluginProcessor.h"
#include "plugin/PluginEditor.h"

namespace ppm
{

namespace
{
constexpr const char* stateTag = "PhonePostMixState";
}

PhonePostMixProcessor::PhonePostMixProcessor()
    : juce::AudioProcessor (BusesProperties()
                                .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                                .withOutput ("Output", juce::AudioChannelSet::stereo(), true))
{
    // The server is deliberately NOT started here. Hosts instantiate a plugin many times
    // while scanning, and validators such as auval and pluginval instantiate it hundreds
    // of times; binding a port in the constructor would mean a port conflict storm, a
    // firewall prompt during a scan, and a listening socket for every plugin the user has
    // never opened. Streaming starts when the user asks for it.
}

PhonePostMixProcessor::~PhonePostMixProcessor()
{
    engine.stop();
}

void PhonePostMixProcessor::prepareToPlay (double sampleRate, int maximumExpectedSamplesPerBlock)
{
    engine.prepare (sampleRate, maximumExpectedSamplesPerBlock, getTotalNumInputChannels());
}

void PhonePostMixProcessor::releaseResources()
{
    engine.release();
}

bool PhonePostMixProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    // Mono and stereo only, and the output layout must match the input: we are a
    // pass-through, so we cannot invent or discard channels.
    const auto& out = layouts.getMainOutputChannelSet();

    if (out != juce::AudioChannelSet::mono() && out != juce::AudioChannelSet::stereo())
        return false;

    return layouts.getMainInputChannelSet() == out;
}

void PhonePostMixProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    // Clear any output channel the host gave us that has no corresponding input, so we
    // never pass uninitialised memory downstream.
    for (auto i = getTotalNumInputChannels(); i < getTotalNumOutputChannels(); ++i)
        buffer.clear (i, 0, buffer.getNumSamples());

    // During an offline bounce the host runs faster than real time, so streaming would
    // fire thousands of packets a second at a listener who cannot possibly use them.
    // Nobody monitors a render on their phone.
    if (! isNonRealtime())
    {
        if (auto* playHead = getPlayHead())
            if (const auto position = playHead->getPosition())
                engine.setHostPlaying (position->getIsPlaying());

        engine.pushAudio (buffer);
    }

    // Audio is deliberately untouched.
}

juce::AudioProcessorEditor* PhonePostMixProcessor::createEditor()
{
    return new PhonePostMixEditor (*this);
}

bool PhonePostMixProcessor::startStreaming()
{
    startFailed = ! engine.start();
    return ! startFailed;
}

void PhonePostMixProcessor::stopStreaming()
{
    engine.stop();
    startFailed = false;
}

void PhonePostMixProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    const auto settings = engine.getSettings();

    juce::ValueTree state (stateTag);
    state.setProperty ("format", static_cast<int> (settings.format), nullptr);
    state.setProperty ("framesPerPacket", settings.framesPerPacket, nullptr);
    state.setProperty ("preferredPort", settings.preferredPort, nullptr);
    state.setProperty ("targetLatencyMs", settings.targetLatencyMs, nullptr);
    state.setProperty ("address", engine.getPreferredAddress(), nullptr);
    state.setProperty ("streaming", engine.isRunning(), nullptr);

    juce::MemoryOutputStream stream (destData, false);
    state.writeToStream (stream);
}

void PhonePostMixProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    const auto state = juce::ValueTree::readFromData (data, static_cast<size_t> (sizeInBytes));

    if (! state.hasType (stateTag))
        return;

    StreamEngine::Settings settings;
    settings.format          = static_cast<wire::Format> (static_cast<int> (state.getProperty ("format", 0)));
    settings.framesPerPacket = state.getProperty ("framesPerPacket", 512);
    settings.preferredPort   = state.getProperty ("preferredPort", 17520);
    settings.targetLatencyMs = state.getProperty ("targetLatencyMs", 120);

    engine.setSettings (settings);
    engine.setPreferredAddress (state.getProperty ("address", juce::String()).toString());

    // Reopening a project that was streaming should stream again — but only from the
    // message thread, and only once the host is done restoring state.
    if (static_cast<bool> (state.getProperty ("streaming", false)))
        juce::MessageManager::callAsync ([this, alive = std::weak_ptr<int> (lifetime)]
                                         {
                                             if (! alive.expired())
                                                 startStreaming();
                                         });
}

} // namespace ppm

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new ppm::PhonePostMixProcessor();
}
