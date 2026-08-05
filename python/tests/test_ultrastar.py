"""Tests for the UltraStar .txt sidecar (DESIGN.md section 3).

The two things worth breaking a build over:

  * the ``x4`` in ``t = GAP/1000 + beat * 60 / (BPM_header * 4)``.  USDX
    quadruples ``#BPM`` at parse time; dropping the factor misplaces every note
    in the file by 4x and the error is invisible until you play the chart.
  * import must **merge**.  ``contour``, ``confidence``, ``centsOffset`` and
    ``warnings`` have no representation in the text format, so an import that
    rebuilds the Chart from scratch destroys the ghost line.
"""

from __future__ import annotations

import dataclasses
from types import SimpleNamespace

import pytest

from vocalchart.chart import Chart, Contour, Note, Phrase
from vocalchart.segment import build_note, build_phrase, get_field
from vocalchart.ultrastar import (
    DEFAULT_BPM_HEADER,
    beat_duration_seconds,
    beat_to_seconds,
    chart_to_ultrastar_text,
    merge_ultrastar_into_chart,
    midi_to_ultrastar_pitch,
    parse_ultrastar_text,
    seconds_to_beat,
    ultrastar_pitch_to_midi,
    write_ultrastar,
)

BEAT = beat_duration_seconds(DEFAULT_BPM_HEADER)      # 18.75 ms


# ---------------------------------------------------------------------------
# Fixtures - built defensively so a field-naming choice in chart.py cannot
# break these tests.
# ---------------------------------------------------------------------------


def make_contour(rate_hz=100.0, start_seconds=0.0, frac_midi=None, confidence=None):
    frac_midi = frac_midi if frac_midi is not None else [None, None, 55.02, 55.06, 54.98]
    confidence = confidence if confidence is not None else [0.03, 0.05, 0.88, 0.92, 0.94]
    kwargs = {"rate_hz": rate_hz, "start_seconds": start_seconds,
              "frac_midi": frac_midi, "confidence": confidence}
    from vocalchart.segment import build_contour_object
    return build_contour_object(**kwargs)


def make_chart(notes=(), phrases=(), contour=None, warnings=None, song=None):
    """Construct a Chart, filling any field this test does not care about."""
    supplied = {
        "notes": list(notes),
        "phrases": list(phrases),
        "contour": contour,
        "warnings": list(warnings) if warnings is not None else [],
        "song": song if song is not None else SimpleNamespace(
            id="test-song", title="Amazing Grace", artist="Traditional",
            duration_seconds=10.0, reference_audio="stems/vocals.wav",
            backing_audio="backing.wav", tuning_hz=440.0),
    }
    aliases = {
        "notes": ("notes",), "phrases": ("phrases",), "contour": ("contour",),
        "warnings": ("warnings",), "song": ("song",),
    }
    if not dataclasses.is_dataclass(Chart):
        return Chart(**supplied)

    kwargs = {}
    for f in dataclasses.fields(Chart):
        if not f.init:
            continue
        matched = None
        for canonical, names in aliases.items():
            if f.name in names:
                matched = canonical
                break
        if matched is not None:
            kwargs[f.name] = supplied[matched]
            continue
        has_default = (f.default is not dataclasses.MISSING
                       or f.default_factory is not dataclasses.MISSING)  # type: ignore[misc]
        if not has_default:
            kwargs[f.name] = None
    return Chart(**kwargs)


def note(id, start, duration, midi, syllable, role="wordStart", type="normal",
         cents_offset=0.0, confidence=0.9, phrase_id=0):
    return build_note(id=id, start_seconds=start, duration_seconds=duration,
                      midi=midi, cents_offset=cents_offset, type=type,
                      syllable=syllable, syllable_role=role, phrase_id=phrase_id,
                      confidence=confidence)


def sample_notes():
    # Deliberately placed on exact 18.75 ms boundaries relative to GAP=2140 ms so
    # that quantisation error is isolated to the rounding test below.
    return [
        note(0, 2.140, 0.88125, 55, "A", "wordStart", cents_offset=-12.4, confidence=0.94),
        note(1, 3.02125, 1.76250, 60, "ma", "continuation", cents_offset=4.1, confidence=0.97),
        note(2, 4.78375, 0.43125, 64, "zing", "continuation", cents_offset=-3.8, confidence=0.96),
        note(3, 5.21500, 0.43125, 60, None, "melisma", cents_offset=8.9, confidence=0.91),
        note(4, 5.64625, 1.32000, 60, "grace", "wordStart", cents_offset=1.2, confidence=0.98,
             phrase_id=1),
        note(5, 6.96625, 0.88125, 64, "how", "wordStart", type="golden",
             cents_offset=-21.7, confidence=0.95, phrase_id=1),
    ]


