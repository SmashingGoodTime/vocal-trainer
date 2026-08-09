#include "ReferenceTone.h"

#include <cmath>

namespace vt
{

namespace
{
    /** Phasor renormalisation interval. A unit complex number rotated a few
        hundred thousand times drifts in magnitude; renormalising periodically
        costs one sqrt per partial per interval and keeps the tone level. */
    constexpr int kRenormaliseInterval = 8192;

    inline double midiToHz (int midi) noexcept
    {
        return 440.0 * std::pow (2.0, ((double) midi - 69.0) / 12.0);
    }
}

//==============================================================================
void ReferenceTone::prepare (double sampleRate) noexcept
{
    if (sampleRate <= 0.0)
        return;

    //-------------------------------------------------------------------------
    // THE MESSAGE THREAD PUBLISHES THE RATE AND TOUCHES NOTHING ELSE.
    //
    // A rate change invalidates every phasor increment in flight — a voice whose
    // cosW/sinW were computed for 44.1 kHz and is then advanced at 48 kHz is a
    // reference tone at the WRONG PITCH, which is the one thing this must never
    // be — so the voices do have to be silenced. But silencing them from HERE
    // would be this class writing voice state while processBlock might be
    // reading it. JUCE is supposed to stop the callback around a device change
    // and mostly does; BackingPlayer carries a long comment about the one time
    // it did not, and that one cost a process.
    //
    // So renderInto() notices the change itself and silences the voices on the
    // thread that owns them. The only thing crossing the boundary is one atomic
    // double, and there is nothing left to get wrong.
    //-------------------------------------------------------------------------
    preparedRateAtomic.store (sampleRate, std::memory_order_release);
}

//==============================================================================
void ReferenceTone::strike (int midi) noexcept
{
    if (midi < 0 || midi > 127)
        return;

    // Read-modify-write on the counter half. Only the message thread writes, so
    // a plain load/store pair would do; fetch_add is the same cost and stays
    // correct if a second UI source is ever added.
    const auto previous = request.load (std::memory_order_relaxed);
    const auto counter  = (previous >> 8) + 1u;

    request.store ((counter << 8) | (juce::uint32) midi, std::memory_order_release);
}

//==============================================================================
void ReferenceTone::startVoice (Voice& voice, int midi, double sampleRate) noexcept
{
    voice.used = 0;
    voice.age = 0;
    voice.active = false;

    const auto frequency = midiToHz (midi);

    if (sampleRate <= 0.0 || frequency <= 0.0)
        return;

    // Lower notes ring longer, as they do on a real instrument — and a low
    // reference that died as fast as a high one leaves a bass singer with
    // nothing to pitch against half way through the note.
    const auto t60Fundamental = juce::jlimit (kT60Min, kT60Max,
                                              kT60AtMiddleC * std::sqrt (261.6256 / frequency));

    double ampSum = 0.0;

    for (int p = 1; p <= kNumPartials; ++p)
    {
        const auto f = frequency * (double) p
                         * std::sqrt (1.0 + kInharmonicity * (double) (p * p));

        // Anything close to Nyquist would alias into an audible WRONG PITCH,
        // which for a pitch reference is the one unacceptable artefact.
        if (f >= 0.45 * sampleRate)
            break;

        auto amp = std::pow (1.0 / (double) p, kPartialRolloff);
        amp *= std::abs (std::sin (juce::MathConstants<double>::pi * (double) p * kHammerPosition));

        if (amp <= 0.0)
            continue;

        const auto t60 = t60Fundamental / (1.0 + kT60PerPartial * (double) (p - 1));
        const auto omega = juce::MathConstants<double>::twoPi * f / sampleRate;

        auto& partial = voice.partials[(size_t) voice.used++];
        partial.re    = 1.0;
        partial.im    = 0.0;
        partial.env   = 1.0;
        partial.cosW  = std::cos (omega);
        partial.sinW  = std::sin (omega);
        partial.amp   = amp;
        partial.decay = std::exp (-std::log (1000.0) / (t60 * sampleRate));

        ampSum += amp;
    }

    if (voice.used == 0 || ampSum <= 0.0)
        return;

    voice.gain = 1.0 / ampSum;
    voice.active = true;
}

//==============================================================================
void ReferenceTone::renderInto (juce::AudioBuffer<float>& buffer) noexcept
{
    const auto numSamples = buffer.getNumSamples();
    const auto numChannels = buffer.getNumChannels();

    const auto sampleRate = preparedRateAtomic.load (std::memory_order_acquire);

    if (numSamples <= 0 || numChannels <= 0 || sampleRate <= 0.0)
        return;

    //---------------------------------------------------------------- rate change
    // See prepare(): the voices are silenced HERE, on the thread that owns them.
    if (! juce::approximatelyEqual (sampleRate, audioRate))
    {
        audioRate = sampleRate;

        for (auto& voice : voices)
            voice.active = false;

        sounding.store (false, std::memory_order_relaxed);
    }

    //-------------------------------------------------------------------- trigger
    const auto pending = request.load (std::memory_order_acquire);

    if (pending != seenRequest)
    {
        seenRequest = pending;

        auto& voice = voices[(size_t) nextVoice];
        nextVoice = (nextVoice + 1) % kNumVoices;

        startVoice (voice, (int) (pending & 0xffu), sampleRate);
    }

    //--------------------------------------------------------------------- render
    const auto peak = (double) gain.load (std::memory_order_relaxed);
    const auto attack = juce::jmax (1.0, kAttackSeconds * sampleRate);

    // The tone is mono and goes to every channel at the same level. Adding it
    // per sample rather than rendering to a scratch buffer and copying keeps
    // this allocation-free with no maximum block size to get wrong — and a
    // channel-wise COPY would be flatly incorrect here, because by this point
    // the buffer already holds the backing track and the monitored microphone.
    auto* const* channels = buffer.getArrayOfWritePointers();

    bool anyActive = false;

    for (auto& voice : voices)
    {
        if (! voice.active)
            continue;

        for (int i = 0; i < numSamples; ++i)
        {
            double sum = 0.0;
            double loudest = 0.0;

            for (int k = 0; k < voice.used; ++k)
            {
                auto& partial = voice.partials[(size_t) k];

                sum += partial.amp * partial.env * partial.im;

                const auto re = partial.re * partial.cosW - partial.im * partial.sinW;
                partial.im    = partial.re * partial.sinW + partial.im * partial.cosW;
                partial.re    = re;
                partial.env  *= partial.decay;

                loudest = juce::jmax (loudest, partial.env);
            }

            const auto rise = juce::jmin (1.0, (double) voice.age / attack);
            const auto s = (float) (peak * voice.gain * sum * rise);

            for (int ch = 0; ch < numChannels; ++ch)
                channels[ch][i] += s;

            ++voice.age;

            if ((voice.age % kRenormaliseInterval) == kRenormaliseInterval - 1)
            {
                for (int k = 0; k < voice.used; ++k)
                {
                    auto& partial = voice.partials[(size_t) k];
                    const auto mag = std::sqrt (partial.re * partial.re + partial.im * partial.im);

                    if (mag > 1.0e-12)
                    {
                        partial.re /= mag;
                        partial.im /= mag;
                    }
                }
            }

            // Stop once the whole voice is 80 dB down. The remaining tail is
            // inaudible and would otherwise cost cycles for the rest of the song.
            if (loudest < kSilenceFloor)
            {
                voice.active = false;
                break;
            }
        }

        anyActive = anyActive || voice.active;
    }

    sounding.store (anyActive, std::memory_order_relaxed);
}

} // namespace vt
