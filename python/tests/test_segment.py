"""Regression tests for the note segmenter (DESIGN.md section 4, stage 2).

These are not smoke tests.  Each one encodes a failure mode that a naive
segmenter actually exhibits:

  * ``test_sustained_vibrato_produces_exactly_one_note`` is the single most
    important test in the repository.  SwiftF0's stock 0.8-semitone split
    threshold is narrower than operatic vibrato excursion, so a held note
    shatters into a dozen alternating events.  If this test ever fails, the
    highway is unusable regardless of what else works.
  * the arpeggio test guards the opposite failure: over-smoothing until real
    transitions disappear.
  * the gap tests pin the consonant-bridging boundary at 80 ms.
  * the portamento test guards against the running-median split rule chopping a
    glide into a staircase.
  * the cents-offset test guards the "the reference singer was 30 cents flat
    here" feedback path.

All fixtures are synthetic ``F0Result`` arrays - no audio files, no SwiftF0.
"""

from __future__ import annotations

import math

import numpy as np
import pytest

from vocalchart.f0 import F0Result
from vocalchart.segment import (
    SegmentConfig,
    build_contour,
    build_dataclass,
    get_field,
    segment_notes,
)

HOP = 0.016          # SwiftF0's hop, per DESIGN.md stage 1
TUNING = 440.0


# ---------------------------------------------------------------------------
# Synthetic f0 helpers
# ---------------------------------------------------------------------------


def midi_to_hz(midi, tuning_hz: float = TUNING):
    return tuning_hz * np.power(2.0, (np.asarray(midi, dtype=float) - 69.0) / 12.0)


def make_f0(hz, confidence=0.95, hop: float = HOP, tuning_hz: float = TUNING) -> F0Result:
    """Build an F0Result directly, the way stage 1 would have emitted it."""
    hz = np.asarray(hz, dtype=float)
    conf = (np.full(hz.shape, float(confidence))
            if np.isscalar(confidence) else np.asarray(confidence, dtype=float))
    # Unvoiced frames in SwiftF0 output carry a nonsense f0 *and* low confidence.
    times = np.arange(hz.size, dtype=float) * hop
    return build_dataclass(
        F0Result,
        times=times,
        hz=hz,
        confidence=conf,
        hop_seconds=hop,
        tuning_hz=tuning_hz,
    )


def n_frames(seconds: float, hop: float = HOP) -> int:
    return int(round(seconds / hop))


def constant_midi(midi: float, seconds: float, hop: float = HOP) -> np.ndarray:
    return np.full(n_frames(seconds, hop), float(midi))


def notes_of(result):
    return result.notes


def midis(result):
    return [int(get_field(n, "midi")) for n in result.notes]


def starts(result):
    return [float(get_field(n, "start_seconds")) for n in result.notes]


def durations(result):
    return [float(get_field(n, "duration_seconds")) for n in result.notes]


# ---------------------------------------------------------------------------
# THE regression test
# ---------------------------------------------------------------------------


@pytest.mark.parametrize("vibrato_hz,peak_to_peak_cents", [
    (6.0, 100.0),      # the DESIGN.md case
    (5.0, 140.0),      # a wider operatic wobble
    (7.0, 60.0),       # a fast, shallow pop vibrato
])
def test_sustained_vibrato_produces_exactly_one_note(vibrato_hz, peak_to_peak_cents):
    """A steady 3 s A4 with vibrato is ONE note, not a dozen.

    This is the vibrato-fragmentation regression test.  The median filter, the
    1.2-semitone threshold and the 6-frame hysteresis are all required; deleting
    any one of them breaks this.
    """
    duration = 3.0
    t = np.arange(n_frames(duration), dtype=float) * HOP
    cents = (peak_to_peak_cents / 2.0) * np.sin(2.0 * math.pi * vibrato_hz * t)
    hz = midi_to_hz(69.0 + cents / 100.0)

    result = segment_notes(make_f0(hz))

    assert len(result.notes) == 1, (
        "vibrato fragmented into %d notes: %s"
        % (len(result.notes), list(zip(midis(result), starts(result))))
    )
    assert midis(result)[0] == 69
    assert durations(result)[0] == pytest.approx(duration, abs=0.05)
    assert abs(float(get_field(result.notes[0], "cents_offset"))) < 15.0