def sample_chart():
    notes = sample_notes()
    phrases = [
        build_phrase(id=0, start_seconds=2.140, end_seconds=5.64625,
                     text="Amazing", note_ids=[0, 1, 2, 3]),
        build_phrase(id=1, start_seconds=5.64625, end_seconds=7.8475,
                     text="grace how", note_ids=[4, 5]),
    ]
    return make_chart(notes=notes, phrases=phrases, contour=make_contour(),
                      warnings=[{"code": "LOW_CONFIDENCE_REGION",
                                 "startSeconds": 4.0, "endSeconds": 4.5,
                                 "message": "breathy"}])


# ---------------------------------------------------------------------------
# Conversion arithmetic
# ---------------------------------------------------------------------------


def test_beat_length_includes_the_implicit_times_four():
    # One beat = 15 / BPM_header seconds.  At #BPM:800 that is 18.75 ms.
    assert beat_duration_seconds(800.0) == pytest.approx(0.01875)
    assert beat_duration_seconds(400.0) == pytest.approx(0.0375)
    assert beat_duration_seconds(60.0) == pytest.approx(0.25)   # NOT 1.0 second


def test_beat_to_seconds_matches_the_design_formula():
    for beat in (0, 1, 17, 400, -3):
        expected = 2.140 + beat * 60.0 / (800.0 * 4.0)
        assert beat_to_seconds(beat, 800.0, 2140.0) == pytest.approx(expected)


def test_seconds_to_beat_round_trips():
    for seconds in (2.140, 3.0, 9.75):
        beat = seconds_to_beat(seconds, 800.0, 2140.0)
        assert beat_to_seconds(beat, 800.0, 2140.0) == pytest.approx(seconds)


def test_pitch_is_relative_to_c4():
    assert midi_to_ultrastar_pitch(60) == 0
    assert midi_to_ultrastar_pitch(65) == 5          # F4 is UltraStar pitch 5
    assert midi_to_ultrastar_pitch(55) == -5
    for midi in (36, 55, 60, 72, 84):
        assert ultrastar_pitch_to_midi(midi_to_ultrastar_pitch(midi)) == midi


# ---------------------------------------------------------------------------
# Export
# ---------------------------------------------------------------------------


def test_export_headers_and_terminator():
    text = chart_to_ultrastar_text(sample_chart())
    lines = text.splitlines()
    assert "#BPM:800" in lines
    assert "#GAP:2140" in lines                       # first note start, in ms
    assert "#TITLE:Amazing Grace" in lines
    assert "#ARTIST:Traditional" in lines
    assert lines[-1] == "E"


def test_export_note_lines():
    text = chart_to_ultrastar_text(sample_chart())
    body = [l for l in text.splitlines() if l and not l.startswith("#")]
    first = body[0].split(" ")
    assert first[0] == ":"                            # normal note
    assert first[1] == "0"                            # first note sits on beat 0
    assert first[2] == "47"                           # 0.88125 s / 18.75 ms
    assert first[3] == "-5"                           # MIDI 55 -> C4-relative -5
    assert body[0].endswith("A")

    assert any(l.startswith("* ") for l in body), "golden note must use '*'"
    assert any(l.startswith("- ") for l in body), "phrase break line missing"
    assert any(l.endswith("~") for l in body), "melisma note must emit a tilde"


def test_export_marks_word_starts_with_a_leading_space():
    """UltraStar joins syllables; a new word is signalled by a leading space.

    Checked by re-parsing, because the field separator is itself a space.
    """
    song = parse_ultrastar_text(chart_to_ultrastar_text(sample_chart()))
    texts = [n.text for n in song.notes]
    # "A" + "ma" + "zing" concatenate into one word; " grace" starts a new one.
    assert texts[0] == "A"
    assert texts[1] == "ma"
    assert texts[2] == "zing"
    assert texts[3] == "~"
    assert texts[4] == "grace"        # first note of its phrase: no space needed
    assert texts[5] == " how"


