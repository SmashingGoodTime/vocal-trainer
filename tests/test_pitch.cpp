/*
    test_pitch.cpp — regression tests for the real-time DSP core.

    Plain main(); returns 0 on success, non-zero on failure. One PASS/FAIL line
    per case, with the measured numbers printed so a failure is diagnosable.

    Build: compile this together with src/dsp/AnalysisChain.cpp and link
           juce_core, juce_audio_basics and juce_dsp. No GUI modules are used
           and the JUCE message manager is never started.

    Cases:
      1. 440 Hz sine through the whole chain at BOTH 44100 and 48000 host rates,
         within 2 cents. This is the regression test for the resampling-rate
         trap (computing f from the host rate instead of 16000 is 145 cents).
      2. E2 (82.41 Hz) and A5 (880 Hz), within 5 cents.
      3. A sine an octave below a constrained search range: no octave error.
         Plus the positive control — the same sine with the range placed
         correctly must be detected.
      4. White noise: low clarity, gate rejects, fracMidi is NaN.
      5. Silence: NaN, no crash.
      6. Supporting invariants (free to run, catch the silliest regressions):
         a. hostSampleAtWindowCentre scales by hostRate / 16000.
         b. Key-maxima picking does not octave-error on a harmonically rich
            signal with the search range unconstrained.
         c. An 8 s sustained note is not gated off by the noise-floor tracker.
         d. The 1-Euro filter passes NaN through and re-primes after a gap.
*/

#include "../src/dsp/AnalysisChain.h"
#include "../src/dsp/OneEuroFilter.h"
#include "../src/dsp/PitchDetectorMPM.h"
#include "../src/dsp/VoicingGate.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

namespace
{

constexpr double kPi = 3.14159265358979323846;

int failures = 0;
int caseIndex = 0;

void report (const char* name, bool ok, const std::string& detail)
{
    ++caseIndex;
    std::printf ("[%s] case %d: %-46s %s\n",
                 ok ? "PASS" : "FAIL", caseIndex, name, detail.c_str());

    if (! ok)
        ++failures;
}

std::string fmt (const char* pattern, double a)
{
    char buffer[256];
    std::snprintf (buffer, sizeof (buffer), pattern, a);
    return buffer;
}

std::string fmt2 (const char* pattern, double a, double b)
{
    char buffer[256];
    std::snprintf (buffer, sizeof (buffer), pattern, a, b);
    return buffer;
}

//==============================================================================
// Deterministic white noise, so a failure is reproducible.
struct Lcg
{
    uint32_t state = 0x13572468u;

