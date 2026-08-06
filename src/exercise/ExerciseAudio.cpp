#include "ExerciseAudio.h"

#include <array>
#include <cmath>

namespace vt
{

namespace
{
    /** Click voice. Short enough to be a transient rather than a pitch. */
    constexpr double kClickSeconds     = 0.045;
    constexpr float  kClickHz          = 1000.0f;
    constexpr float  kAccentHz         = 1600.0f;

    /** Quiet on purpose: the click is a reference, and a loud metronome in
        headphones makes people tense their throat. */
    constexpr float  kClickAmplitude   = 0.10f;
    constexpr float  kAccentAmplitude  = 0.20f;

    //== The piano voice ======================================================
    /** Twelve partials. Enough for the strike to have a recognisable edge;
        beyond about twelve the extra content is above 6 kHz for any note in
        singing range, where it is doing nothing but adding hiss. */
    constexpr int kNumPartials = 12;

    /** String stiffness. A real string's partials are not exact multiples of the
        fundamental — f(n) = n·f0·sqrt(1 + B·n²) — and that slight stretch is a
        large part of why a piano sounds like a piano and an additive stack of
        exact harmonics sounds like an organ. */
    constexpr double kInharmonicity = 0.00045;

    /** Hammers strike about an eighth of the way along the string, which nulls
        the 8th partial and thins the ones either side of it. One multiply at
        setup, and without it the tone is noticeably more nasal. */
    constexpr double kHammerPosition = 1.0 / 8.0;

    /** Partial amplitudes fall off as 1/n^this. Near the 1/n² of an ideal struck
        string: bright enough to place the note, warm enough not to compete with
        the singer's own upper harmonics. */
    constexpr double kPartialRolloff = 1.9;

    /** Ring time of the fundamental at middle C, and how much faster each
        partial above it dies away. The upper partials decaying FIRST is the
        signature of a struck string — it is what makes the note settle into a
        purer tone a moment after the strike instead of buzzing at one colour. */
    constexpr double kT60AtMiddleC    = 6.0;
    constexpr double kT60PerPartial   = 0.55;
    constexpr double kT60Min          = 1.8;
    constexpr double kT60Max          = 6.5;

    constexpr double kPianoAttackSeconds = 0.004;

    /** How far the note rings past the span it was asked for, and the fade
        applied at the very end of that so a truncated note cannot click. */
    constexpr double kPianoTailSeconds = 0.30;
    constexpr double kPianoFadeSeconds = 0.06;

    /** Stop rendering a note once it is 80 dB down: the remaining tail is
        inaudible, and for a long exercise it is most of the samples. */
    constexpr double kPianoSilenceFloor = 1.0e-4;

    /** Phasor renormalisation interval — see the recurrence in addPianoNote. */
    constexpr int kRenormaliseInterval = 8192;

    /** Peak amplitude of the guide tone before the reference-deck gain is
        applied. The Guide slider defaults to -12 dB (theme: guideVocalDb), so
        this lands around 0.125 in practice — present, not dominant. */
    constexpr float  kToneAmplitude    = 0.50f;

    /** A re-strike inside a long note is a reminder, not a new note, so it is
        quieter than the one that started it. */
    constexpr float  kRestrikeAmplitude = 0.55f;

    /** Notes are re-struck every this many beats, and never faster than
        kRestrikeMinSeconds apart at a quick tempo.

        Three beats, not four: an eight-beat long tone struck twice leaves the
        reference 20 dB down by the last quarter of the note, which is precisely
        the part where a held note starts to sag and the singer most needs
        something to check it against. */
    constexpr double kRestrikeBeats      = 3.0;
    constexpr double kRestrikeMinSeconds = 1.5;

    /** Peak amplitude of the starting reference pitch, which plays on the CLICK
        deck at unity rather than through the Guide slider. Louder than the
        clicks it sits over, quiet enough not to be a shock in headphones. */
    constexpr float  kReferenceAmplitude = 0.32f;

    /** A cap, not a design limit: a pathological spec must not try to allocate
        an hour of audio. */
    constexpr double kMaxRenderSeconds = 900.0;

    inline float midiToHz (int midi) noexcept
    {
        return 440.0f * std::pow (2.0f, ((float) midi - 69.0f) / 12.0f);
    }

