#include "plugin/PluginEditor.h"

namespace ppm
{

namespace
{

const juce::Colour background   { 0xff14161a };
const juce::Colour panel        { 0xff1c2027 };
const juce::Colour line         { 0xff2a303a };
const juce::Colour textColour   { 0xffe8ecf2 };
const juce::Colour mutedColour  { 0xff8a94a6 };
const juce::Colour accentColour { 0xff46d19a };
const juce::Colour hotColour    { 0xffe5484d };

constexpr int refreshHz = 15;

void styleCaption (juce::Label& label)
{
    label.setFont (juce::FontOptions (11.0f));
    label.setColour (juce::Label::textColourId, mutedColour);
    label.setJustificationType (juce::Justification::centredLeft);
}

void styleBox (juce::ComboBox& box)
{
    box.setColour (juce::ComboBox::backgroundColourId, panel);
    box.setColour (juce::ComboBox::outlineColourId, line);
    box.setColour (juce::ComboBox::textColourId, textColour);
    box.setColour (juce::ComboBox::arrowColourId, mutedColour);
}

/** Maps a peak level to a bar width, roughly -60 dBFS to 0 dBFS. */
float meterProportion (float level)
{
    if (level <= 0.0f)
        return 0.0f;

    const auto db = juce::Decibels::gainToDecibels (level, -60.0f);
    return juce::jlimit (0.0f, 1.0f, (db + 60.0f) / 60.0f);
}

} // namespace

//==============================================================================
PhonePostMixEditor::PhonePostMixEditor (PhonePostMixProcessor& p)
    : juce::AudioProcessorEditor (&p), processorRef (p)
{
    addAndMakeVisible (startButton);
    startButton.setColour (juce::TextButton::buttonColourId, accentColour);
    startButton.setColour (juce::TextButton::textColourOffId, background);
    startButton.onClick = [this]
    {
        if (processorRef.getEngine().isRunning())
            processorRef.stopStreaming();
        else
            processorRef.startStreaming();

        refreshFromEngine();
    };

    addAndMakeVisible (copyUrlButton);
    copyUrlButton.setColour (juce::TextButton::buttonColourId, panel);
    copyUrlButton.setColour (juce::TextButton::textColourOffId, textColour);
    copyUrlButton.onClick = [this]
    {
        if (lastUrl.isNotEmpty())
            juce::SystemClipboard::copyTextToClipboard (lastUrl);
    };

    addAndMakeVisible (qrCode);

    addAndMakeVisible (urlLabel);
    urlLabel.setFont (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 12.0f, juce::Font::plain));
    urlLabel.setColour (juce::Label::textColourId, textColour);
    urlLabel.setJustificationType (juce::Justification::centred);

    addAndMakeVisible (statusLabel);
    statusLabel.setFont (juce::FontOptions (11.0f));
    statusLabel.setColour (juce::Label::textColourId, mutedColour);
    statusLabel.setJustificationType (juce::Justification::centred);

    addAndMakeVisible (clientsLabel);
    clientsLabel.setFont (juce::FontOptions (11.0f));
    clientsLabel.setColour (juce::Label::textColourId, mutedColour);
    clientsLabel.setJustificationType (juce::Justification::topLeft);

    for (auto* caption : { &addressCaption, &formatCaption, &packetCaption, &portCaption })
    {
        addAndMakeVisible (*caption);
        styleCaption (*caption);
    }

    addAndMakeVisible (portValue);
    portValue.setFont (juce::FontOptions (12.0f));
    portValue.setColour (juce::Label::textColourId, textColour);

    addAndMakeVisible (addressBox);
    styleBox (addressBox);
    addressBox.onChange = [this]
    {
        processorRef.getEngine().setPreferredAddress (addressBox.getText());
        refreshFromEngine();
    };

