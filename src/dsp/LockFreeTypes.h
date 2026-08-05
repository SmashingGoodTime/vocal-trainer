#pragma once

/*
    LockFreeTypes.h — the shared type contract between the audio thread, the
    analysis worker thread and the GUI thread.

    THIS FILE IS NORMATIVE. The declarations below are reproduced verbatim from
    the project contract (see DESIGN.md §5, "Thread architecture"). Do not
    redefine these types anywhere else, and do not alter them here.

    Everything in the project lives in namespace vt.
*/

#include <atomic>
#include <cstdint>
#include <limits>

namespace vt
{

//==============================================================================
// ==== SHARED C++ TYPE CONTRACT — VERBATIM. DO NOT EDIT. ======================

    struct AudioChunk {
        int64_t hostSampleIndexOfFirst = 0;  // host-domain sample index of data[0]
        int     numSamples = 0;              // always kChunkSize when pushed
        float   data[128] {};                // 16 kHz mono
    };
    inline constexpr int kChunkSize      = 128;    // 8.0 ms at 16 kHz
    inline constexpr int kAnalysisWindow = 1024;   // 64.0 ms at 16 kHz
    inline constexpr double kAnalysisRate = 16000.0;

    struct PitchEstimate {
        int64_t hostSampleAtWindowCentre = 0;
        float   fracMidi = std::numeric_limits<float>::quiet_NaN();  // NaN == unvoiced/rejected
        float   clarity  = 0.0f;   // NSDF peak, 0..1
        float   rmsDb    = -100.0f;
    };

    struct TransportSnapshot {
        int64_t timeInSamples = 0;
        bool    isPlaying = false;
        double  bpm = 120.0;
        double  sampleRate = 48000.0;
    };

    class TransportSeqlock {   // audio thread publishes, GUI thread reads
        std::atomic<uint32_t> seq { 0 };
        TransportSnapshot data {};
    public:
        void publish (const TransportSnapshot& s) noexcept {
            seq.fetch_add (1, std::memory_order_release);
            data = s;
            seq.fetch_add (1, std::memory_order_release);
        }
        bool read (TransportSnapshot& out) const noexcept {
            const auto a = seq.load (std::memory_order_acquire);
            if (a & 1u) return false;
            out = data;
            return seq.load (std::memory_order_acquire) == a;
        }
    };

// ==== END OF VERBATIM CONTRACT ===============================================
//==============================================================================

/** Depth of the audio-thread -> worker AbstractFifo, in AudioChunks.
    64 chunks == 512 ms of slack (DESIGN.md §5). */
inline constexpr int kChunkFifoCapacity = 64;

/** Depth of the worker -> GUI PitchEstimate AbstractFifo.
    256 estimates == ~2.05 s at 125 estimates/s. */
inline constexpr int kEstimateFifoCapacity = 256;

/** Hop between successive analysis windows, in 16 kHz samples.
    One AudioChunk == one hop == 8 ms == 125 estimates/s. */
inline constexpr int kAnalysisHop = kChunkSize;

/** Convenience: the NaN used to mean "unvoiced / rejected" throughout. */
inline constexpr float kUnvoiced = std::numeric_limits<float>::quiet_NaN();

} // namespace vt