def test_export_is_utf8_without_bom(tmp_path):
    path = tmp_path / "chart.ultrastar.txt"
    write_ultrastar(sample_chart(), str(path))
    raw = path.read_bytes()
    assert not raw.startswith(b"\xef\xbb\xbf")
    assert raw.decode("utf-8").endswith("E\r\n")


def test_export_does_not_overlap_notes():
    text = chart_to_ultrastar_text(sample_chart())
    body = [l.split(" ") for l in text.splitlines()
            if l[:1] in (":", "*", "F", "R", "G")]
    for prev, nxt in zip(body, body[1:]):
        assert int(prev[1]) + int(prev[2]) <= int(nxt[1])


# ---------------------------------------------------------------------------
# Import
# ---------------------------------------------------------------------------


HAND_WRITTEN = "\r\n".join([
    "﻿#TITLE:Hand Edited",
    "#ARTIST:Someone",
    "#MP3:backing.wav",
    "#BPM:800",
    "#GAP:2140",
    ": 0 47 -5 A",
    ": 47 94 0 ma",
    ": 141 23 4 zing",
    ": 164 23 0 ~",
    "- 180",
    "* 188 70 0  grace",
    "F 258 47 4  how",
    "E",
    "",
])


def test_parse_headers_notes_and_breaks():
    song = parse_ultrastar_text(HAND_WRITTEN)
    assert song.headers["TITLE"] == "Hand Edited"
    assert song.bpm_header == 800.0
    assert song.gap_ms == 2140.0
    assert len(song.notes) == 6
    assert song.phrase_break_beats == [180]
    assert song.notes[0].beat == 0 and song.notes[0].length == 47
    assert song.notes[0].text == "A"
    assert song.notes[4].text == " grace"            # leading space preserved
    assert [n.line_index for n in song.notes] == [0, 0, 0, 0, 1, 1]


def test_parse_note_types():
    song = parse_ultrastar_text(HAND_WRITTEN)
    types = [n.note_type for n in song.notes]
    assert types[:4] == ["normal"] * 4
    assert types[4] == "golden"
    assert types[5] == "freestyle"


def test_parse_note_timing_uses_the_times_four_rule():
    song = parse_ultrastar_text(HAND_WRITTEN)
    start, duration = song.note_seconds(song.notes[0])
    assert start == pytest.approx(2.140)
    assert duration == pytest.approx(47 * 0.01875)
    start1, _ = song.note_seconds(song.notes[1])
    assert start1 == pytest.approx(2.140 + 47 * 0.01875)


def test_parse_rejects_relative_files():
    with pytest.raises(ValueError):
        parse_ultrastar_text("#BPM:800\r\n#RELATIVE:yes\r\n: 0 4 0 a\r\nE\r\n")


def test_bom_and_lf_only_files_parse():
    song = parse_ultrastar_text(HAND_WRITTEN.replace("\r\n", "\n"))
    assert len(song.notes) == 6


# ---------------------------------------------------------------------------
# Merge semantics - the part that must not lose data
# ---------------------------------------------------------------------------


def test_merge_preserves_contour_warnings_and_song():
    chart = sample_chart()
    contour_before = get_field(chart, "contour")
    warnings_before = get_field(chart, "warnings")

    merged = merge_ultrastar_into_chart(chart, HAND_WRITTEN)

    assert get_field(merged, "contour") is contour_before
    assert get_field(merged, "warnings") == warnings_before
    assert get_field(merged, "song") is get_field(chart, "song")
    # and the original chart is untouched
    assert len(get_field(chart, "notes")) == 6


def test_merge_carries_confidence_across_from_matched_notes():
    chart = sample_chart()
    merged = merge_ultrastar_into_chart(chart, HAND_WRITTEN)
    notes = get_field(merged, "notes")
    assert float(get_field(notes[0], "confidence")) == pytest.approx(0.94)
    assert float(get_field(notes[1], "confidence")) == pytest.approx(0.97)