def test_vibrato_stays_one_note_with_spectral_flux_supplied():
    """The optional third onset term must not reintroduce fragmentation."""
    duration = 3.0
    t = np.arange(n_frames(duration), dtype=float) * HOP
    hz = midi_to_hz(69.0 + 0.5 * np.sin(2.0 * math.pi * 6.0 * t) / 1.0 * 0.5)
    f0 = make_f0(hz)
    # A plausible sustained-tone flux envelope: one attack transient, then noise.
    rng = np.random.default_rng(0)
    flux = 0.02 * rng.random(t.size)
    flux[0:3] = 1.0
    result = segment_notes(f0, spectral_flux=flux, spectral_flux_times=t)
    assert len(result.notes) == 1


def test_vibrato_fragments_without_the_median_filter():
    """Negative control: prove the three mechanisms are load-bearing.

    With the median filter off, the deviation threshold at SwiftF0's stock 0.8
    semitones and hysteresis disabled, the same signal shatters.  If this test
    starts passing with 1 note, the defaults are no longer doing the work the
    comments claim they are.

    +/-80 cents is a wide operatic vibrato and is the widest the current defaults
    survive; the amplitude sweep lives in the module docstring of the probe used
    to tune `cents_rate_reference`.
    """
    duration = 3.0
    t = np.arange(n_frames(duration), dtype=float) * HOP
    hz = midi_to_hz(69.0 + 0.8 * np.sin(2.0 * math.pi * 6.0 * t))   # +/-80 cents

    naive = SegmentConfig(
        median_filter_frames=1,
        split_deviation_semitones=0.8,
        hysteresis_frames=1,
        min_note_duration_seconds=0.05,
        merge_gap_seconds=0.0,
    )
    fragmented = segment_notes(make_f0(hz), naive)
    good = segment_notes(make_f0(hz))

    assert len(fragmented.notes) > 3
    assert len(good.notes) == 1


# ---------------------------------------------------------------------------
# Real transitions must survive
# ---------------------------------------------------------------------------


def test_clean_arpeggio_gives_three_notes():
    """C4-E4-G4, 300 ms each, contiguous and voiced throughout."""
    hz = midi_to_hz(np.concatenate([
        constant_midi(60, 0.30),
        constant_midi(64, 0.30),
        constant_midi(67, 0.30),
    ]))

    result = segment_notes(make_f0(hz))

    assert midis(result) == [60, 64, 67], (
        "expected [60, 64, 67], got %s at %s" % (midis(result), starts(result)))
    for start, expected in zip(starts(result), [0.0, 0.30, 0.60]):
        assert start == pytest.approx(expected, abs=0.05)
    for offset in (float(get_field(n, "cents_offset")) for n in result.notes):
        assert abs(offset) < 5.0


def test_arpeggio_with_breath_gaps_still_gives_three_notes():
    """Same arpeggio, but with 120 ms unvoiced gaps between the notes."""
    silent = np.full(n_frames(0.120), 0.0)
    hz = np.concatenate([
        midi_to_hz(constant_midi(60, 0.30)), silent,
        midi_to_hz(constant_midi(64, 0.30)), silent,
        midi_to_hz(constant_midi(67, 0.30)),
    ])
    conf = np.concatenate([
        np.full(n_frames(0.30), 0.95), np.full(n_frames(0.120), 0.05),
        np.full(n_frames(0.30), 0.95), np.full(n_frames(0.120), 0.05),
        np.full(n_frames(0.30), 0.95),
    ])
    result = segment_notes(make_f0(hz, confidence=conf))
    assert midis(result) == [60, 64, 67]


