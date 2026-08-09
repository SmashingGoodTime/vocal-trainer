#pragma once

/*
    LockFreeTypes.h — the shared type contract between the audio thread, the
    analysis worker thread and the GUI thread.

    THIS FILE IS NORMATIVE. The declarations below are reproduced verbatim from
    the project contract (see DESIGN.md §5, "Thread architecture"). Do not
    redefine these types anywhere else, and do not alter them here.

    TransportSnapshot and TransportSeqlock were part of that contract and have
    been REMOVED. They existed to carry a plugin host's playhead to the GUI, and
    the app is standalone only: the backing track owns the clock, and every
    timestamp is expressed in the free-running device-sample domain below.

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

// ==== END OF VERBATIM CONTRACT ===============================================
//==============================================================================

/** Depth of the audio-thread -> worker AbstractFifo, in AudioChunks.
    64 chunks == 512 ms of slack (DESIGN.md §5). */
inline constexpr int kChunkFifoCapacity = 64;

/** Depth of the worker -> GUI PitchEstimate AbstractFifo.
    512 estimates == ~4.1 s at 125 estimates/s — enough slack that a GUI stall
    (window drag, modal dialog) drops nothing. AudioEngine sizes its FIFO and
    storage from this; do not redeclare it there. */
inline constexpr int kEstimateFifoCapacity = 512;

/** Hop between successive analysis windows, in 16 kHz samples.
    One AudioChunk == one hop == 8 ms == 125 estimates/s. */
inline constexpr int kAnalysisHop = kChunkSize;

/** Convenience: the NaN used to mean "unvoiced / rejected" throughout. */
inline constexpr float kUnvoiced = std::numeric_limits<float>::quiet_NaN();

} // namespace vt