def test_merge_keeps_cents_offset_only_when_the_pitch_is_unchanged():
    chart = sample_chart()
    edited = HAND_WRITTEN.replace(": 141 23 4 zing", ": 141 23 7 zing")  # E4 -> G4
    merged = merge_ultrastar_into_chart(chart, edited)
    notes = get_field(merged, "notes")
    assert int(get_field(notes[0], "midi")) == 55
    assert float(get_field(notes[0], "cents_offset")) == pytest.approx(-12.4)
    assert int(get_field(notes[2], "midi")) == 67                # the human's edit wins
    assert float(get_field(notes[2], "cents_offset")) == pytest.approx(0.0, abs=1e-9)


def test_merge_rebuilds_phrases_and_text():
    merged = merge_ultrastar_into_chart(sample_chart(), HAND_WRITTEN)
    phrases = get_field(merged, "phrases")
    assert len(phrases) == 2
    assert get_field(phrases[0], "text") == "Amazing"
    assert get_field(phrases[1], "text") == "grace how"
    assert get_field(phrases[0], "note_ids") == [0, 1, 2, 3]
    notes = get_field(merged, "notes")
    assert get_field(notes[3], "syllable") is None
    assert get_field(notes[3], "syllable_role") == "melisma"
    assert get_field(notes[4], "syllable_role") == "wordStart"


def test_merge_accepts_added_notes_without_a_match():
    chart = sample_chart()
    added = HAND_WRITTEN.replace("E\r\n", ": 305 40 2 new\r\nE\r\n")
    merged = merge_ultrastar_into_chart(chart, added)
    notes = get_field(merged, "notes")
    assert len(notes) == 7
    assert int(get_field(notes[6], "midi")) == 62
    assert float(get_field(notes[6], "confidence")) >= 0.0


# ---------------------------------------------------------------------------
# Round trip
# ---------------------------------------------------------------------------


def test_round_trip_preserves_count_pitch_timing_and_contour():
    chart = sample_chart()
    text = chart_to_ultrastar_text(chart)
    merged = merge_ultrastar_into_chart(chart, text)

    before = get_field(chart, "notes")
    after = get_field(merged, "notes")

    assert len(after) == len(before)
    assert [int(get_field(n, "midi")) for n in after] == \
           [int(get_field(n, "midi")) for n in before]

    for old, new in zip(before, after):
        assert float(get_field(new, "start_seconds")) == pytest.approx(
            float(get_field(old, "start_seconds")), abs=BEAT)
        assert float(get_field(new, "duration_seconds")) == pytest.approx(
            float(get_field(old, "duration_seconds")), abs=BEAT)

    assert get_field(merged, "contour") is get_field(chart, "contour")
    assert get_field(merged, "warnings") == get_field(chart, "warnings")
    assert [float(get_field(n, "confidence")) for n in after] == \
           [float(get_field(n, "confidence")) for n in before]


def test_round_trip_through_a_file(tmp_path):
    chart = sample_chart()
    path = tmp_path / "chart.ultrastar.txt"
    write_ultrastar(chart, str(path))
    from vocalchart.ultrastar import merge_ultrastar_file_into_chart
    merged = merge_ultrastar_file_into_chart(chart, str(path))
    assert len(get_field(merged, "notes")) == 6
    assert get_field(merged, "contour") is get_field(chart, "contour")


def test_round_trip_is_stable_a_second_time():
    chart = sample_chart()
    once = merge_ultrastar_into_chart(chart, chart_to_ultrastar_text(chart))
    twice = merge_ultrastar_into_chart(once, chart_to_ultrastar_text(once))
    a = get_field(once, "notes")
    b = get_field(twice, "notes")
    assert len(a) == len(b)
    for x, y in zip(a, b):
        assert float(get_field(x, "start_seconds")) == pytest.approx(
            float(get_field(y, "start_seconds")), abs=1e-6)
        assert int(get_field(x, "midi")) == int(get_field(y, "midi"))
        assert get_field(x, "syllable") == get_field(y, "syllable")


def test_round_trip_at_a_non_default_bpm_header():
    chart = sample_chart()
    text = chart_to_ultrastar_text(chart, bpm_header=400.0)
    assert "#BPM:400" in text.splitlines()
    merged = merge_ultrastar_into_chart(chart, text)
    for old, new in zip(get_field(chart, "notes"), get_field(merged, "notes")):
        assert float(get_field(new, "start_seconds")) == pytest.approx(
            float(get_field(old, "start_seconds")), abs=beat_duration_seconds(400.0))
