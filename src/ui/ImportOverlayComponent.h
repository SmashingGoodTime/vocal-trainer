/*
  ==============================================================================

    ImportOverlayComponent.h
    Vocal Trainer — M3.5

    Covers the highway while a song is being prepared.

    It is a full-area overlay rather than a status line because chart prep takes
    MINUTES, not seconds — separation alone is ~2 minutes for a 4-minute song on
    this machine's GPU, and 20-40 minutes without one. A progress hint tucked
    into a footer at that duration reads as a frozen app.

    The stage name is shown alongside the bar for the same reason: "Separating
    vocals from the backing" is a believable two-minute wait, an unlabelled bar
    stuck at 30% is not.

  ==============================================================================
*/

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>

#include "Theme.h"

namespace vt
{

class ImportOverlayComponent final : public juce::Component
{
public:
    explicit ImportOverlayComponent (Theme&);

    void paint (juce::Graphics&) override;
    void resized() override;

    /** Show the overlay and reset it for a new run. */
    void beginImport (const juce::String& fileName);

    void setProgress (double zeroToOne, const juce::String& message);

    /** Leave the overlay up with a message and a Close button — used for
        failures, so the reason does not vanish before it can be read. */
    void showFailure (const juce::String& message, const juce::String& detail);

    void applyTheme();

    std::function<void()> onCancel;
    std::function<void()> onDismiss;

private:
    Theme& theme;

    juce::Label titleLabel;
    juce::Label messageLabel;
    juce::TextButton cancelButton { "Cancel" };

    double progress = 0.0;
    juce::ProgressBar bar { progress };

    juce::String detailText;
    bool failed = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ImportOverlayComponent)
};

} // namespace vt
