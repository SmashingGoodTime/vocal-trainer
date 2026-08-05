#pragma once

/*
    VoicingGate.h — level + clarity gating and octave-glitch suppression for the
    analysis worker thread. DESIGN.md §5, "Signal conditioning and voicing gate".

        RMS window        20 ms
        Noise floor       min-tracker over 3 s
        Voicing gate      opens at floor + 15 dB
                          closes at floor +  9 dB   (hysteresis)
                          120 ms hold               (unvoiced consonants mid-word)
                          absolute floor -55 dBFS
        Clarity gate      reject NSDF clarity < 0.85
        Median filter     3-tap, on fractional MIDI

    The 120 ms hold is what stops t / s / k mid-word from dropping the indicator.
    The 3-tap median is the cheapest and most effective octave-glitch killer;
    it costs exactly one hop (8 ms).

    NOISE FLOOR IMPLEMENTATION NOTE
    A naive single-pole "exponential" tracker that creeps toward the current
    level is unusable here: after ~3 s of sustained singing the estimate would
    have climbed to the singing level and the gate would close mid-note. What is
    implemented instead is a true sliding-window minimum over 3 s, computed in
    O(1) with four rotating 750 ms sub-window minima, followed by an exponential
    smoother (fast down, 250 ms up) so the estimate never steps. This is the
    standard minimum-statistics construction and is what "exponential
    min-tracker over 3 s" has to mean in practice.

    Header-only, self-contained. All storage is allocated in prepare();
    every other method is allocation-free and noexcept.
*/

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <vector>

#include "LockFreeTypes.h"

namespace vt
{

class VoicingGate
{
public:
    //==========================================================================
    static constexpr double kRmsWindowSeconds   = 0.020;   // 20 ms
    static constexpr double kNoiseWindowSeconds = 3.0;     // min-tracker span
    static constexpr int    kNoiseSubWindows    = 4;       // 4 x 750 ms
    static constexpr float  kOpenAboveFloorDb   = 15.0f;
    static constexpr float  kCloseAboveFloorDb  =  9.0f;
    static constexpr double kHoldSeconds        = 0.120;   // 120 ms
    static constexpr float  kAbsoluteFloorDb    = -55.0f;  // dBFS
    static constexpr float  kClarityThreshold   = 0.85f;
    static constexpr float  kSilenceDb          = -120.0f; // clamp for log of ~0
    static constexpr int    kMedianTaps         = 3;

    VoicingGate() = default;

    //==========================================================================
    /** @param sampleRate  the ANALYSIS rate, 16000.0. */
    void prepare (double sampleRate = kAnalysisRate)
    {
        rate = (sampleRate > 0.0) ? sampleRate : kAnalysisRate;

        rmsLength = std::max (1, (int) std::lround (kRmsWindowSeconds * rate));
        rmsRing.assign ((size_t) rmsLength, 0.0f);

        subWindowSeconds = kNoiseWindowSeconds / (double) kNoiseSubWindows;

        reset();
    }

    void reset() noexcept
    {
        std::fill (rmsRing.begin(), rmsRing.end(), 0.0f);
        rmsWrite   = 0;
        rmsSum     = 0.0;
        rmsFilled  = 0;
        currentRmsDb = kSilenceDb;

        for (auto& s : subMinDb)
            s = std::numeric_limits<float>::infinity();

        subIndex        = 0;
        subAccumMinDb   = std::numeric_limits<float>::infinity();
        subElapsed      = 0.0;
        floorDb         = kSilenceDb;
        floorInitialised = false;

        open      = false;
        holdLeft  = 0.0;

        medianCount = 0;
        for (auto& v : medianHistory)
            v = 0.0f;
    }

    //==========================================================================
    /** Feed one hop of 16 kHz audio (normally kChunkSize == 128 samples).
        Updates the RMS, the noise floor and the gate state machine.
        Call this exactly once per analysis hop, in order. */
    void pushHop (const float* samples, int numSamples) noexcept
    {
        if (samples == nullptr || numSamples <= 0 || rmsRing.empty())
            return;

        for (int i = 0; i < numSamples; ++i)
        {
            const auto s = (double) samples[i] * (double) samples[i];

            rmsSum -= (double) rmsRing[(size_t) rmsWrite];
            rmsRing[(size_t) rmsWrite] = (float) s;
            rmsSum += s;

            if (++rmsWrite >= rmsLength)
                rmsWrite = 0;

            if (rmsFilled < rmsLength)
                ++rmsFilled;
        }

        if (rmsSum < 0.0)          // guard against float cancellation drift
            rmsSum = 0.0;

        const double meanSq = rmsSum / (double) std::max (1, rmsFilled);
        currentRmsDb = toDb (std::sqrt (meanSq));

        const double dt = (double) numSamples / rate;

        updateNoiseFloor (currentRmsDb, dt);
        updateGate (currentRmsDb, dt);
    }

    //==========================================================================
    /** Overrides the clarity gate threshold. kClarityThreshold (0.85) is the
        default; theme.json can retune it live without a rebuild, which is the
        point of the hot-reload surface. Clamped to a sane range so a bad edit
        cannot wedge the gate permanently open or shut.

        Safe to call from the worker thread between process() calls. */
    void setClarityThreshold (float t) noexcept
    {
        clarityThreshold = std::isfinite (t) ? std::clamp (t, 0.0f, 1.0f)
                                             : kClarityThreshold;
    }

    float getClarityThreshold() const noexcept { return clarityThreshold; }

