"""Stage 2 of the offline pipeline: continuous f0 -> discrete note events.

Implements the ten-step recipe from ``docs/DESIGN.md`` section 4, stage 2.

The whole point of this module is to *not* fragment sustained singing.  SwiftF0's
own note segmenter uses an 0.8-semitone split threshold and a 50 ms minimum note
length; operatic vibrato routinely swings +/-60 cents at 5-7 Hz, so a single held
note explodes into a dozen alternating events.  Three mechanisms are needed
together to stop that, and removing any one of them re-breaks it:

  * a 7-frame (~112 ms) median filter on the cents curve,
  * a wider 1.2-semitone running-median deviation threshold,
  * hysteresis: the deviation must persist 6 consecutive frames (~96 ms) before
    a split is committed, and the split is then placed retroactively at the
    frame where the deviation *started*.

Pure numpy.  No madmom (Windows-hostile and non-commercial licensed), no aubio,
no crepe_notes: the algorithm below is a reimplementation from the published
description, not a port of anyone's source.

Public entry points
-------------------
``segment_notes(f0, config=..., spectral_flux=...)`` -> :class:`SegmentationResult`
``build_contour(f0, ...)``                            -> ``chart.Contour``
``spectral_flux_from_audio(audio, sample_rate, ...)`` -> optional librosa helper

The ``build_*`` / ``get_field`` helpers at the bottom are a thin adapter over the
dataclasses in :mod:`vocalchart.chart`; they exist so this module keeps working
whether those dataclasses spell their fields ``start_seconds`` or ``startSeconds``.
:mod:`vocalchart.ultrastar` imports them too.
"""

from __future__ import annotations

import bisect
import dataclasses
import inspect
import warnings as _warnings
from dataclasses import dataclass, field
from typing import Any, Dict, List, Optional, Sequence, Tuple

import numpy as np

from vocalchart.chart import Contour, Note
from vocalchart.f0 import F0Result

__all__ = [
    "SegmentConfig",
    "SegmentationResult",
    "segment_notes",
    "build_contour",
    "spectral_flux_from_audio",
    "frame_level_db_from_audio",
    "build_note",
    "build_phrase",
    "build_contour_object",
    "build_dataclass",
    "get_field",
    "set_fields",
]


# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------


@dataclass
class SegmentConfig:
    """Every threshold in the segmenter, exposed for the validation harness.

    Defaults are the numbers written down in DESIGN.md section 4 stage 2.  The
    handful of values the design doc leaves as "tune it" (onset threshold,
    normalisation reference, merge guard) are called out in their comments.
    """

    # -- step 1: f0 -> fractional MIDI -------------------------------------
    min_hz: float = 60.0                     # DESIGN: constrain SwiftF0 to 60-1100 Hz
    max_hz: float = 1100.0

    # -- step 2: voicing gate ----------------------------------------------
    confidence_threshold: float = 0.50

    # -- step 3: median filter ---------------------------------------------
    median_filter_frames: int = 7            # ~112 ms at a 16 ms hop; must be odd

    # -- step 4: onset signal ----------------------------------------------
    onset_weight_rate: float = 1.0           # |d(cents)/dt| term
    onset_weight_confidence: float = 1.0     # (1 - confidence) term
    onset_weight_flux: float = 1.0           # spectral flux term (only if supplied)
    onset_combine: str = "max"               # "max" (default, see _onset_signal),
                                             # "mean" (weighted mean of the active
                                             # terms) or "sum" (literal DESIGN sum).
                                             # "mean" keeps the threshold meaningful
                                             # whether or not flux is available.
    rate_normalisation: str = "reference"    # "reference" | "peak".  "peak" is
                                             # literal min-max normalisation; it is
                                             # unusable on its own because a file
                                             # containing nothing but vibrato then
                                             # normalises the vibrato to full scale.
    cents_rate_reference: float = 9000.0     # cents/s that maps to 1.0 in "reference"
                                             # mode.  A sung note-to-note step lands
                                             # at 5000-12000 c/s; a wide 6 Hz vibrato
                                             # of +/-80 cents peaks near 3000 c/s
                                             # before filtering.  See the vibrato
                                             # amplitude sweep in test_segment.py.
    flux_normalisation_percentile: float = 99.0
    onset_threshold: float = 0.35            # peak-pick sensitivity, on the combined
                                             # signal (crepe_notes' 0.001 is on a
                                             # differently-scaled signal)
    strong_onset_threshold: float = 0.62     # "a strong onset peak" for step 10
    onset_min_separation_seconds: float = 0.100
    onset_cut_offset_frames: int = 1         # forward difference peaks land on the
                                             # last frame of the *old* note

    # -- step 5: running-median deviation split ----------------------------
    split_deviation_semitones: float = 1.2
    hysteresis_frames: int = 6               # ~96 ms
    min_frames_for_median: int = 3
    # DEVIATION FROM THE LITERAL RECIPE, and the reason is portamento.  A glide
    # trips the running-median rule repeatedly on its way up, producing a
    # staircase of spurious notes.  With `require_settled_split` the split is
    # held back until the pitch stops moving, and is then placed retroactively at
    # the frame where the deviation began - i.e. at the start of the slide, which
    # is where a human would chart the note boundary.  Set False for the literal
    # DESIGN.md behaviour.
    require_settled_split: bool = True
    settle_window_frames: int = 12           # ~192 ms
    settle_max_drift_cents: float = 40.0     # movement below this counts as settled
    settle_max_defer_frames: int = 120       # ~1.9 s; commit regardless past this

    # -- step 6: minimum note length ---------------------------------------
    min_note_duration_seconds: float = 0.120

    # -- step 7: consonant bridging ----------------------------------------
    max_bridged_gap_seconds: float = 0.080

    # -- step 8/9: pitch assignment ----------------------------------------
    core_fraction: float = 0.60              # median of the middle 60%
    pitch_from_smoothed: bool = True

    # -- step 10: merge ----------------------------------------------------
    merge_gap_seconds: float = 0.080
    merge_onset_guard_seconds: float = 0.020

    # -- step 11: silent-note rejection ------------------------------------
    # THE SPURIOUS NOTES ARE REVERB TAILS, NOT BREATH.
    #
    # SwiftF0 tracks the decaying reverb of the PREVIOUS phrase: it is a
    # genuinely pitched signal, so confidence stays high (0.6-0.9) and the
    # plausibility band accepts it. Measured across the 21-song library those
    # tails fall 60-76 dB over 1.7-2.9 s and end between -50 and -87 dBFS, while
    # no verified real note sits more than 23.6 dB below its own song's median
    # note level.
    #
    # This is why every other cue failed: spectral shape cannot see it (the
    # reverb of a voice is voice-shaped), confidence cannot (the detector is
    # correct, not hallucinating), and isolation cannot (a tail is adjacent to
    # the phrase it decays from). Level is the only axis on which a decaying
    # tail is definitionally different from singing.
    #
    # Set to None to disable; chart.json is derived, so rebuilding restores it.
    reject_level_db: Optional[float] = -40.0
    reject_level_reference_percentile: float = 50.0   # of the song's OWN notes
    reject_level_min_notes: int = 20                  # fewer than this: skip

    # -- contour ------------------------------------------------------------
    contour_rate_hz: float = 100.0
    contour_decimals: Optional[int] = 3

    # -- misc ---------------------------------------------------------------
    default_note_type: str = "normal"

    def __post_init__(self) -> None:
        if self.median_filter_frames % 2 == 0:
            self.median_filter_frames += 1
        if self.onset_combine not in ("mean", "sum", "max"):
            raise ValueError("onset_combine must be 'mean', 'sum' or 'max'")
        if self.rate_normalisation not in ("reference", "peak"):
            raise ValueError("rate_normalisation must be 'reference' or 'peak'")
        if not 0.0 < self.core_fraction <= 1.0:
            raise ValueError("core_fraction must be in (0, 1]")