    addAndMakeVisible (formatBox);
    styleBox (formatBox);
    formatBox.addItem ("PCM 16-bit  (1.5 Mbit/s)", 1);
    formatBox.addItem ("PCM 24-bit  (2.3 Mbit/s)", 2);
    formatBox.addItem ("Float 32-bit  (3.1 Mbit/s)", 3);
    formatBox.onChange = [this] { pushSettingsToEngine(); };

    addAndMakeVisible (packetBox);
    styleBox (packetBox);
    packetBox.addItem ("256 frames", 1);
    packetBox.addItem ("512 frames", 2);
    packetBox.addItem ("1024 frames", 3);
    packetBox.onChange = [this] { pushSettingsToEngine(); };

    const auto settings = processorRef.getEngine().getSettings();
    formatBox.setSelectedId (static_cast<int> (settings.format) + 1, juce::dontSendNotification);
    packetBox.setSelectedId (settings.framesPerPacket == 256 ? 1 : settings.framesPerPacket == 1024 ? 3 : 2,
                             juce::dontSendNotification);

    setSize (520, 560);
    refreshFromEngine();
    startTimerHz (refreshHz);
}

PhonePostMixEditor::~PhonePostMixEditor() = default;

//==============================================================================
void PhonePostMixEditor::pushSettingsToEngine()
{
    auto settings = processorRef.getEngine().getSettings();

    settings.format = static_cast<wire::Format> (juce::jlimit (0, 2, formatBox.getSelectedId() - 1));
    settings.framesPerPacket = packetBox.getSelectedId() == 1 ? 256
                             : packetBox.getSelectedId() == 3 ? 1024 : 512;

    processorRef.getEngine().setSettings (settings);
}

void PhonePostMixEditor::updateStartButton()
{
    const auto running = processorRef.getEngine().isRunning();

    startButton.setButtonText (running ? "STOP STREAMING" : "START STREAMING");
    startButton.setColour (juce::TextButton::buttonColourId, running ? panel : accentColour);
    startButton.setColour (juce::TextButton::textColourOffId, running ? textColour : background);
}

juce::String PhonePostMixEditor::describeStatus() const
{
    auto& engine = processorRef.getEngine();

    if (processorRef.didStartFail())
        return "Could not bind a port. Another application may be using the whole range.";

    if (! engine.isRunning())
        return "Not streaming. Press start, then scan the code with your phone.";

    if (engine.getCandidateAddresses().isEmpty())
        return "No usable network address. Are you connected to Wi-Fi or Ethernet?";

    const auto clients = engine.getNumClients();
    const auto overruns = engine.getOverrunCount();
    const auto drops = engine.getDropCount();

    juce::String status;
    status << (clients == 0 ? juce::String ("Waiting for a listener")
                            : juce::String (clients) + (clients == 1 ? " listener" : " listeners"));

    if (overruns > 0 || drops > 0)
        status << "  ·  " << overruns << " buffer overruns, " << drops << " frames dropped";

    return status;
}

void PhonePostMixEditor::refreshFromEngine()
{
    auto& engine = processorRef.getEngine();

    const auto addresses = engine.getCandidateAddresses();

    if (addresses != lastAddresses)
    {
        lastAddresses = addresses;
        addressBox.clear (juce::dontSendNotification);

        for (int i = 0; i < addresses.size(); ++i)
            addressBox.addItem (addresses[i], i + 1);

        const auto preferred = addresses.indexOf (engine.getPreferredAddress());
        addressBox.setSelectedId (juce::jmax (0, preferred) + 1, juce::dontSendNotification);
    }

    const auto url = engine.getListenUrl();

    if (url != lastUrl)
    {
        lastUrl = url;
        qrCode.setText (url);
        urlLabel.setText (url.isEmpty() ? "—" : url.upToFirstOccurrenceOf ("#", false, false),
                          juce::dontSendNotification);
    }

    portValue.setText (engine.isRunning() ? juce::String (engine.getPort()) : juce::String ("—"),
                       juce::dontSendNotification);

    statusLabel.setText (describeStatus(), juce::dontSendNotification);

    juce::StringArray rows;

    for (const auto& client : engine.getClients())
    {
        juce::String row;
        row << (client.playing ? "▶ " : "· ");
        row << (client.audioPath.isEmpty() ? juce::String ("connecting") : client.audioPath);

        if (client.bufferMs > 0)
            row << "  ·  buffer " << client.bufferMs << " / " << client.targetMs << " ms";

        if (client.underruns > 0)
            row << "  ·  " << client.underruns << " dropouts";

        rows.add (row);
    }

    clientsLabel.setText (rows.joinIntoString ("\n"), juce::dontSendNotification);

    copyUrlButton.setEnabled (url.isNotEmpty());
    updateStartButton();
}