    //==========================================================================
    /** Applies the clarity gate and the level gate, then the 3-tap median.
        @param fracMidiCandidate  the detector's fractional MIDI, or NaN
        @param clarity            the NSDF peak, 0..1
        @returns the median-filtered fractional MIDI, or NaN if rejected. */
    float process (float fracMidiCandidate, float clarity) noexcept
    {
        const bool accepted = open
                           && std::isfinite (fracMidiCandidate)
                           && clarity >= clarityThreshold;

        if (! accepted)
        {
            medianCount = 0;         // a rejected frame breaks the run
            lastAccepted = false;
            return kUnvoiced;
        }

        lastAccepted = true;

        if (medianCount < kMedianTaps)
        {
            medianHistory[(size_t) medianCount] = fracMidiCandidate;
            ++medianCount;

            // Fewer than 3 samples: pass the newest through rather than
            // averaging, so a single glitch is never blended into the output.
            return fracMidiCandidate;
        }

        medianHistory[0] = medianHistory[1];
        medianHistory[1] = medianHistory[2];
        medianHistory[2] = fracMidiCandidate;

        return median3 (medianHistory[0], medianHistory[1], medianHistory[2]);
    }

    //==========================================================================
    float getRmsDb() const noexcept        { return currentRmsDb; }
    float getNoiseFloorDb() const noexcept { return floorDb; }
    bool  isOpen() const noexcept          { return open; }
    bool  lastFrameAccepted() const noexcept { return lastAccepted; }

    float getOpenThresholdDb() const noexcept
    {
        return std::max (floorDb + kOpenAboveFloorDb, kAbsoluteFloorDb);
    }

    float getCloseThresholdDb() const noexcept
    {
        return std::max (floorDb + kCloseAboveFloorDb, kAbsoluteFloorDb);
    }

private:
    //==========================================================================
    static float toDb (double linear) noexcept
    {
        if (! (linear > 1.0e-7))
            return kSilenceDb;

        return (float) std::max (20.0 * std::log10 (linear), (double) kSilenceDb);
    }

    static float median3 (float a, float b, float c) noexcept
    {
        return std::max (std::min (a, b), std::min (std::max (a, b), c));
    }

    void updateNoiseFloor (float rmsDb, double dt) noexcept
    {
        subAccumMinDb = std::min (subAccumMinDb, rmsDb);
        subElapsed   += dt;

        if (subElapsed >= subWindowSeconds)
        {
            subMinDb[(size_t) subIndex] = subAccumMinDb;
            subIndex      = (subIndex + 1) % kNoiseSubWindows;
            subAccumMinDb = std::numeric_limits<float>::infinity();
            subElapsed    = 0.0;
        }

        float rawMin = subAccumMinDb;

        for (int i = 0; i < kNoiseSubWindows; ++i)
            rawMin = std::min (rawMin, subMinDb[(size_t) i]);

        if (! std::isfinite (rawMin))
            rawMin = rmsDb;

        if (! floorInitialised)
        {
            floorDb = rawMin;
            floorInitialised = true;
            return;
        }

        if (rawMin <= floorDb)
        {
            floorDb = rawMin;                       // instant downward tracking, always
        }
        else if (! open)
        {
            // Upward creep ONLY while the gate is shut. Otherwise a note held
            // for longer than kNoiseWindowSeconds would drag the floor up to
            // its own level and shut the gate mid-note.
            constexpr double kUpTimeConstant = 0.25;   // seconds
            const auto alpha = (float) (1.0 - std::exp (-dt / kUpTimeConstant));
            floorDb += (rawMin - floorDb) * alpha;
        }
    }

    void updateGate (float rmsDb, double dt) noexcept
    {
        const bool aboveOpen  = (rmsDb >= floorDb + kOpenAboveFloorDb)  && (rmsDb >= kAbsoluteFloorDb);
        const bool aboveClose = (rmsDb >= floorDb + kCloseAboveFloorDb) && (rmsDb >= kAbsoluteFloorDb);

        if (! open)
        {
            if (aboveOpen)
            {
                open     = true;
                holdLeft = kHoldSeconds;
            }
        }
        else
        {
            if (aboveClose)
            {
                holdLeft = kHoldSeconds;
            }
            else
            {
                holdLeft -= dt;

                if (holdLeft <= 0.0)
                {
                    open     = false;
                    holdLeft = 0.0;
                }
            }
        }
    }

    //==========================================================================
    double rate      = kAnalysisRate;
    int    rmsLength = 320;

    std::vector<float> rmsRing;
    int    rmsWrite  = 0;
    int    rmsFilled = 0;
    double rmsSum    = 0.0;
    float  clarityThreshold = kClarityThreshold;   // runtime-tunable via theme.json
    float  currentRmsDb = kSilenceDb;

    double subWindowSeconds = kNoiseWindowSeconds / (double) kNoiseSubWindows;
    float  subMinDb[kNoiseSubWindows] { std::numeric_limits<float>::infinity(),
                                        std::numeric_limits<float>::infinity(),
                                        std::numeric_limits<float>::infinity(),
                                        std::numeric_limits<float>::infinity() };
    int    subIndex      = 0;
    float  subAccumMinDb = std::numeric_limits<float>::infinity();
    double subElapsed    = 0.0;
    float  floorDb       = kSilenceDb;
    bool   floorInitialised = false;

    bool   open     = false;
    double holdLeft = 0.0;

    float  medianHistory[kMedianTaps] { 0.0f, 0.0f, 0.0f };
    int    medianCount  = 0;
    bool   lastAccepted = false;
};

} // namespace vt