@dataclass
class SegmentationResult:
    """Notes plus every intermediate array, so the harness can plot what happened."""

    notes: List[Any] = field(default_factory=list)      # List[chart.Note]
    contour: Any = None                                  # chart.Contour
    times: np.ndarray = field(default_factory=lambda: np.zeros(0))
    frac_midi: np.ndarray = field(default_factory=lambda: np.zeros(0))
    cents_smoothed: np.ndarray = field(default_factory=lambda: np.zeros(0))
    voiced: np.ndarray = field(default_factory=lambda: np.zeros(0, bool))
    bridged: np.ndarray = field(default_factory=lambda: np.zeros(0, bool))
    onset_signal: np.ndarray = field(default_factory=lambda: np.zeros(0))
    onset_frames: np.ndarray = field(default_factory=lambda: np.zeros(0, int))
    hop_seconds: float = 0.0
    level_reference_db: float = float("nan")   # what step 11 normalised against
    rejected_silent: int = 0                   # notes step 11 dropped
    config: SegmentConfig = field(default_factory=SegmentConfig)


# ---------------------------------------------------------------------------
# Small numeric helpers
# ---------------------------------------------------------------------------


def _nan_median_filter(x: np.ndarray, kernel: int, keep_nan: np.ndarray) -> np.ndarray:
    """NaN-aware running median.  ``keep_nan`` frames are forced back to NaN.

    ``scipy.signal.medfilt`` is not NaN-aware: a single unvoiced frame would
    poison three quarters of a 7-frame window.
    """
    x = np.asarray(x, dtype=float)
    if kernel <= 1 or x.size == 0:
        out = x.copy()
    else:
        half = kernel // 2
        padded = np.pad(x, half, mode="edge")
        idx = np.arange(x.size)[:, None] + np.arange(kernel)[None, :]
        window = padded[idx]
        with _warnings.catch_warnings():
            _warnings.simplefilter("ignore", RuntimeWarning)
            out = np.nanmedian(window, axis=1)
    out = np.asarray(out, dtype=float)
    out[keep_nan] = np.nan
    return out


def _interpolate_gaps(values: np.ndarray, valid: np.ndarray) -> np.ndarray:
    """Linear interpolation of ``values`` over the ``~valid`` frames (edge-held)."""
    out = np.array(values, dtype=float, copy=True)
    if not np.any(valid):
        return np.zeros_like(out)
    idx = np.arange(out.size)
    out[:] = np.interp(idx, idx[valid], values[valid])
    return out


def _runs(mask: np.ndarray) -> List[Tuple[int, int]]:
    """Inclusive (start, end) index pairs for each contiguous True run."""
    mask = np.asarray(mask, dtype=bool)
    if mask.size == 0:
        return []
    edges = np.diff(mask.astype(np.int8))
    starts = list(np.flatnonzero(edges == 1) + 1)
    ends = list(np.flatnonzero(edges == -1))
    if mask[0]:
        starts.insert(0, 0)
    if mask[-1]:
        ends.append(mask.size - 1)
    return list(zip(starts, ends))


