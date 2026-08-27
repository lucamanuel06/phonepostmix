#pragma once

#include "core/StreamEngine.h"

#include <juce_audio_processors/juce_audio_processors.h>

namespace ppm
{

/** The PhonePostMix audio processor.

    PhonePostMix is a monitoring tool, not an effect: it never alters the signal. Audio
    arrives on the master bus, is copied out to whatever is listening on the network, and
    is passed through to the host bit-for-bit unchanged.
*/
class PhonePostMixProcessor final : public juce::AudioProcessor
{
public:
    PhonePostMixProcessor();
    ~PhonePostMixProcessor() override;

    void prepareToPlay (double sampleRate, int maximumExpectedSamplesPerBlock) override;
    void releaseResources() override;
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    // Keeps the double-precision overload visible. Without this GCC warns that declaring
    // only the float version hides the base class's AudioBuffer<double> overload.
    using juce::AudioProcessor::processBlock;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override                          { return true; }

    const juce::String getName() const override              { return JucePlugin_Name; }
    bool acceptsMidi() const override                        { return false; }
    bool producesMidi() const override                       { return false; }
    bool isMidiEffect() const override                       { return false; }
    double getTailLengthSeconds() const override             { return 0.0; }

    int getNumPrograms() override                            { return 1; }
    int getCurrentProgram() override                         { return 0; }
    void setCurrentProgram (int) override                    {}
    const juce::String getProgramName (int) override         { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    //== Used by the editor ======================================================
    StreamEngine& getEngine() noexcept { return engine; }

    /** Starts the server. Returns false if every port in the search range was taken. */
    bool startStreaming();
    void stopStreaming();

    /** True once a start attempt has failed, so the editor can explain itself. */
    bool didStartFail() const noexcept { return startFailed; }

private:
    StreamEngine engine;
    bool startFailed = false;

    /** Lifetime token for deferred message-thread work.

        setStateInformation has to defer the auto-start, and a host may well destroy the
        processor before that message runs. Capturing a weak_ptr to this is the cheapest
        way to make the deferred call a no-op instead of a use-after-free.
    */
    std::shared_ptr<int> lifetime { std::make_shared<int> (0) };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PhonePostMixProcessor)
};

} // namespace ppm