# ---------------------------------------------------------------------------
# Consonant bridging - the 80 ms boundary
# ---------------------------------------------------------------------------


def _note_with_gap(gap_seconds: float):
    """400 ms of A4, an unvoiced gap, then 400 ms more of A4."""
    voiced_frames = n_frames(0.40)
    gap_frames = n_frames(gap_seconds)
    hz = np.concatenate([
        midi_to_hz(np.full(voiced_frames, 69.0)),
        np.zeros(gap_frames),
        midi_to_hz(np.full(voiced_frames, 69.0)),
    ])
    conf = np.concatenate([
        np.full(voiced_frames, 0.95),
        np.full(gap_frames, 0.05),
        np.full(voiced_frames, 0.95),
    ])
    return make_f0(hz, confidence=conf)


def test_long_unvoiced_gap_splits_the_note():
    result = segment_notes(_note_with_gap(0.200))
    assert len(result.notes) == 2
    assert midis(result) == [69, 69]
    assert starts(result)[1] == pytest.approx(0.60, abs=0.05)


def test_short_unvoiced_gap_is_bridged():
    """A 50 ms consonant must not split the syllable in two."""
    result = segment_notes(_note_with_gap(0.048))
    assert len(result.notes) == 1
    assert midis(result) == [69]
    assert durations(result)[0] == pytest.approx(0.848, abs=0.05)


def test_gap_bridging_threshold_is_configurable():
    tight = SegmentConfig(max_bridged_gap_seconds=0.0, merge_gap_seconds=0.0)
    result = segment_notes(_note_with_gap(0.048), tight)
    assert len(result.notes) == 2


# ---------------------------------------------------------------------------
# Portamento
# ---------------------------------------------------------------------------


@pytest.mark.parametrize("glide_seconds", [0.3, 0.5, 0.8, 1.0, 1.5])
def test_slow_portamento_does_not_fragment(glide_seconds):
    """A 5-semitone glide is at most two notes, never a staircase.

    The running-median rule on its own trips repeatedly on the way up; the
    `require_settled_split` guard defers the split until the pitch arrives.
    """
    head = constant_midi(60, 0.25)
    glide_frames = n_frames(glide_seconds)
    glide = 60.0 + 5.0 * np.linspace(0.0, 1.0, glide_frames)
    tail = constant_midi(65, 0.25)
    hz = midi_to_hz(np.concatenate([head, glide, tail]))

    result = segment_notes(make_f0(hz))

    assert 1 <= len(result.notes) <= 2, (
        "portamento fragmented into %d notes: %s" % (len(result.notes), midis(result)))
    # The target of the slide must be charted correctly, and the boundary must
    # land at the end of the glide rather than in the middle of it.  The boundary
    # is placed where the pitch comes within `settle_max_drift_cents` of its
    # arrival value, so it leads the geometric end of the glide slightly.
    assert midis(result)[-1] == 65
    if len(result.notes) == 2:
        assert starts(result)[1] == pytest.approx(0.25 + glide_seconds, abs=0.15)


def test_portamento_staircases_without_the_settle_guard():
    """Negative control for `require_settled_split`."""
    head = constant_midi(60, 0.25)
    glide = 60.0 + 5.0 * np.linspace(0.0, 1.0, n_frames(1.0))
    tail = constant_midi(65, 0.25)
    hz = midi_to_hz(np.concatenate([head, glide, tail]))

    literal = segment_notes(make_f0(hz), SegmentConfig(require_settled_split=False))
    assert len(literal.notes) > 2
    assert len(segment_notes(make_f0(hz)).notes) <= 2


# ---------------------------------------------------------------------------
# Pitch assignment, snapping and centsOffset
# ---------------------------------------------------------------------------


def test_flat_note_snaps_and_records_cents_offset():
    """A4 sung 30 cents flat: MIDI 69, centsOffset ~= -30."""
    hz = midi_to_hz(np.full(n_frames(0.8), 69.0 - 0.30))
    result = segment_notes(make_f0(hz))

    assert len(result.notes) == 1
    assert int(get_field(result.notes[0], "midi")) == 69
    assert float(get_field(result.notes[0], "cents_offset")) == pytest.approx(-30.0, abs=3.0)


