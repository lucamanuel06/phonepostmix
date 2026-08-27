#pragma once

#include "plugin/PluginProcessor.h"
#include "plugin/QrCodeComponent.h"

#include <juce_audio_utils/juce_audio_utils.h>

namespace ppm
{

/** The plugin's window.

    One screen, and its job is to answer one question: what do I point my phone at? The
    QR code and the URL are the two largest things on it. Everything else is either a
    setting that changes the stream or a number that explains why it is not working.
*/
class PhonePostMixEditor final : public juce::AudioProcessorEditor,
                                 private juce::Timer
{
public:
    explicit PhonePostMixEditor (PhonePostMixProcessor&);
    ~PhonePostMixEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;

    void refreshFromEngine();
    void pushSettingsToEngine();
    void updateStartButton();
    juce::String describeStatus() const;

    PhonePostMixProcessor& processorRef;

    juce::TextButton startButton;
    juce::TextButton copyUrlButton { "Copy link" };
    QrCodeComponent qrCode;

    juce::Label urlLabel;
    juce::Label statusLabel;
    juce::Label clientsLabel;

    juce::Label addressCaption { {}, "Network" };
    juce::ComboBox addressBox;

    juce::Label formatCaption { {}, "Quality" };
    juce::ComboBox formatBox;

    juce::Label packetCaption { {}, "Packet" };
    juce::ComboBox packetBox;

    juce::Label portCaption { {}, "Port" };
    juce::Label portValue;

    float meterLevels[2] { 0.0f, 0.0f };
    juce::String lastUrl;
    juce::StringArray lastAddresses;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PhonePostMixEditor)
};

} // namespace ppm
