#include "plugin/PluginProcessor.h"
#include "plugin/PluginEditor.h"

namespace ppm
{

PhonePostMixProcessor::PhonePostMixProcessor()
    : juce::AudioProcessor (BusesProperties()
                                .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                                .withOutput ("Output", juce::AudioChannelSet::stereo(), true))
{
}

PhonePostMixProcessor::~PhonePostMixProcessor() = default;

void PhonePostMixProcessor::prepareToPlay (double, int)
{
}

void PhonePostMixProcessor::releaseResources()
{
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

    // Audio is deliberately untouched. Capture and streaming are added in a later step.
}

juce::AudioProcessorEditor* PhonePostMixProcessor::createEditor()
{
    return new PhonePostMixEditor (*this);
}

void PhonePostMixProcessor::getStateInformation (juce::MemoryBlock&)
{
}

void PhonePostMixProcessor::setStateInformation (const void*, int)
{
}

} // namespace ppm

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new ppm::PhonePostMixProcessor();
}