def test_sharp_note_records_positive_cents_offset():
    hz = midi_to_hz(np.full(n_frames(0.8), 62.0 + 0.42))
    result = segment_notes(make_f0(hz))
    assert int(get_field(result.notes[0], "midi")) == 62
    assert float(get_field(result.notes[0], "cents_offset")) == pytest.approx(42.0, abs=3.0)


def test_leading_scoop_is_excluded_from_the_pitch_median():
    """Step 8: the median of the middle 60% discards the scoop and the fall-off."""
    scoop = np.linspace(66.0, 69.0, n_frames(0.15))       # 3 semitones of approach
    body = np.full(n_frames(0.60), 69.0)
    fall = np.linspace(69.0, 67.5, n_frames(0.15))
    hz = midi_to_hz(np.concatenate([scoop, body, fall]))

    result = segment_notes(make_f0(hz))
    assert int(get_field(result.notes[0], "midi")) == 69
    assert abs(float(get_field(result.notes[0], "cents_offset"))) < 20.0


def test_tuning_hz_is_respected():
    """A record cut at A=432 must not bake in a global -32 cent offset."""
    tuning = 432.0
    hz = 432.0 * np.power(2.0, (69.0 - 69.0) / 12.0) * np.ones(n_frames(0.8))
    result = segment_notes(make_f0(hz, tuning_hz=tuning))
    assert int(get_field(result.notes[0], "midi")) == 69
    assert abs(float(get_field(result.notes[0], "cents_offset"))) < 2.0

    # The same audio interpreted at A=440 is ~32 cents flat.
    result_440 = segment_notes(make_f0(hz, tuning_hz=440.0))
    assert float(get_field(result_440.notes[0], "cents_offset")) == pytest.approx(-31.8, abs=3.0)


# ---------------------------------------------------------------------------
# Minimum duration, merging, confidence gate
# ---------------------------------------------------------------------------


def test_notes_shorter_than_the_minimum_are_dropped():
    """SwiftF0's 50 ms default is far too short for singing; ours is 120 ms."""
    hz = midi_to_hz(np.concatenate([
        constant_midi(60, 0.40),
        constant_midi(72, 0.064),      # a 64 ms blip - an octave artefact
        constant_midi(60, 0.40),
    ]))
    result = segment_notes(make_f0(hz))
    assert 72 not in midis(result)
    assert all(d >= 0.120 - 1e-9 for d in durations(result))


def test_low_confidence_frames_are_unvoiced():
    frames = n_frames(0.60)
    hz = midi_to_hz(np.full(frames, 69.0))
    conf = np.full(frames, 0.95)
    conf[n_frames(0.20):n_frames(0.40)] = 0.20      # 200 ms of breath
    result = segment_notes(make_f0(hz, confidence=conf))
    assert len(result.notes) == 2


def test_adjacent_same_pitch_fragments_are_merged():
    """Step 10: two same-semitone fragments of ONE note become one note.

    This is what the merge rule is actually for: a brief TRACKING wobble that
    split a single sustained note in two. Confidence stays high throughout, so
    no onset is reported between them, so they merge.
    """
    frames_a = n_frames(0.30)
    gap = n_frames(0.032)
    hz = np.concatenate([
        midi_to_hz(np.full(frames_a, 64.0)),
        midi_to_hz(np.full(gap, 64.0)),
        midi_to_hz(np.full(frames_a, 64.0)),
    ])
    # Confidence dips but does NOT collapse - the singer never stopped phonating.
    conf = np.concatenate([
        np.full(frames_a, 0.95), np.full(gap, 0.60), np.full(frames_a, 0.95)])
    cfg = SegmentConfig(max_bridged_gap_seconds=0.0)
    result = segment_notes(make_f0(hz, confidence=conf), cfg)
    assert len(result.notes) == 1
    assert midis(result) == [64]


