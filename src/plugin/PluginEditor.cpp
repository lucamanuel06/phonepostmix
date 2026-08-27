#include "plugin/PluginEditor.h"

namespace ppm
{

PhonePostMixEditor::PhonePostMixEditor (PhonePostMixProcessor& p)
    : juce::AudioProcessorEditor (&p), processorRef (p)
{
    setSize (480, 320);
}

PhonePostMixEditor::~PhonePostMixEditor() = default;

void PhonePostMixEditor::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff14161a));

    g.setColour (juce::Colours::white);
    g.setFont (juce::FontOptions (22.0f));
    g.drawText ("PhonePostMix", getLocalBounds().removeFromTop (120),
                juce::Justification::centred, false);

    g.setColour (juce::Colour (0xff8a94a6));
    g.setFont (juce::FontOptions (14.0f));
    g.drawText ("Streaming is not wired up yet.",
                getLocalBounds(), juce::Justification::centred, false);
}

void PhonePostMixEditor::resized()
{
}

} // namespace ppm