    float next() noexcept
    {
        state = state * 1664525u + 1013904223u;
        return (float) ((double) (state >> 8) / 8388608.0 - 1.0);   // ~[-1, 1)
    }
};

enum class SignalType { sine, saw, noise, silence };

/** Band-limited sawtooth: a crude but harmonically rich stand-in for a chest
    voice. This is the signal shape that tempts autocorrelation methods into an
    octave-up error, so it is worth having in the suite. */
double bandLimitedSaw (double phase, int numHarmonics)
{
    double v = 0.0;

    for (int k = 1; k <= numHarmonics; ++k)
        v += std::sin (phase * (double) k) / (double) k;

    return v * (2.0 / kPi);
}

/** Builds a host-rate test signal: `leadInSeconds` of near-silence (so the
    noise-floor min-tracker has something sane to latch onto) followed by
    `bodySeconds` of the requested content. */
std::vector<float> makeSignal (SignalType type,
                               double freqHz,
                               double hostRate,
                               double bodySeconds,
                               double amplitude,
                               double leadInSeconds = 0.5)
{
    const auto leadIn = (size_t) std::lround (leadInSeconds * hostRate);
    const auto body   = (size_t) std::lround (bodySeconds   * hostRate);

    std::vector<float> out (leadIn + body, 0.0f);

    Lcg rng;

    // -100 dBFS-ish dither during the lead-in: quiet enough to be well under
    // the -55 dBFS absolute floor, non-zero so the floor estimate is finite.
    for (size_t i = 0; i < leadIn; ++i)
        out[i] = rng.next() * 1.0e-5f;

    const double phaseStep = 2.0 * kPi * freqHz / hostRate;

    for (size_t i = 0; i < body; ++i)
    {
        switch (type)
        {
            case SignalType::sine:    out[leadIn + i] = (float) (amplitude * std::sin (phaseStep * (double) i)); break;
            case SignalType::saw:
            {
                const auto maxHarmonic = (int) std::max (1.0, std::floor (7000.0 / std::max (1.0, freqHz)));
                out[leadIn + i] = (float) (amplitude * bandLimitedSaw (phaseStep * (double) i, maxHarmonic));
                break;
            }
            case SignalType::noise:   out[leadIn + i] = (float) (amplitude * rng.next()); break;
            case SignalType::silence: out[leadIn + i] = 0.0f; break;
        }
    }

    return out;
}

//==============================================================================
struct Frame
{
    float rawFracMidi  = vt::kUnvoiced;   // detector output, before gating
    float freqHz       = 0.0f;
    float clarity      = 0.0f;
    float gatedFracMidi = vt::kUnvoiced;  // after voicing/clarity gate + median
    float smoothed     = vt::kUnvoiced;   // after the 1-Euro filter
    float rmsDb        = -120.0f;
    bool  gateOpen     = false;
};

/** Runs the full production path: AnalysisChain -> PitchDetectorMPM ->
    VoicingGate (level + clarity + 3-tap median) -> OneEuroFilter. */
std::vector<Frame> runChain (const std::vector<float>& hostSignal,
                             double hostRate,
                             double searchMinHz = vt::PitchDetectorMPM::kDefaultMinHz,
                             double searchMaxHz = vt::PitchDetectorMPM::kDefaultMaxHz,
                             int hostBlockSize = 512)
{
    vt::AnalysisChain chain;
    chain.prepare (hostRate, hostBlockSize);

    // Drain the chain into a contiguous 16 kHz buffer, checking the timestamps
    // stay monotonic while we are here.
    std::vector<float> lo;
    lo.reserve (hostSignal.size());

    int64_t previousStamp = std::numeric_limits<int64_t>::min();
    bool    stampsMonotonic = true;

    int64_t pos = 0;
    const auto total = (int64_t) hostSignal.size();

    while (pos < total)
    {
        const auto n = (int) std::min<int64_t> (hostBlockSize, total - pos);
        chain.processBlock (&hostSignal[(size_t) pos], n, pos);

        vt::AudioChunk chunk;

        while (chain.popChunk (chunk))
        {
            if (chunk.hostSampleIndexOfFirst <= previousStamp)
                stampsMonotonic = false;

            previousStamp = chunk.hostSampleIndexOfFirst;
            lo.insert (lo.end(), chunk.data, chunk.data + chunk.numSamples);
        }

        pos += n;
    }

    if (! stampsMonotonic)
        std::printf ("      WARNING: chunk timestamps were not monotonic\n");

    // Worker-thread equivalent: 1024-sample window, 128-sample hop.
    vt::PitchDetectorMPM detector;
    detector.prepare (vt::kAnalysisWindow);
    detector.setSearchRangeHz (searchMinHz, searchMaxHz);

    vt::VoicingGate gate;
    gate.prepare (vt::kAnalysisRate);

    vt::OneEuroFilter smoother;
    smoother.prepare (vt::kAnalysisRate / (double) vt::kAnalysisHop);   // 125 Hz

    std::vector<Frame> frames;
    frames.reserve (lo.size() / (size_t) vt::kAnalysisHop + 1);

    int cursor = 0;

    while (cursor + vt::kAnalysisHop <= (int) lo.size())
    {
        gate.pushHop (&lo[(size_t) cursor], vt::kAnalysisHop);
        cursor += vt::kAnalysisHop;

        if (cursor < vt::kAnalysisWindow)
            continue;

        const auto result = detector.process (&lo[(size_t) (cursor - vt::kAnalysisWindow)],
                                              vt::kAnalysisWindow);

        Frame f;
        f.rawFracMidi   = result.valid ? result.fracMidi : vt::kUnvoiced;
        f.freqHz        = result.valid ? result.frequencyHz : 0.0f;
        f.clarity       = result.valid ? result.clarity : 0.0f;
        f.gatedFracMidi = gate.process (f.rawFracMidi, f.clarity);
        f.smoothed      = smoother.process (f.gatedFracMidi);
        f.rmsDb         = gate.getRmsDb();
        f.gateOpen      = gate.isOpen();

        frames.push_back (f);
    }

    return frames;
}

//==============================================================================
double medianOf (std::vector<double> values)
{
    if (values.empty())
        return 0.0;

    std::sort (values.begin(), values.end());
    return values[values.size() / 2];
}

double centsError (double measuredHz, double referenceHz)
{
    if (! (measuredHz > 0.0) || ! (referenceHz > 0.0))
        return 1.0e9;

    return 1200.0 * std::log2 (measuredHz / referenceHz);
}

/** Frequencies of the last `count` gate-accepted frames. */
std::vector<double> tailVoicedFrequencies (const std::vector<Frame>& frames, size_t count)
{
    std::vector<double> out;

    for (auto it = frames.rbegin(); it != frames.rend() && out.size() < count; ++it)
        if (std::isfinite (it->gatedFracMidi))
            out.push_back ((double) it->freqHz);

    return out;
}

double voicedFraction (const std::vector<Frame>& frames)
{
    if (frames.empty())
        return 0.0;

    size_t voiced = 0;

    for (const auto& f : frames)
        if (std::isfinite (f.gatedFracMidi))
            ++voiced;

    return (double) voiced / (double) frames.size();
}

//==============================================================================
void case1_resamplingRateTrap()
{
    for (const double hostRate : { 44100.0, 48000.0 })
    {
        const auto signal = makeSignal (SignalType::sine, 440.0, hostRate, 2.0, 0.25);
        const auto frames = runChain (signal, hostRate);
        const auto tail   = tailVoicedFrequencies (frames, 100);

        const auto name = std::string ("440 Hz through the chain @ ")
                        + std::to_string ((int) hostRate) + " Hz host";

        if (tail.size() < 50)
        {
            report (name.c_str(), false,
                    fmt ("only %.0f voiced frames — gate never opened", (double) tail.size()));
            continue;
        }

        const auto measured = medianOf (tail);
        const auto cents    = centsError (measured, 440.0);

        report (name.c_str(), std::abs (cents) <= 2.0,
                fmt2 ("measured %.4f Hz, error %+.3f cents (tol 2.0)", measured, cents));
    }
}

void case2_rangeExtremes()
{
    struct Target { const char* label; double hz; };

    const Target targets[] = { { "E2 (82.41 Hz)", 82.41 }, { "A5 (880 Hz)", 880.0 } };

    for (const auto& t : targets)
    {
        const double hostRate = 48000.0;
        const auto signal = makeSignal (SignalType::sine, t.hz, hostRate, 2.0, 0.30);
        const auto frames = runChain (signal, hostRate);
        const auto tail   = tailVoicedFrequencies (frames, 100);

        const auto name = std::string ("range extreme ") + t.label;

        if (tail.size() < 50)
        {
            report (name.c_str(), false,
                    fmt ("only %.0f voiced frames — gate never opened", (double) tail.size()));
            continue;
        }

        const auto measured = medianOf (tail);
        const auto cents    = centsError (measured, t.hz);

        report (name.c_str(), std::abs (cents) <= 5.0,
                fmt2 ("measured %.4f Hz, error %+.3f cents (tol 5.0)", measured, cents));
    }
}

void case3_noOctaveErrorWhenConstrained()
{
    const double hostRate = 48000.0;
    const double trueHz   = 220.0;                      // A3
    const auto   signal   = makeSignal (SignalType::sine, trueHz, hostRate, 2.0, 0.25);

    // (a) The search range sits an octave ABOVE the signal (chart says A4).
    //     The detector must NOT invent 440 Hz.
    {
        const auto frames = runChain (signal, hostRate, 419.0, 880.0);

        double worstCents = 1.0e9;
        size_t octaveErrors = 0;

        for (const auto& f : frames)
        {
            if (! std::isfinite (f.gatedFracMidi))
                continue;

            const auto c = std::abs (centsError (f.freqHz, 440.0));
            worstCents = std::min (worstCents, c);

            if (c < 50.0)
                ++octaveErrors;
        }

        const auto ok = (octaveErrors == 0);

        report ("constrained range: no octave-up error", ok,
                fmt2 ("%.0f frames near 440 Hz, voiced fraction %.3f (want 0 / ~0)",
                      (double) octaveErrors, voicedFraction (frames)));
    }

    // (b) Positive control: the same signal with the range placed correctly
    //     (chart note A3 +/- one octave) must be detected.
    {
        const auto frames = runChain (signal, hostRate, 110.0, 440.0);
        const auto tail   = tailVoicedFrequencies (frames, 100);

        if (tail.size() < 50)
        {
            report ("constrained range: correct octave detected", false,
                    fmt ("only %.0f voiced frames", (double) tail.size()));
        }
        else
        {
            const auto measured = medianOf (tail);
            const auto cents    = centsError (measured, trueHz);

            report ("constrained range: correct octave detected",
                    std::abs (cents) <= 5.0,
                    fmt2 ("measured %.4f Hz, error %+.3f cents (tol 5.0)", measured, cents));
        }
    }
}

void case4_whiteNoiseRejected()
{
    const double hostRate = 48000.0;
    const auto signal = makeSignal (SignalType::noise, 0.0, hostRate, 2.0, 0.20);
    const auto frames = runChain (signal, hostRate);

    size_t clarityAbove = 0;
    double claritySum = 0.0;

    for (const auto& f : frames)
    {
        claritySum += (double) f.clarity;

        if (f.clarity >= vt::VoicingGate::kClarityThreshold)
            ++clarityAbove;
    }

    const auto voiced   = voicedFraction (frames);
    const auto meanClar = frames.empty() ? 0.0 : claritySum / (double) frames.size();
    const auto ok = frames.size() > 50
                 && voiced < 0.15
                 && meanClar < (double) vt::VoicingGate::kClarityThreshold;

    report ("white noise rejected (low clarity -> NaN)", ok,
            fmt2 ("voiced fraction %.3f (want <0.15), mean clarity %.3f (want <0.85)",
                  voiced, meanClar));

    (void) clarityAbove;
}

void case5_silence()
{
    const double hostRate = 48000.0;
    const auto signal = makeSignal (SignalType::silence, 0.0, hostRate, 2.0, 0.0, 0.0);
    const auto frames = runChain (signal, hostRate);

    bool allNaN = true;

    for (const auto& f : frames)
        if (std::isfinite (f.gatedFracMidi) || std::isfinite (f.smoothed))
            allNaN = false;

    const auto ok = frames.size() > 50 && allNaN;

    report ("digital silence -> NaN, no crash", ok,
            fmt2 ("%.0f frames analysed, all-NaN = %.0f", (double) frames.size(), allNaN ? 1.0 : 0.0));
}

//==============================================================================
void case6_supportingInvariants()
{
    // Not in the required list, but these are free and they catch the two
    // silliest possible regressions.

    // 6a. The window-centre helper must scale by hostRate / 16000, not 1.
    {
        const auto centre48 = vt::AnalysisChain::hostSampleAtWindowCentre (0, 48000.0);
        const auto centre44 = vt::AnalysisChain::hostSampleAtWindowCentre (0, 44100.0);
        const bool ok = (centre48 == 1536) && (centre44 == 1411);

        report ("hostSampleAtWindowCentre scaling", ok,
                fmt2 ("48k -> %.0f (want 1536), 44.1k -> %.0f (want 1411)",
                      (double) centre48, (double) centre44));
    }

    // 6b. Key-maxima picking (k = 0.90) must not pick the octave on a
    //     harmonically rich signal, with the search range UNCONSTRAINED.
    {
        const double hostRate = 48000.0;
        const double trueHz   = 146.83;               // D3, a typical chest-voice note
        const auto signal = makeSignal (SignalType::saw, trueHz, hostRate, 2.0, 0.20);
        const auto frames = runChain (signal, hostRate);
        const auto tail   = tailVoicedFrequencies (frames, 100);

        if (tail.size() < 50)
        {
            report ("harmonic-rich saw, free range: no octave error", false,
                    fmt ("only %.0f voiced frames", (double) tail.size()));
        }
        else
        {
            const auto measured = medianOf (tail);
            const auto cents    = centsError (measured, trueHz);

            report ("harmonic-rich saw, free range: no octave error",
                    std::abs (cents) <= 10.0,
                    fmt2 ("measured %.4f Hz, error %+.3f cents (tol 10.0)", measured, cents));
        }
    }

    // 6c. A note held for longer than the 3 s noise-floor window must NOT be
    //     gated off by the floor estimate creeping up to its own level.
    {
        const double hostRate = 48000.0;
        const auto signal = makeSignal (SignalType::sine, 261.63, hostRate, 8.0, 0.25);
        const auto frames = runChain (signal, hostRate);

        // Ignore the lead-in; look only at the last 6 seconds (750 frames).
        const size_t tailFrames = std::min<size_t> (750, frames.size());
        size_t voiced = 0;

        for (size_t i = frames.size() - tailFrames; i < frames.size(); ++i)
            if (std::isfinite (frames[i].gatedFracMidi))
                ++voiced;

        const auto ratio = tailFrames > 0 ? (double) voiced / (double) tailFrames : 0.0;

        report ("8 s sustained note stays voiced", ratio > 0.98,
                fmt2 ("%.3f of the last %.0f frames voiced (want >0.98)",
                      ratio, (double) tailFrames));
    }

    // 6d. The 1-Euro filter must be transparent to NaN and must not smear
    //     across an unvoiced gap.
    {
        vt::OneEuroFilter f;
        f.prepare (125.0);

        for (int i = 0; i < 50; ++i)
            f.process (60.0f);

        const auto beforeGap = f.process (60.0f);
        const auto duringGap = f.process (vt::kUnvoiced);
        const auto afterGap  = f.process (72.0f);

        const bool ok = std::isfinite (beforeGap)
                     && std::abs (beforeGap - 60.0f) < 0.01f
                     && ! std::isfinite (duringGap)
                     && std::abs (afterGap - 72.0f) < 0.01f;    // re-primed, no smearing

        report ("1-Euro filter NaN handling / re-priming", ok,
                fmt2 ("settled %.4f (want 60), after gap %.4f (want 72)",
                      (double) beforeGap, (double) afterGap));
    }
}

} // namespace

//==============================================================================
int main()
{
    std::printf ("vocal-trainer DSP core tests\n");
    std::printf ("----------------------------\n");

    case1_resamplingRateTrap();
    case2_rangeExtremes();
    case3_noOctaveErrorWhenConstrained();
    case4_whiteNoiseRejected();
    case5_silence();
    case6_supportingInvariants();

    std::printf ("----------------------------\n");

    if (failures == 0)
        std::printf ("ALL %d CASES PASSED\n", caseIndex);
    else
        std::printf ("%d of %d CASES FAILED\n", failures, caseIndex);

    return failures == 0 ? 0 : 1;
}