    juce::AudioBuffer<float> makeStereoBuffer (double seconds, double sampleRate)
    {
        const auto clamped = juce::jlimit (0.25, kMaxRenderSeconds, seconds);
        const auto numSamples = (int) std::ceil (clamped * sampleRate);

        // Stereo, even though both channels are identical: every consumer of
        // these buffers mixes channel-wise into a stereo output, and handing it
        // a mono source is one more special case for no benefit.
        juce::AudioBuffer<float> buffer (2, juce::jmax (1, numSamples));
        buffer.clear();
        return buffer;
    }
}

//==============================================================================
void ExerciseAudio::addClick (juce::AudioBuffer<float>& buffer, double startSeconds,
                              double sampleRate, float frequency, float amplitude)
{
    const auto start = (int) std::llround (startSeconds * sampleRate);
    const auto length = (int) std::llround (kClickSeconds * sampleRate);

    if (start < 0 || start >= buffer.getNumSamples())
        return;

    const auto n = juce::jmin (length, buffer.getNumSamples() - start);
    const auto omega = juce::MathConstants<float>::twoPi * frequency / (float) sampleRate;

    // Exponential decay to about -60 dB over the click's length: a hard cutoff
    // would put a step in the waveform, which is audible as a second, duller
    // click a few milliseconds after the first.
    const auto decay = std::exp (-7.0f / (float) juce::jmax (1, n));

    float env = amplitude;

    for (int i = 0; i < n; ++i)
    {
        const auto s = env * std::sin (omega * (float) i);

        for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
            buffer.addSample (ch, start + i, s);

        env *= decay;
    }
}

//==============================================================================
void ExerciseAudio::addPianoNote (juce::AudioBuffer<float>& buffer, double startSeconds,
                                  double sustainSeconds, double frequency,
                                  float amplitude, double sampleRate)
{
    const auto start = (int) std::llround (startSeconds * sampleRate);

    if (start < 0 || start >= buffer.getNumSamples()
          || frequency <= 0.0 || amplitude <= 0.0f || sampleRate <= 0.0)
        return;

    const auto span = (int) std::llround ((juce::jmax (0.0, sustainSeconds) + kPianoTailSeconds)
                                            * sampleRate);
    const auto n = juce::jmin (span, buffer.getNumSamples() - start);

    if (n <= 0)
        return;

    //-------------------------------------------------------------------------
    // ONE ROTATING PHASOR PER PARTIAL, not a std::sin per partial per sample.
    //
    // Twelve partials over a two-minute exercise is on the order of a hundred
    // million sine calls, which is a visible freeze on the message thread when
    // all the user did was pick an item from a menu. Rotating a unit complex
    // number by a fixed angle costs four multiplies and gives the same sequence;
    // its magnitude drifts by rounding, so it is renormalised periodically.
    //-------------------------------------------------------------------------
    struct Partial
    {
        double re = 1.0, im = 0.0;    // phase 0 => starts at zero, so no click
        double cosW = 1.0, sinW = 0.0;
        double amp = 0.0;
        double env = 1.0;
        double decay = 1.0;
    };

    std::array<Partial, (size_t) kNumPartials> partials {};
    int used = 0;
    double ampSum = 0.0;

    // Lower notes ring longer, as they do on a real instrument — and, more to the
    // point here, a low reference that died as fast as a high one would leave a
    // bass singer with nothing to pitch against half way through the note.
    const auto t60Fundamental = juce::jlimit (kT60Min, kT60Max,
                                              kT60AtMiddleC * std::sqrt (261.6256 / frequency));

    for (int p = 1; p <= kNumPartials; ++p)
    {
        const auto f = frequency * (double) p
                         * std::sqrt (1.0 + kInharmonicity * (double) (p * p));

        // Anything close to Nyquist would alias into an audible wrong pitch,
        // which for a PITCH REFERENCE is the one unacceptable artefact.
        if (f >= 0.45 * sampleRate)
            break;

        auto amp = std::pow (1.0 / (double) p, kPartialRolloff);
        amp *= std::abs (std::sin (juce::MathConstants<double>::pi * (double) p * kHammerPosition));

        if (amp <= 0.0)
            continue;

        const auto t60 = t60Fundamental / (1.0 + kT60PerPartial * (double) (p - 1));
        const auto omega = juce::MathConstants<double>::twoPi * f / sampleRate;

        auto& partial = partials[(size_t) used++];
        partial.cosW  = std::cos (omega);
        partial.sinW  = std::sin (omega);
        partial.amp   = amp;
        partial.decay = std::exp (-std::log (1000.0) / (t60 * sampleRate));

        ampSum += amp;
    }

    if (used == 0 || ampSum <= 0.0)
        return;

    const auto gain    = (double) amplitude / ampSum;
    const auto attack  = juce::jmax (1.0, kPianoAttackSeconds * sampleRate);
    const auto fadeLen = juce::jmax (1, (int) std::llround (kPianoFadeSeconds * sampleRate));
    const auto fadeAt  = n - fadeLen;

    for (int i = 0; i < n; ++i)
    {
        double sum = 0.0;

        for (int k = 0; k < used; ++k)
        {
            auto& partial = partials[(size_t) k];

            sum += partial.amp * partial.env * partial.im;

            const auto re = partial.re * partial.cosW - partial.im * partial.sinW;
            partial.im    = partial.re * partial.sinW + partial.im * partial.cosW;
            partial.re    = re;
            partial.env  *= partial.decay;
        }

        const auto rise = juce::jmin (1.0, (double) i / attack);
        const auto fall = i >= fadeAt ? (double) (n - i) / (double) fadeLen : 1.0;
        const auto s    = (float) (gain * sum * rise * fall);

        for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
            buffer.addSample (ch, start + i, s);

        if ((i % kRenormaliseInterval) == kRenormaliseInterval - 1)
        {
            for (int k = 0; k < used; ++k)
            {
                auto& partial = partials[(size_t) k];
                const auto mag = std::sqrt (partial.re * partial.re + partial.im * partial.im);

                if (mag > 0.0)
                {
                    partial.re /= mag;
                    partial.im /= mag;
                }
            }
        }

        // The fundamental has the longest ring time of the lot, so once IT is
        // below the floor everything above it already is.
        if (partials[0].env < kPianoSilenceFloor)
            break;
    }
}

//==============================================================================
juce::AudioBuffer<float> ExerciseAudio::renderClickTrack (const Chart& chart,
                                                          const ExerciseSpec& spec,
                                                          double sampleRate)
{
    if (sampleRate <= 0.0)
        return {};

    // A beat of air past the end, so the transport does not report the exercise
    // finished while the last note is still being sung.
    auto buffer = makeStereoBuffer (chart.getDurationSeconds() + 1.0, sampleRate);

    const auto beat = ExerciseLibrary::beatSeconds (spec);
    const auto leadIn = ExerciseLibrary::leadInSeconds (spec);
    const auto referenceSeconds = ExerciseLibrary::referencePitchSeconds (spec);
    const auto totalSeconds = (double) buffer.getNumSamples() / sampleRate;

    if (beat <= 0.0)
        return buffer;

    //-------------------------------------------------------------------------
    // The starting pitch, struck alone. It rings through the count-in and is
    // gone by the time the singer comes in, which is the whole shape of the
    // thing: here is the note, now here is the tempo, now sing it.
    //-------------------------------------------------------------------------
    if (referenceSeconds > 0.0 && ! chart.getNotes().empty())
    {
        const auto& first = chart.getNotes().front();

        addPianoNote (buffer, 0.0, leadIn,
                      (double) midiToHz (first.midi)
                        * std::pow (2.0, (double) first.centsOffset / 1200.0),
                      kReferenceAmplitude, sampleRate);
    }

    // Clicks start AFTER the reference pitch, not at zero. A tick over the strike
    // is the one moment the singer is listening hardest to the pitch, and it is
    // the moment they have least need of the tempo.
    for (double t = referenceSeconds; t < totalSeconds; t += beat)
    {
        // Everything before the first note is count-in and is accented.
        const bool countIn = t < leadIn - 1.0e-6;

        addClick (buffer, t, sampleRate,
                  countIn ? kAccentHz : kClickHz,
                  countIn ? kAccentAmplitude : kClickAmplitude);
    }

    return buffer;
}

//==============================================================================
juce::AudioBuffer<float> ExerciseAudio::renderGuideTone (const Chart& chart,
                                                         const ExerciseSpec& spec,
                                                         double sampleRate)
{
    if (sampleRate <= 0.0)
        return {};

    auto buffer = makeStereoBuffer (chart.getDurationSeconds() + 1.0, sampleRate);

    const auto restrikeEvery = juce::jmax (kRestrikeMinSeconds,
                                           ExerciseLibrary::beatSeconds (spec) * kRestrikeBeats);

    for (const auto& note : chart.getNotes())
    {
        if (note.durationSeconds <= 0.0)
            continue;

        // centsOffset is respected: for a chart derived from a real recording it
        // carries the reference singer's own deviation, and a guide tone that
        // ignored it would disagree with the ghost contour drawn on the highway.
        const auto hz = (double) midiToHz (note.midi)
                          * std::pow (2.0, (double) note.centsOffset / 1200.0);

        // Re-strike a note that outlasts the decay. Not doing so is what made
        // the long-tone exercise silent for its second half — the singer is left
        // holding a note with nothing to check it against at exactly the point
        // where a held note starts to drift.
        for (double offset = 0.0; offset < note.durationSeconds - 0.4; offset += restrikeEvery)
            addPianoNote (buffer, note.startSeconds + offset,
                          note.durationSeconds - offset, hz,
                          offset > 0.0 ? kToneAmplitude * kRestrikeAmplitude : kToneAmplitude,
                          sampleRate);
    }

    return buffer;
}

} // namespace vt