void PhonePostMixEditor::timerCallback()
{
    for (int channel = 0; channel < 2; ++channel)
        meterLevels[channel] = processorRef.getEngine().getPeakLevel (channel);

    refreshFromEngine();
    repaint();
}

//==============================================================================
void PhonePostMixEditor::paint (juce::Graphics& g)
{
    g.fillAll (background);

    auto bounds = getLocalBounds().reduced (18);

    auto header = bounds.removeFromTop (34);
    g.setColour (textColour);
    g.setFont (juce::FontOptions (17.0f, juce::Font::bold));
    g.drawText ("PhonePostMix", header, juce::Justification::centredLeft, false);

    g.setColour (mutedColour);
    g.setFont (juce::FontOptions (11.0f));
    g.drawText (juce::String ("v") + PPM_VERSION, header, juce::Justification::centredRight, false);

    // Meters, drawn rather than made of components: two rectangles per frame is cheaper
    // than laying out child components fifteen times a second.
    const auto meterArea = getLocalBounds().reduced (18).removeFromBottom (108).removeFromTop (30);

    for (int channel = 0; channel < 2; ++channel)
    {
        const auto row = meterArea.withHeight (10).withY (meterArea.getY() + channel * 14);

        g.setColour (panel);
        g.fillRoundedRectangle (row.toFloat(), 4.0f);

        const auto level = meterProportion (meterLevels[channel]);

        if (level > 0.0f)
        {
            g.setColour (meterLevels[channel] > 0.99f ? hotColour : accentColour);
            g.fillRoundedRectangle (row.withWidth (juce::roundToInt (static_cast<float> (row.getWidth()) * level)).toFloat(), 4.0f);
        }
    }
}

void PhonePostMixEditor::resized()
{
    auto bounds = getLocalBounds().reduced (18);
    bounds.removeFromTop (34 + 10);

    startButton.setBounds (bounds.removeFromTop (46));
    bounds.removeFromTop (12);

    auto qrRow = bounds.removeFromTop (176);
    qrCode.setBounds (qrRow.removeFromLeft (176));
    qrRow.removeFromLeft (14);

    urlLabel.setBounds (qrRow.removeFromTop (40));
    copyUrlButton.setBounds (qrRow.removeFromTop (28).removeFromLeft (110));
    qrRow.removeFromTop (8);
    clientsLabel.setBounds (qrRow);

    bounds.removeFromTop (10);
    statusLabel.setBounds (bounds.removeFromTop (32));
    bounds.removeFromTop (6);

    auto controlRow = [&bounds] (juce::Label& caption, juce::Component& control, int captionWidth)
    {
        auto row = bounds.removeFromTop (26);
        caption.setBounds (row.removeFromLeft (captionWidth));
        control.setBounds (row);
        bounds.removeFromTop (6);
    };

    controlRow (addressCaption, addressBox, 70);
    controlRow (formatCaption, formatBox, 70);
    controlRow (packetCaption, packetBox, 70);

    auto portRow = bounds.removeFromTop (22);
    portCaption.setBounds (portRow.removeFromLeft (70));
    portValue.setBounds (portRow);
}

} // namespace ppm
