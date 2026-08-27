#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace ppm
{

/** Renders a URL as a QR code.

    Discovery is a QR code and nothing else: no mDNS, no UDP beacon. That is not a
    shortcut, it is the reason the plugin never triggers macOS's Local Network permission
    prompt — Apple's rules exempt "listening for and accepting incoming TCP connections"
    but cover every form of discovery. A camera pointed at the screen is also faster than
    any protocol.
*/
class QrCodeComponent final : public juce::Component
{
public:
    QrCodeComponent();
    ~QrCodeComponent() override;

    /** Sets the text to encode. An empty string clears the display. */
    void setText (const juce::String& text);

    void paint (juce::Graphics&) override;

private:
    juce::String encoded;
    juce::Image image;

    void rebuildImage();

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (QrCodeComponent)
};

} // namespace ppm
