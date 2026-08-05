/*
  ==============================================================================

    ImportOverlayComponent.cpp
    Vocal Trainer — M3.5

  ==============================================================================
*/

#include "ImportOverlayComponent.h"

namespace vt
{

namespace
{
    constexpr int kPanelWidth  = 460;
    constexpr int kPanelHeight = 176;
}

ImportOverlayComponent::ImportOverlayComponent (Theme& t)
    : theme (t)
{
    titleLabel.setJustificationType (juce::Justification::centredLeft);
    addAndMakeVisible (titleLabel);

    messageLabel.setJustificationType (juce::Justification::topLeft);
    // Progress messages run long ("Separating vocals from the backing", plus a
    // file name). Wrapping beats truncating: the stage IS the reassurance.
    messageLabel.setMinimumHorizontalScale (1.0f);
    addAndMakeVisible (messageLabel);

    bar.setPercentageDisplay (true);
    addAndMakeVisible (bar);

    cancelButton.onClick = [this]
    {
        if (failed)
        {
            if (onDismiss != nullptr)
                onDismiss();
        }
        else if (onCancel != nullptr)
        {
            cancelButton.setEnabled (false);
            cancelButton.setButtonText ("Cancelling...");
            onCancel();
        }
    };

    addAndMakeVisible (cancelButton);

    setInterceptsMouseClicks (true, true);
    applyTheme();
}

void ImportOverlayComponent::applyTheme()
{
    const auto& t = theme.get();

    titleLabel.setColour (juce::Label::textColourId, t.text);
    messageLabel.setColour (juce::Label::textColourId, t.textDim);

    titleLabel.setFont (juce::Font (juce::FontOptions (18.0f, juce::Font::bold)));
    messageLabel.setFont (juce::Font (juce::FontOptions (13.0f)));

    cancelButton.setColour (juce::TextButton::buttonColourId, t.panel);
    cancelButton.setColour (juce::TextButton::textColourOffId, t.text);

    bar.setColour (juce::ProgressBar::backgroundColourId, t.background);
    bar.setColour (juce::ProgressBar::foregroundColourId, t.targetBarActive);

    repaint();
}

void ImportOverlayComponent::beginImport (const juce::String& fileName)
{
    failed = false;
    detailText.clear();
    progress = 0.0;

    titleLabel.setText ("Preparing " + fileName, juce::dontSendNotification);
    messageLabel.setText ("Starting...", juce::dontSendNotification);

    bar.setVisible (true);
    cancelButton.setButtonText ("Cancel");
    cancelButton.setEnabled (true);

    setVisible (true);
    toFront (false);
    resized();
    repaint();
}

void ImportOverlayComponent::setProgress (double zeroToOne, const juce::String& message)
{
    progress = juce::jlimit (0.0, 1.0, zeroToOne);

    if (message.isNotEmpty())
        messageLabel.setText (message, juce::dontSendNotification);
}

void ImportOverlayComponent::showFailure (const juce::String& message,
                                          const juce::String& detail)
{
    failed = true;
    detailText = detail;

    titleLabel.setText ("Import failed", juce::dontSendNotification);

    // The last few lines of a Python traceback say more than the exception
    // message alone, and there is nowhere else for the user to see them.
    auto full = message;

    if (detail.isNotEmpty())
    {
        auto lines = juce::StringArray::fromLines (detail.trim());

        while (lines.size() > 4)
            lines.remove (0);

        full << "\n" << lines.joinIntoString ("\n");
    }

    messageLabel.setText (full, juce::dontSendNotification);

    bar.setVisible (false);
    cancelButton.setButtonText ("Close");
    cancelButton.setEnabled (true);

    setVisible (true);
    toFront (false);
    resized();
    repaint();
}

void ImportOverlayComponent::paint (juce::Graphics& g)
{
    const auto& t = theme.get();

    // Scrim: the highway underneath must read as inactive, not merely covered.
    g.fillAll (t.background.withAlpha (0.88f));

    const auto panel = getLocalBounds()
                         .withSizeKeepingCentre (juce::jmin (kPanelWidth, getWidth() - 24),
                                                 juce::jmin (kPanelHeight, getHeight() - 24))
                         .toFloat();

    g.setColour (t.panel);
    g.fillRoundedRectangle (panel, 6.0f);

    g.setColour (failed ? t.traceOutOfTune : t.gridLine);
    g.drawRoundedRectangle (panel, 6.0f, 1.0f);
}

void ImportOverlayComponent::resized()
{
    auto panel = getLocalBounds()
                   .withSizeKeepingCentre (juce::jmin (kPanelWidth, getWidth() - 24),
                                           juce::jmin (kPanelHeight, getHeight() - 24))
                   .reduced (18, 16);

    titleLabel.setBounds (panel.removeFromTop (24));
    panel.removeFromTop (6);

    auto footer = panel.removeFromBottom (28);
    cancelButton.setBounds (footer.removeFromRight (96));

    panel.removeFromBottom (10);
    bar.setBounds (panel.removeFromBottom (18));
    panel.removeFromBottom (8);

    messageLabel.setBounds (panel);
}

} // namespace vt
