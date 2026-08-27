#pragma once

#include "plugin/PluginProcessor.h"

namespace ppm
{

class PhonePostMixEditor final : public juce::AudioProcessorEditor
{
public:
    explicit PhonePostMixEditor (PhonePostMixProcessor&);
    ~PhonePostMixEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    PhonePostMixProcessor& processorRef;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PhonePostMixEditor)
};

} // namespace ppm