def test_same_pitch_across_a_consonant_stays_two_notes():
    """REGRESSION GUARD for the syllable-fusing bug found on real singing.

    Two syllables sung at the SAME pitch, separated by a consonant (confidence
    collapses), must remain TWO notes. Merging them is how a near-monotone verse
    turns into a handful of multi-second bars with the rhythm erased.

    Measured on the Mr Brightside lead vocal before the fix: 74 of 158 notes
    landed on MIDI 61, covering 93 s of the 148 s sung, with single "notes" up to
    4.5 s long. The pitch was right; the rhythm was gone. See DESIGN.md 11.5.
    """
    frames_a = n_frames(0.30)
    gap = n_frames(0.032)
    hz = np.concatenate([
        midi_to_hz(np.full(frames_a, 64.0)),
        np.zeros(gap),
        midi_to_hz(np.full(frames_a, 64.0)),
    ])
    conf = np.concatenate([
        np.full(frames_a, 0.95), np.full(gap, 0.02), np.full(frames_a, 0.95)])
    cfg = SegmentConfig(max_bridged_gap_seconds=0.0)
    result = segment_notes(make_f0(hz, confidence=conf), cfg)
    assert len(result.notes) == 2, (
        "same-pitch syllables separated by a consonant must not fuse")
    assert midis(result) == [64, 64]


def test_empty_input_is_handled():
    result = segment_notes(make_f0(np.zeros(0)))
    assert result.notes == []
    assert result.contour is not None


def test_all_unvoiced_input_produces_no_notes():
    frames = n_frames(1.0)
    result = segment_notes(make_f0(np.zeros(frames), confidence=np.full(frames, 0.01)))
    assert result.notes == []


# ---------------------------------------------------------------------------
# Contour
# ---------------------------------------------------------------------------


def test_contour_is_100hz_with_none_where_unvoiced():
    frames = n_frames(1.0)
    hz = midi_to_hz(np.full(frames, 69.0))
    conf = np.full(frames, 0.95)
    conf[n_frames(0.40):n_frames(0.60)] = 0.05
    hz[n_frames(0.40):n_frames(0.60)] = 0.0

    contour = build_contour(make_f0(hz, confidence=conf), rate_hz=100.0)

    frac = get_field(contour, "frac_midi")
    confidence = get_field(contour, "confidence")
    assert float(get_field(contour, "rate_hz")) == 100.0
    assert len(frac) == len(confidence)
    assert len(frac) == pytest.approx(100, abs=2)          # ~1 s at 100 Hz
    assert frac[10] == pytest.approx(69.0, abs=0.01)
    assert frac[50] is None                                 # inside the unvoiced hole
    assert frac[90] == pytest.approx(69.0, abs=0.01)
    assert all(v is None or isinstance(v, float) for v in frac)


def test_segmentation_result_carries_the_contour():
    hz = midi_to_hz(np.full(n_frames(0.5), 69.0))
    result = segment_notes(make_f0(hz))
    assert result.contour is not None
    assert len(get_field(result.contour, "frac_midi")) > 0


# ---------------------------------------------------------------------------
# Config surface
# ---------------------------------------------------------------------------


def test_every_documented_threshold_is_exposed():
    cfg = SegmentConfig()
    assert cfg.confidence_threshold == 0.50
    assert cfg.median_filter_frames == 7
    assert cfg.split_deviation_semitones == 1.2
    assert cfg.hysteresis_frames == 6
    assert cfg.min_note_duration_seconds == 0.120
    assert cfg.max_bridged_gap_seconds == 0.080
    assert cfg.core_fraction == 0.60
    assert cfg.merge_gap_seconds == 0.080
    assert cfg.contour_rate_hz == 100.0


def test_even_median_kernel_is_forced_odd():
    assert SegmentConfig(median_filter_frames=6).median_filter_frames == 7