def _pick_peaks(x: np.ndarray, height: float, min_distance: int) -> np.ndarray:
    """Plateau-aware local-maximum picking with greedy amplitude suppression.

    Deliberately dependency-free (no scipy) so this module imports on a bare
    numpy install and so the peak semantics are ours to keep stable.
    """
    n = x.size
    if n < 3:
        return np.zeros(0, dtype=int)
    candidates: List[int] = []
    i = 1
    while i < n - 1:
        if x[i] < height or not (x[i] > x[i - 1]):
            i += 1
            continue
        j = i
        while j + 1 < n and x[j + 1] == x[i]:
            j += 1
        if j + 1 < n and x[j + 1] < x[i]:
            candidates.append((i + j) // 2)
        i = j + 1
    if not candidates:
        return np.zeros(0, dtype=int)
    min_distance = max(1, int(min_distance))
    chosen: List[int] = []
    for k in sorted(candidates, key=lambda c: -float(x[c])):
        if all(abs(k - c) >= min_distance for c in chosen):
            chosen.append(k)
    return np.array(sorted(chosen), dtype=int)


def _normalise_peak(x: np.ndarray) -> np.ndarray:
    finite = np.isfinite(x)
    if not np.any(finite):
        return np.zeros_like(x)
    lo = float(np.min(x[finite]))
    hi = float(np.max(x[finite]))
    if hi - lo <= 1e-12:
        return np.zeros_like(x)
    out = (x - lo) / (hi - lo)
    out[~finite] = 0.0
    return np.clip(out, 0.0, 1.0)


def _core_median(values: np.ndarray, core_fraction: float) -> float:
    """Median of the middle ``core_fraction`` of a time-ordered value array.

    Step 8: discards the leading scoop and the trailing fall-off.
    """
    values = np.asarray(values, dtype=float)
    values = values[np.isfinite(values)]
    if values.size == 0:
        return float("nan")
    if values.size < 3 or core_fraction >= 1.0:
        return float(np.median(values))
    drop = (1.0 - core_fraction) / 2.0
    lo = int(np.floor(values.size * drop))
    hi = values.size - lo
    if hi - lo < 1:
        return float(np.median(values))
    return float(np.median(values[lo:hi]))


# ---------------------------------------------------------------------------
# Steps 1-4
# ---------------------------------------------------------------------------


def _hop_seconds(f0: F0Result) -> float:
    hop = float(get_field(f0, "hop_seconds", 0.0) or 0.0)
    if hop > 0.0:
        return hop
    times = np.asarray(get_field(f0, "times"), dtype=float)
    if times.size >= 2:
        return float(np.median(np.diff(times)))
    return 0.016


def f0_to_frac_midi(hz: np.ndarray, tuning_hz: float) -> np.ndarray:
    """Step 1: ``m = 69 + 12*log2(f / tuning_hz)``, NaN where f is unusable."""
    hz = np.asarray(hz, dtype=float)
    out = np.full(hz.shape, np.nan, dtype=float)
    usable = np.isfinite(hz) & (hz > 0.0)
    with np.errstate(divide="ignore", invalid="ignore"):
        out[usable] = 69.0 + 12.0 * np.log2(hz[usable] / float(tuning_hz))
    return out


def _onset_signal(
    cents_smoothed: np.ndarray,
    confidence: np.ndarray,
    voiced: np.ndarray,
    hop: float,
    flux: Optional[np.ndarray],
    cfg: SegmentConfig,
) -> np.ndarray:
    """Step 4: ``normalise(|d(cents)/dt|) + (1 - confidence) + normalise(flux)``."""
    n = cents_smoothed.size

    # Forward difference so a step at frame j->j+1 produces a single peak at j.
    filled = _interpolate_gaps(cents_smoothed, voiced)
    if n >= 2:
        rate = np.abs(np.diff(filled, append=filled[-1])) / max(hop, 1e-9)
    else:
        rate = np.zeros(n)
    rate[~voiced] = 0.0
    if cfg.rate_normalisation == "peak":
        rate_term = _normalise_peak(rate)
    else:
        rate_term = np.clip(rate / max(cfg.cents_rate_reference, 1e-9), 0.0, 1.0)

    conf_term = np.clip(1.0 - np.asarray(confidence, dtype=float), 0.0, 1.0)

    terms = [(cfg.onset_weight_rate, rate_term), (cfg.onset_weight_confidence, conf_term)]

    if flux is not None:
        flux = np.asarray(flux, dtype=float)
        pct = float(np.percentile(flux[np.isfinite(flux)], cfg.flux_normalisation_percentile)) \
            if np.any(np.isfinite(flux)) else 0.0
        flux_term = np.clip(flux / pct, 0.0, 1.0) if pct > 1e-12 else _normalise_peak(flux)
        terms.append((cfg.onset_weight_flux, flux_term))

    # "max" is the DEFAULT for a reason. The three terms detect DIFFERENT,
    # independent kinds of onset, and any one of them alone is a real onset:
    # a pitch jump, a confidence drop (consonant), or a spectral-flux spike
    # (a new syllable at unchanged pitch).
    #
    # Averaging them actively destroys the third case. A new syllable sung at
    # constant pitch gives rate~0, conf~0, flux~1.0 -> mean 0.33, which sits
    # BELOW the 0.45 peak-pick threshold, so the onset never fires. Measured on
    # the Mr Brightside lead vocal (a near-monotone verse) that fused 94 s of
    # singing into 74 notes on a single pitch, some over 4 s long: the pitch was
    # right and the rhythm was gone.
    if cfg.onset_combine == "max":
        total = np.zeros(n, dtype=float)
        for weight, term in terms:
            if weight > 0.0:
                total = np.maximum(total, weight * term)
        return total

    total = np.zeros(n, dtype=float)
    weight_sum = 0.0
    for weight, term in terms:
        if weight == 0.0:
            continue
        total += weight * term
        weight_sum += weight
    if cfg.onset_combine == "mean" and weight_sum > 0.0:
        total /= weight_sum
    return total


def _align_flux(
    flux: Optional[Sequence[float]],
    flux_times: Optional[Sequence[float]],
    times: np.ndarray,
) -> Optional[np.ndarray]:
    """Resample an optional precomputed flux array onto the f0 frame grid."""
    if flux is None:
        return None
    flux = np.asarray(flux, dtype=float)
    if flux.size == 0:
        return None
    if flux_times is not None:
        flux_times = np.asarray(flux_times, dtype=float)
        if flux_times.size != flux.size:
            raise ValueError("spectral_flux_times must match spectral_flux in length")
        return np.interp(times, flux_times, flux)
    if flux.size == times.size:
        return flux
    src = np.linspace(0.0, 1.0, flux.size)
    dst = np.linspace(0.0, 1.0, times.size)
    return np.interp(dst, src, flux)


# ---------------------------------------------------------------------------
# Steps 5-7: cutting
# ---------------------------------------------------------------------------


def _active_mask(voiced: np.ndarray, hop: float, cfg: SegmentConfig) -> Tuple[np.ndarray, np.ndarray]:
    """Step 7: bridge unvoiced gaps shorter than ``max_bridged_gap_seconds``.

    Returns ``(active, bridged)``.  Only *interior* gaps (flanked by voiced frames
    on both sides) are bridged - leading and trailing silence stays silence.
    """
    bridged = np.zeros_like(voiced, dtype=bool)
    max_frames = int(np.floor(cfg.max_bridged_gap_seconds / max(hop, 1e-9) + 1e-9))
    if max_frames > 0:
        for start, end in _runs(~voiced):
            if start == 0 or end == voiced.size - 1:
                continue                       # not interior
            if (end - start + 1) <= max_frames:
                bridged[start:end + 1] = True
    return (voiced | bridged), bridged


def _still_gliding(cents: np.ndarray, i: int, seg_start: int, dev_run: int,
                   cfg: SegmentConfig) -> bool:
    """True while the pitch is mid-slide and a split should be deferred.

    Compares the current frame against the last finite frame roughly
    ``settle_window_frames`` back.  A settled note drifts by ~nothing over that
    window; a portamento drifts by tens of cents.  Vibrato never reaches this
    code path because it does not trip the 1.2-semitone deviation rule.
    """
    if not cfg.require_settled_split or dev_run >= cfg.settle_max_defer_frames:
        return False
    window_start = max(seg_start, i - cfg.settle_window_frames + 1)
    reference = np.nan
    for j in range(window_start, i + 1):
        if np.isfinite(cents[j]):
            reference = cents[j]
            break
    if not np.isfinite(reference) or not np.isfinite(cents[i]):
        return False
    return abs(float(cents[i]) - float(reference)) > cfg.settle_max_drift_cents


def _cut_run(
    run_start: int,
    run_end: int,
    cents: np.ndarray,
    cut_frames: set,
    cfg: SegmentConfig,
) -> List[Tuple[int, int]]:
    """Step 5 applied inside one active run.  Returns inclusive frame ranges.

    Two independent cut sources:
      * onset peaks (already offset and filtered to voiced frames by the caller),
      * running-median deviation > ``split_deviation_semitones`` sustained for
        ``hysteresis_frames`` consecutive frames, committed *retroactively* at the
        first deviating frame - or, if the split had to be deferred because the
        pitch was still sliding, at the frame where it arrived.
    """
    threshold_cents = cfg.split_deviation_semitones * 100.0
    boundaries: List[int] = [run_start]

    sorted_acc: List[float] = []
    dev_run = 0
    dev_start: Optional[int] = None
    deferred = False
    seg_start = run_start

    i = run_start
    while i <= run_end:
        if i > seg_start and i in cut_frames:
            boundaries.append(i)
            seg_start = i
            sorted_acc = []
            dev_run = 0
            dev_start = None
            deferred = False

        c = cents[i]
        if np.isfinite(c):
            if len(sorted_acc) >= cfg.min_frames_for_median:
                m = len(sorted_acc)
                median = (sorted_acc[m // 2] if m % 2
                          else 0.5 * (sorted_acc[m // 2 - 1] + sorted_acc[m // 2]))
                if abs(c - median) > threshold_cents:
                    if dev_run == 0:
                        dev_start = i
                    dev_run += 1
                    if dev_run >= cfg.hysteresis_frames and _still_gliding(
                            cents, i, seg_start, dev_run, cfg):
                        # The pitch is still travelling: this is a portamento, not
                        # a note boundary.  Hold the split until it arrives.
                        deferred = True
                        bisect.insort(sorted_acc, float(c))
                        i += 1
                        continue
                    if dev_run >= cfg.hysteresis_frames:
                        b = max(int(dev_start), seg_start + 1)
                        if deferred:
                            # The boundary belongs where the slide ended, not where
                            # it began - otherwise the remainder of the glide forms
                            # a third, spurious note.
                            b = max(b, i - cfg.settle_window_frames + 1)
                        boundaries.append(b)
                        seg_start = b
                        window = cents[b:i + 1]
                        sorted_acc = sorted(float(v) for v in window if np.isfinite(v))
                        dev_run = 0
                        dev_start = None
                        deferred = False
                        i += 1
                        continue
                else:
                    dev_run = 0
                    dev_start = None
                    deferred = False
            bisect.insort(sorted_acc, float(c))
        i += 1

    boundaries = sorted(set(boundaries))
    segments: List[Tuple[int, int]] = []
    for k, b in enumerate(boundaries):
        end = (boundaries[k + 1] - 1) if k + 1 < len(boundaries) else run_end
        if end >= b:
            segments.append((b, end))
    return segments


# ---------------------------------------------------------------------------
# Steps 8-10: pitch, snap, merge
# ---------------------------------------------------------------------------


@dataclass
class _Candidate:
    frames: np.ndarray            # frame indices belonging to this note
    start_seconds: float = 0.0
    duration_seconds: float = 0.0
    pitch_cents: float = float("nan")
    midi: int = 0
    cents_offset: float = 0.0
    confidence: float = 0.0


def _measure(
    frames: np.ndarray,
    times: np.ndarray,
    hop: float,
    pitch_cents_curve: np.ndarray,
    confidence: np.ndarray,
    cfg: SegmentConfig,
) -> _Candidate:
    cand = _Candidate(frames=frames)
    first, last = int(frames[0]), int(frames[-1])
    cand.start_seconds = float(times[first])
    cand.duration_seconds = float(times[last] - times[first] + hop)

    values = pitch_cents_curve[frames]
    values = values[np.isfinite(values)]
    cand.pitch_cents = _core_median(values, cfg.core_fraction)      # step 8

    if np.isfinite(cand.pitch_cents):                               # step 9
        cand.midi = int(np.round(cand.pitch_cents / 100.0))
        cand.cents_offset = float(cand.pitch_cents - cand.midi * 100.0)
    else:
        cand.midi = 0
        cand.cents_offset = 0.0

    conf = np.asarray(confidence, dtype=float)[frames]
    conf = conf[np.isfinite(conf)]
    cand.confidence = float(np.mean(conf)) if conf.size else 0.0
    return cand


def _reject_silent(
    candidates: List["_Candidate"],
    level_db: Optional[np.ndarray],
    cfg: SegmentConfig,
) -> Tuple[List["_Candidate"], float, int]:
    """Step 11: drop candidates whose own audio never rises off the floor.

    The statistic is the LOUDEST frame in the note, NOT the median. A real
    note's decaying tail drags its own median down, so a median-based rule
    deletes exactly the long sustained notes the highway most wants to keep
    (measured: median-based removes 442 notes and destroys 95 audible ones;
    max-based removes 307 and destroys 23).

    RMS, not sample peak: the only two marginal removals in the whole library
    are ~20 ms click transients sitting on a -70 dBFS reverb floor, and a peak
    statistic would keep both.

    The reference is the median of that statistic across THIS song's own
    candidates, which makes the test exactly gain-invariant (verified: -40 to
    +6 dB of stem gain gives byte-identical output) and makes it fail SAFE - on
    a noisy or badly separated stem every note's floor rises together, the
    reference rises with it, and the rule degrades to a no-op rather than
    eating the chart.
    """
    if level_db is None or cfg.reject_level_db is None:
        return candidates, float("nan"), 0

    if len(candidates) < cfg.reject_level_min_notes:
        return candidates, float("nan"), 0

    peaks = np.array([float(np.max(level_db[c.frames])) if c.frames.size else -np.inf
                      for c in candidates], dtype=float)

    if not np.all(np.isfinite(peaks)):
        return candidates, float("nan"), 0

    reference = float(np.percentile(peaks, cfg.reject_level_reference_percentile))
    keep = (peaks - reference) >= cfg.reject_level_db

    return ([c for c, k in zip(candidates, keep) if k],
            reference,
            int(np.count_nonzero(~keep)))


def _merge_candidates(
    candidates: List[_Candidate],
    times: np.ndarray,
    hop: float,
    pitch_cents_curve: np.ndarray,
    confidence: np.ndarray,
    onset_frames: np.ndarray,
    onset_signal: np.ndarray,
    cfg: SegmentConfig,
) -> List[_Candidate]:
    """Step 10: merge adjacent same-semitone notes < 80 ms apart, unless a strong
    onset peak sits between them."""
    if not candidates:
        return []

    strong_times = np.array(
        [times[int(fr)] for fr in onset_frames
         if onset_signal[int(fr)] >= cfg.strong_onset_threshold],
        dtype=float,
    )

    merged: List[_Candidate] = [candidates[0]]
    for nxt in candidates[1:]:
        prev = merged[-1]
        gap = nxt.start_seconds - (prev.start_seconds + prev.duration_seconds)
        same_pitch = prev.midi == nxt.midi
        if same_pitch and gap < cfg.merge_gap_seconds:
            lo = prev.start_seconds + prev.duration_seconds - cfg.merge_onset_guard_seconds
            hi = nxt.start_seconds + cfg.merge_onset_guard_seconds
            blocked = bool(np.any((strong_times >= lo) & (strong_times <= hi)))
            if not blocked:
                frames = np.concatenate([prev.frames, nxt.frames])
                merged[-1] = _measure(frames, times, hop, pitch_cents_curve,
                                      confidence, cfg)
                continue
        merged.append(nxt)
    return merged


# ---------------------------------------------------------------------------
# Contour
# ---------------------------------------------------------------------------


def build_contour(
    f0: F0Result,
    rate_hz: float = 100.0,
    start_seconds: float = 0.0,
    confidence_threshold: float = 0.50,
    decimals: Optional[int] = 3,
) -> Any:
    """The reference singer's raw fractional-MIDI curve, resampled to ``rate_hz``.

    Nearest-source-frame sampling (not interpolation) so that ``None`` lands on
    exactly the frames SwiftF0 called unvoiced.  ``chart.contour`` is the ghost
    line behind the note bars; it must not invent pitch inside a consonant.
    """
    times = np.asarray(get_field(f0, "times"), dtype=float)
    hz = np.asarray(get_field(f0, "hz"), dtype=float)
    conf = np.asarray(get_field(f0, "confidence"), dtype=float)
    tuning = float(get_field(f0, "tuning_hz", 440.0) or 440.0)

    frac = f0_to_frac_midi(hz, tuning)
    voiced = np.isfinite(frac) & (conf >= confidence_threshold)

    frac_out: List[Optional[float]] = []
    conf_out: List[float] = []
    if times.size:
        step = 1.0 / float(rate_hz)
        n_out = int(np.floor((times[-1] - start_seconds) / step)) + 1
        n_out = max(n_out, 0)
        out_times = start_seconds + np.arange(n_out) * step
        nearest = np.clip(np.searchsorted(times, out_times), 0, times.size - 1)
        left = np.clip(nearest - 1, 0, times.size - 1)
        pick_left = np.abs(times[left] - out_times) <= np.abs(times[nearest] - out_times)
        nearest = np.where(pick_left, left, nearest)
        for idx in nearest:
            idx = int(idx)
            c = float(conf[idx]) if np.isfinite(conf[idx]) else 0.0
            if voiced[idx]:
                v = float(frac[idx])
                frac_out.append(round(v, decimals) if decimals is not None else v)
            else:
                frac_out.append(None)
            conf_out.append(round(c, decimals) if decimals is not None else c)

    return build_contour_object(
        rate_hz=float(rate_hz),
        start_seconds=float(start_seconds),
        frac_midi=frac_out,
        confidence=conf_out,
    )


# ---------------------------------------------------------------------------
# Main entry point
# ---------------------------------------------------------------------------


def segment_notes(
    f0: F0Result,
    config: Optional[SegmentConfig] = None,
    spectral_flux: Optional[Sequence[float]] = None,
    spectral_flux_times: Optional[Sequence[float]] = None,
    level_db: Optional[Sequence[float]] = None,
    level_db_times: Optional[Sequence[float]] = None,
) -> SegmentationResult:
    """Run the full ten-step segmentation.

    Parameters
    ----------
    f0
        Stage 1 output: ``times``, ``hz``, ``confidence``, ``hop_seconds``,
        ``tuning_hz``.
    config
        :class:`SegmentConfig`; defaults are the DESIGN.md numbers.
    spectral_flux
        Optional precomputed onset-strength envelope.  Absent, the onset signal
        degrades gracefully to the first two terms (rate + inverse confidence).
    spectral_flux_times
        Frame times for ``spectral_flux``.  If omitted the array is assumed to be
        either frame-aligned with ``f0`` or uniformly resampleable onto it.
    """
    cfg = config or SegmentConfig()

    times = np.asarray(get_field(f0, "times"), dtype=float)
    hz = np.asarray(get_field(f0, "hz"), dtype=float)
    conf = np.asarray(get_field(f0, "confidence"), dtype=float)
    tuning = float(get_field(f0, "tuning_hz", 440.0) or 440.0)
    hop = _hop_seconds(f0)

    if not (times.size == hz.size == conf.size):
        raise ValueError("F0Result times/hz/confidence must be the same length")

    result = SegmentationResult(hop_seconds=hop, config=cfg, times=times)

    if times.size == 0:
        result.contour = build_contour(f0, cfg.contour_rate_hz,
                                       confidence_threshold=cfg.confidence_threshold,
                                       decimals=cfg.contour_decimals)
        return result

    # Step 1 -----------------------------------------------------------------
    frac = f0_to_frac_midi(hz, tuning)

    # Step 2 -----------------------------------------------------------------
    voiced = (np.isfinite(frac)
              & (conf >= cfg.confidence_threshold)
              & (hz >= cfg.min_hz) & (hz <= cfg.max_hz))

    # Step 3 -----------------------------------------------------------------
    cents_raw = frac * 100.0
    cents_raw = np.where(voiced, cents_raw, np.nan)
    cents_smoothed = _nan_median_filter(cents_raw, cfg.median_filter_frames, ~voiced)

    # Step 4 -----------------------------------------------------------------
    flux = _align_flux(spectral_flux, spectral_flux_times, times)
    onset = _onset_signal(cents_smoothed, conf, voiced, hop, flux, cfg)
    onset_frames = _pick_peaks(
        onset,
        cfg.onset_threshold,
        int(round(cfg.onset_min_separation_seconds / max(hop, 1e-9))),
    )

    # Step 7 (before 5: bridged frames must not be cut at) --------------------
    active, bridged = _active_mask(voiced, hop, cfg)

    # Onset peaks inside a bridged consonant gap are suppressed - that is the
    # whole point of bridging.  Peaks are shifted forward so the boundary lands
    # on the first frame of the new note.
    cut_frames = set()
    for fr in onset_frames:
        target = int(fr) + cfg.onset_cut_offset_frames
        if 0 <= target < times.size and not bridged[target]:
            cut_frames.add(target)

    # Step 5 -----------------------------------------------------------------
    pitch_curve = cents_smoothed if cfg.pitch_from_smoothed else cents_raw
    segments: List[Tuple[int, int]] = []
    for run_start, run_end in _runs(active):
        segments.extend(_cut_run(run_start, run_end, cents_smoothed, cut_frames, cfg))

    # Step 6 -----------------------------------------------------------------
    candidates: List[_Candidate] = []
    for seg_start, seg_end in segments:
        frames = np.arange(seg_start, seg_end + 1)
        if not np.any(np.isfinite(pitch_curve[frames])):
            continue
        duration = float(times[seg_end] - times[seg_start] + hop)
        if duration + 1e-9 < cfg.min_note_duration_seconds:
            continue
        candidates.append(_measure(frames, times, hop, pitch_curve, conf, cfg))

    # Step 10 ----------------------------------------------------------------
    candidates = _merge_candidates(candidates, times, hop, pitch_curve, conf,
                                   onset_frames, onset, cfg)

    # Step 11 ----------------------------------------------------------------
    # Placed AFTER the merge and BEFORE note building, so it only ever deletes
    # whole candidates. It never touches the cents curve, the onset signal, the
    # cut logic or the merge, so it cannot re-break the vibrato or
    # syllable-fusion fixes in DESIGN.md 11.5 - the survivors are a strict
    # subset with identical boundaries.
    level = _align_flux(level_db, level_db_times, times)
    candidates, level_reference_db, rejected_silent = _reject_silent(candidates, level, cfg)
    result.level_reference_db = level_reference_db
    result.rejected_silent = rejected_silent

    notes = []
    for i, cand in enumerate(candidates):
        notes.append(build_note(
            id=i,
            start_seconds=round(cand.start_seconds, 6),
            duration_seconds=round(cand.duration_seconds, 6),
            midi=int(cand.midi),
            cents_offset=round(float(cand.cents_offset), 2),
            type=cfg.default_note_type,
            syllable=None,
            syllable_role="none",
            phrase_id=None,
            confidence=round(float(cand.confidence), 4),
        ))

    result.notes = notes
    result.frac_midi = frac
    result.cents_smoothed = cents_smoothed
    result.voiced = voiced
    result.bridged = bridged
    result.onset_signal = onset
    result.onset_frames = onset_frames
    result.contour = build_contour(f0, cfg.contour_rate_hz,
                                   confidence_threshold=cfg.confidence_threshold,
                                   decimals=cfg.contour_decimals)
    return result


def frame_level_db_from_audio(
    audio: np.ndarray,
    sample_rate: float,
    times: Optional[Sequence[float]] = None,
    hop_length: int = 256,
    frame_length: int = 512,
) -> Tuple[np.ndarray, np.ndarray]:
    """Per-frame RMS in dBFS on the f0 grid, for the step-11 silent-note gate.

    512 samples at 16 kHz is 32 ms - the nearest power of two to the runtime
    VoicingGate's 20 ms RMS window - and hop_length 256 IS the 16 ms f0 hop, so
    the interpolation onto `times` is a no-op in the normal case.
    """
    import librosa  # noqa: PLC0415 - deliberately lazy

    rms = librosa.feature.rms(y=np.asarray(audio, dtype=float),
                              frame_length=frame_length, hop_length=hop_length,
                              center=True)[0]
    level = 20.0 * np.log10(np.maximum(rms, 1e-10))
    level_times = librosa.frames_to_time(np.arange(level.size), sr=sample_rate,
                                         hop_length=hop_length)

    if times is not None:
        times = np.asarray(times, dtype=float)
        return np.interp(times, level_times, level), times

    return level, level_times


def spectral_flux_from_audio(
    audio: np.ndarray,
    sample_rate: float,
    times: Optional[Sequence[float]] = None,
    hop_length: int = 256,
) -> Tuple[np.ndarray, np.ndarray]:
    """Optional convenience: half-wave-rectified spectral flux via librosa.

    librosa is an offline-pipeline-only dependency (ISC licensed) and is imported
    lazily so ``segment.py`` stays importable on a bare numpy install.  Returns
    ``(flux, flux_times)`` suitable for :func:`segment_notes`.
    """
    import librosa  # noqa: PLC0415 - deliberately lazy

    # aggregate=median and lag=3 are BOTH load-bearing, and were chosen against
    # ground truth rather than by taste.
    #
    # Vibrato sweeps the spectrum at 4.5-6.5 Hz, and plain mean-aggregated flux
    # reads every one of those sweeps as an onset. Measured on a synthetic vocal
    # with SEVEN known notes and 60 cents of vibrato: default onset_strength
    # produced 18 notes, fragmenting every sustained note into ~3. The median
    # aggregate rejects the narrow-band vibrato sweep (it is not a broadband
    # event), and lag=3 compares frames ~48 ms apart rather than adjacent ones,
    # so a slow frequency glide does not register while a syllable attack does.
    # With both, the same synthetic returns exactly 7.
    #
    # Do not "simplify" this back to the default: it silently re-breaks every
    # held note in the library, and the failure looks like a UI bug rather than
    # a segmentation one.
    flux = librosa.onset.onset_strength(y=np.asarray(audio, dtype=float),
                                        sr=sample_rate, hop_length=hop_length,
                                        lag=3, aggregate=np.median)
    flux_times = librosa.frames_to_time(np.arange(flux.size), sr=sample_rate,
                                        hop_length=hop_length)
    if times is not None:
        times = np.asarray(times, dtype=float)
        return np.interp(times, flux_times, flux), times
    return flux, flux_times


# ---------------------------------------------------------------------------
# chart.py dataclass adapter
# ---------------------------------------------------------------------------
#
# vocalchart.chart is owned by another module; DESIGN.md section 3 specifies the
# JSON in camelCase while Python dataclasses conventionally use snake_case.  This
# adapter maps a single canonical (snake_case) vocabulary onto whichever spelling
# the dataclasses actually use, so a naming choice made elsewhere cannot silently
# break this module.  It also tolerates fields that simply do not exist.

_ALIASES: Dict[str, Tuple[str, ...]] = {
    "id": ("id", "note_id", "noteId", "index"),
    "start_seconds": ("start_seconds", "startSeconds", "start"),
    "end_seconds": ("end_seconds", "endSeconds", "end"),
    "duration_seconds": ("duration_seconds", "durationSeconds", "duration"),
    "midi": ("midi", "midi_note", "midiNote", "pitch"),
    "cents_offset": ("cents_offset", "centsOffset"),
    "type": ("type", "note_type", "noteType", "kind"),
    "syllable": ("syllable",),
    "syllable_role": ("syllable_role", "syllableRole", "role"),
    "phrase_id": ("phrase_id", "phraseId"),
    "confidence": ("confidence",),
    "text": ("text",),
    "note_ids": ("note_ids", "noteIds"),
    "rate_hz": ("rate_hz", "rateHz"),
    "frac_midi": ("frac_midi", "fracMidi"),
    "notes": ("notes",),
    "phrases": ("phrases",),
    "contour": ("contour",),
    "warnings": ("warnings",),
    "times": ("times", "time", "t"),
    "hz": ("hz", "f0", "frequency"),
    "hop_seconds": ("hop_seconds", "hopSeconds", "hop"),
    "tuning_hz": ("tuning_hz", "tuningHz", "tuning"),
}


def _init_field_names(cls: Any) -> Optional[set]:
    if dataclasses.is_dataclass(cls):
        return {f.name for f in dataclasses.fields(cls) if f.init}
    try:
        params = inspect.signature(cls.__init__).parameters
    except (TypeError, ValueError):
        return None
    names = {n for n, p in params.items() if n != "self"
             and p.kind in (p.POSITIONAL_OR_KEYWORD, p.KEYWORD_ONLY)}
    return names or None


def _resolve(names: Optional[set], canonical: str) -> Optional[str]:
    if names is None:
        return canonical
    for alias in _ALIASES.get(canonical, (canonical,)):
        if alias in names:
            return alias
    return None


def build_dataclass(cls: Any, **canonical_kwargs: Any) -> Any:
    """Construct ``cls``, translating canonical field names and dropping unknowns."""
    names = _init_field_names(cls)
    kwargs: Dict[str, Any] = {}
    for key, value in canonical_kwargs.items():
        resolved = _resolve(names, key)
        if resolved is not None:
            kwargs[resolved] = value
    return cls(**kwargs)


def get_field(obj: Any, canonical: str, default: Any = None) -> Any:
    """Read a canonical field from an object that may spell it either way."""
    for alias in _ALIASES.get(canonical, (canonical,)):
        if hasattr(obj, alias):
            return getattr(obj, alias)
    if isinstance(obj, dict):
        for alias in _ALIASES.get(canonical, (canonical,)):
            if alias in obj:
                return obj[alias]
    return default


def set_fields(obj: Any, **canonical_kwargs: Any) -> Any:
    """``dataclasses.replace`` with canonical field-name translation."""
    names = _init_field_names(type(obj))
    kwargs: Dict[str, Any] = {}
    for key, value in canonical_kwargs.items():
        resolved = _resolve(names, key)
        if resolved is not None:
            kwargs[resolved] = value
    if dataclasses.is_dataclass(obj):
        return dataclasses.replace(obj, **kwargs)
    import copy
    clone = copy.copy(obj)
    for key, value in kwargs.items():
        setattr(clone, key, value)
    return clone


def build_note(**kwargs: Any) -> Any:
    return build_dataclass(Note, **kwargs)


def build_contour_object(**kwargs: Any) -> Any:
    return build_dataclass(Contour, **kwargs)


def build_phrase(**kwargs: Any) -> Any:
    from vocalchart.chart import Phrase  # local import: only ultrastar needs it
    return build_dataclass(Phrase, **kwargs)
