/*
    ==========================================================================

    ReferenceTone.h — the note you get when you click a piano key.

    A small polyphonic struck-piano voice that lives on the audio thread and is
    triggered from the message thread with a single atomic store.

    WHY A LIVE SYNTH AND NOT THE EXISTING RENDER-TO-BUFFER PATH
    -----------------------------------------------------------
    ExerciseAudio already synthesises this exact tone, and it is the right tool
    for what it does: an exercise's guide track is KNOWN IN ADVANCE, so rendering
    it once and handing it to a BackingPlayer deck costs nothing at play time and
    keeps the chart off the audio thread.

    A piano key is the opposite case. Which note, and when, is not known until
    the user clicks it, and the answer has to be audible within a frame or the
    keyboard does not feel like an instrument. Rendering a fresh buffer per click
    would mean an allocation, a file-or-deck install and a transport seek on the
    message thread for every key press — hundreds of milliseconds of latency for
    a sound that has to arrive in tens.

    SAME VOICE, DELIBERATELY
    ------------------------
    The partial model is the one DESIGN.md specifies and ExerciseAudio
    implements: twelve inharmonic partials, f(n) = n*f0*sqrt(1 + B*n^2), each
    decaying faster than the one below it, with the 8th nulled by hammer
    position. It is duplicated here rather than shared because the two run on
    opposite sides of the realtime boundary — one fills a buffer of known length
    ahead of time, the other advances a handful of phasors per sample and must
    never allocate — and factoring them together would produce an abstraction
    whose only client is one of two callers. The CONSTANTS are what must not
    drift, and they are named identically in both files.

    THREADING
    ---------
    strike() is message thread and does one relaxed store. renderInto() is audio
    thread, allocates nothing, and takes no locks. The only work a trigger costs
    the audio thread is setting up twelve partials — about a hundred
    transcendental calls, a few microseconds against a 2.67 ms deadline, and only
    on the block a key is actually pressed.

    ==========================================================================
*/

#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

#include <array>
#include <atomic>

namespace vt
{

class ReferenceTone
{
public:
    ReferenceTone() = default;

    /** Message thread. Safe to call repeatedly; safe before any strike. */
    void prepare (double sampleRate) noexcept;

    /** MESSAGE THREAD. Asks for a strike at this MIDI note.

        Returns immediately having done one atomic store — the note starts on
        the next audio block. Out-of-range notes are ignored rather than
        clamped: a clamped note is a wrong pitch reference, which is worse than
        silence. */
    void strike (int midi) noexcept;

    /** Peak level of one struck note, linear. */
    void setGain (float g) noexcept
    {
        gain.store (juce::jlimit (0.0f, 1.0f, g), std::memory_order_relaxed);
    }

    float getGain() const noexcept { return gain.load (std::memory_order_relaxed); }

    /** AUDIO THREAD. Mixes every ringing voice into `buffer`. Returns at once
        when nothing is sounding. */
    void renderInto (juce::AudioBuffer<float>& buffer) noexcept;

    /** True while any voice is still audible. Audio thread state, so this is a
        snapshot — only useful for tests. */
    bool isSounding() const noexcept { return sounding.load (std::memory_order_relaxed); }

    //== The voice, shared with ExerciseAudio =================================
    static constexpr int    kNumPartials     = 12;
    static constexpr double kInharmonicity   = 0.00045;
    static constexpr double kHammerPosition  = 1.0 / 8.0;
    static constexpr double kPartialRolloff  = 1.9;
    static constexpr double kT60AtMiddleC    = 6.0;
    static constexpr double kT60PerPartial   = 0.55;
    static constexpr double kT60Min          = 1.8;
    static constexpr double kT60Max          = 6.5;
    static constexpr double kAttackSeconds   = 0.004;
    static constexpr double kSilenceFloor    = 1.0e-4;

    /** Four voices, round-robin.

        Not one. Retriggering a single voice would cut the previous note dead,
        and running up a scale on the keyboard is exactly what someone checking
        their range does — every note would end in a click. Four is enough that a
        played phrase overlaps naturally, as a sustain pedal would. */
    static constexpr int kNumVoices = 4;

private:
    struct Partial
    {
        double re = 1.0, im = 0.0;    // phase 0 => starts at zero, so no click
        double cosW = 1.0, sinW = 0.0;
        double amp = 0.0;
        double env = 1.0;
        double decay = 1.0;
    };

    struct Voice
    {
        std::array<Partial, (size_t) kNumPartials> partials {};
        int    used = 0;
        double gain = 0.0;         ///< 1 / sum(amp), times the peak level
        int    age = 0;            ///< samples since the strike, for the attack
        bool   active = false;
    };

    /** AUDIO THREAD. Sets a voice up for `midi`. No allocation. */
    void startVoice (Voice&, int midi, double sampleRate) noexcept;

    //== AUDIO THREAD ONLY ====================================================
    std::array<Voice, (size_t) kNumVoices> voices {};
    int nextVoice = 0;

    /** The rate the ringing voices were built for, so renderInto() can tell
        when prepare() has published a new one. Audio thread only. */
    double audioRate = 0.0;

    //== The boundary =========================================================
    /** The only state prepare() writes. See the comment in the .cpp. */
    std::atomic<double> preparedRateAtomic { 0.0 };

    /** Note and a monotonic counter in ONE word, so the audio thread can never
        read a new counter against an old note. Layout: (counter << 8) | midi. */
    std::atomic<juce::uint32> request { 0 };
    juce::uint32 seenRequest = 0;

    std::atomic<float> gain     { 0.22f };
    std::atomic<bool>  sounding { false };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ReferenceTone)
};

} // namespace vt
