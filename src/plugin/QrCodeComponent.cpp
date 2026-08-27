#include "plugin/QrCodeComponent.h"

#include <qrcodegen.hpp>

namespace ppm
{

QrCodeComponent::QrCodeComponent() = default;
QrCodeComponent::~QrCodeComponent() = default;

void QrCodeComponent::setText (const juce::String& text)
{
    if (text == encoded)
        return;

    encoded = text;
    rebuildImage();
    repaint();
}

void QrCodeComponent::rebuildImage()
{
    image = {};

    if (encoded.isEmpty())
        return;

    try
    {
        // Medium error correction: a phone screen is close to a monitor and the code is
        // not going to be printed or scuffed, so the extra redundancy of Q or H would only
        // buy a denser, harder-to-scan code.
        const auto code = qrcodegen::QrCode::encodeText (encoded.toRawUTF8(),
                                                         qrcodegen::QrCode::Ecc::MEDIUM);

        const auto modules = code.getSize();
        constexpr int quietZone = 4;   // required by the QR spec; scanners rely on it
        const auto side = modules + quietZone * 2;

        // Rendered one image pixel per module and scaled up at paint time with no
        // smoothing, so the modules stay hard-edged at any component size.
        image = juce::Image (juce::Image::ARGB, side, side, true);

        for (int y = 0; y < side; ++y)
            for (int x = 0; x < side; ++x)
            {
                const auto inQuietZone = x < quietZone || y < quietZone
                                      || x >= side - quietZone || y >= side - quietZone;
                const auto dark = ! inQuietZone && code.getModule (x - quietZone, y - quietZone);

                image.setPixelAt (x, y, dark ? juce::Colours::black : juce::Colours::white);
            }
    }
    catch (const std::exception&)
    {
        // encodeText throws if the text does not fit in any QR version. A URL never will,
        // but a failure here must not take the editor down with it.
        image = {};
    }
}

void QrCodeComponent::paint (juce::Graphics& g)
{
    if (! image.isValid())
        return;

    const auto square = juce::jmin (getWidth(), getHeight());
    const auto bounds = juce::Rectangle<int> (square, square).withCentre (getLocalBounds().getCentre());

    g.setImageResamplingQuality (juce::Graphics::lowResamplingQuality);
    g.drawImage (image, bounds.toFloat(), juce::RectanglePlacement::stretchToFit);
}

} // namespace ppm
