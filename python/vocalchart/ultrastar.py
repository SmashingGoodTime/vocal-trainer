"""Bidirectional conversion between ``chart.json`` and the UltraStar ``.txt`` format.

``chart.json`` is normative.  ``chart.ultrastar.txt`` is a **lossy sidecar** whose
only purpose is to unlock the existing manual-correction tooling (Yass, UltraStar
Manager, UltraStar Play's song editor).  See DESIGN.md section 3.

The two facts that break every naive implementation:

1. **``#BPM`` is quadrupled at parse time.**  USDX ``USong.pas`` does
   ``self.BPM := StrToFloatI18n(Value) * Mult * 4``.  So

       t = GAP/1000 + beat * 60 / (BPM_header * 4)

   i.e. one beat is ``15 / BPM_header`` seconds.  Get the x4 wrong and every note
   in the file is misplaced by a factor of four.

2. **Import must merge, not replace.**  ``centsOffset``, ``contour``,
   ``confidence`` and ``warnings`` have no representation in the ``.txt``.  A
   round trip that rebuilds the Chart from the text alone silently destroys the
   ghost line, which is the single most valuable field in the format.

Export settings per DESIGN.md: ``#BPM:800`` (an 18.75 ms beat grid, well under the
30 ms onset-perception threshold), ``#GAP`` = first note start in ms, UTF-8 with
no BOM.
"""

from __future__ import annotations

import os
import re
from dataclasses import dataclass, field
from typing import Any, Dict, Iterable, List, Optional, Sequence, Tuple

from vocalchart.chart import Chart, Contour, Note, Phrase
from vocalchart.segment import build_note, build_phrase, get_field, set_fields

__all__ = [
    "DEFAULT_BPM_HEADER",
    "UltrastarNote",
    "UltrastarSong",
    "beat_duration_seconds",
    "beat_to_seconds",
    "seconds_to_beat",
    "midi_to_ultrastar_pitch",
    "ultrastar_pitch_to_midi",
    "NOTE_TYPE_TO_CHAR",
    "CHAR_TO_NOTE_TYPE",
    "chart_to_ultrastar_text",
    "write_ultrastar",
    "parse_ultrastar_text",
    "read_ultrastar",
    "merge_ultrastar_into_chart",
    "merge_ultrastar_file_into_chart",
]


DEFAULT_BPM_HEADER = 800.0          # 15/800 s = 18.75 ms per beat
MELISMA_TEXT = "~"


# ---------------------------------------------------------------------------
# Time and pitch conversion
# ---------------------------------------------------------------------------


def beat_duration_seconds(bpm_header: float) -> float:
    """Seconds per UltraStar beat.  ``15 / BPM_header`` - note the implicit x4."""
    return 60.0 / (float(bpm_header) * 4.0)


def beat_to_seconds(beat: float, bpm_header: float, gap_ms: float) -> float:
    """``t = GAP/1000 + beat * 60 / (BPM_header * 4)``."""
    return gap_ms / 1000.0 + float(beat) * beat_duration_seconds(bpm_header)


def seconds_to_beat(seconds: float, bpm_header: float, gap_ms: float) -> float:
    """Inverse of :func:`beat_to_seconds`, unrounded."""
    return (float(seconds) - gap_ms / 1000.0) / beat_duration_seconds(bpm_header)


def midi_to_ultrastar_pitch(midi: int) -> int:
    """UltraStar pitch is relative to C4 = MIDI 60 (so ``5`` is F4)."""
    return int(midi) - 60


def ultrastar_pitch_to_midi(pitch: int) -> int:
    return int(pitch) + 60


NOTE_TYPE_TO_CHAR: Dict[str, str] = {
    "normal": ":",
    "golden": "*",
    "freestyle": "F",
    "rap": "R",
    # DESIGN.md lists `breath` as a chart-only type; the closest UltraStar
    # behaviour (always-hit, pitch ignored) is freestyle.
    "breath": "F",
}

CHAR_TO_NOTE_TYPE: Dict[str, str] = {
    ":": "normal",
    "*": "golden",
    "F": "freestyle",
    "R": "rap",
    # 'G' is golden rap.  chart.json carries a single `type` string, so the
    # pitch-ignoring half is the one worth keeping; the golden multiplier is lost.
    "G": "rap",
}


# ---------------------------------------------------------------------------
# Parsed representation
# ---------------------------------------------------------------------------


@dataclass
class UltrastarNote:
    type_char: str
    beat: int
    length: int
    pitch: int
    text: str
    line_index: int = 0          # which phrase ('-'-separated line group)

    @property
    def note_type(self) -> str:
        return CHAR_TO_NOTE_TYPE.get(self.type_char.upper(), "normal")


@dataclass
class UltrastarSong:
    headers: Dict[str, str] = field(default_factory=dict)
    notes: List[UltrastarNote] = field(default_factory=list)
    phrase_break_beats: List[int] = field(default_factory=list)

    def header_float(self, key: str, default: float) -> float:
        raw = self.headers.get(key.upper())
        if raw is None:
            return default
        try:
            return float(str(raw).strip().replace(",", "."))
        except ValueError:
            return default

    @property
    def bpm_header(self) -> float:
        return self.header_float("BPM", DEFAULT_BPM_HEADER)

    @property
    def gap_ms(self) -> float:
        return self.header_float("GAP", 0.0)

    def note_seconds(self, note: UltrastarNote) -> Tuple[float, float]:
        start = beat_to_seconds(note.beat, self.bpm_header, self.gap_ms)
        duration = note.length * beat_duration_seconds(self.bpm_header)
        return start, duration


# ---------------------------------------------------------------------------
# Export
# ---------------------------------------------------------------------------


def _song_attr(chart: Any, *names: str, default: Any = None) -> Any:
    song = get_field(chart, "song")
    if song is None:
        return default
    for name in names:
        if isinstance(song, dict):
            if name in song:
                return song[name]
        elif hasattr(song, name):
            return getattr(song, name)
    return default


def _note_end(note: Any) -> float:
    return float(get_field(note, "start_seconds", 0.0)) + \
        float(get_field(note, "duration_seconds", 0.0))


def _ordered_notes(chart: Any) -> List[Any]:
    notes = list(get_field(chart, "notes") or [])
    return sorted(notes, key=lambda n: float(get_field(n, "start_seconds", 0.0)))


def _phrase_groups(chart: Any, notes: List[Any]) -> List[List[Any]]:
    """Group notes into phrases, falling back to a single phrase."""
    phrases = list(get_field(chart, "phrases") or [])
    if not phrases:
        return [notes] if notes else []

    by_id = {}
    for n in notes:
        nid = get_field(n, "id")
        if nid is not None:
            by_id[nid] = n

    groups: List[List[Any]] = []
    claimed = set()
    ordered_phrases = sorted(
        phrases, key=lambda p: float(get_field(p, "start_seconds", 0.0)))
    for phrase in ordered_phrases:
        ids = list(get_field(phrase, "note_ids") or [])
        group = [by_id[i] for i in ids if i in by_id]
        if not group:
            start = float(get_field(phrase, "start_seconds", 0.0))
            end = float(get_field(phrase, "end_seconds", 0.0))
            group = [n for n in notes
                     if start - 1e-6 <= float(get_field(n, "start_seconds", 0.0)) <= end + 1e-6]
        group = [n for n in group if id(n) not in claimed]
        if group:
            claimed.update(id(n) for n in group)
            groups.append(sorted(group,
                                 key=lambda n: float(get_field(n, "start_seconds", 0.0))))

    leftovers = [n for n in notes if id(n) not in claimed]
    if leftovers:
        groups.append(leftovers)
    return groups


def _export_text(note: Any, is_first_in_phrase: bool, melisma_text: str) -> str:
    syllable = get_field(note, "syllable")
    role = (get_field(note, "syllable_role") or "none")
    if syllable is None or str(syllable) == "":
        return melisma_text
    text = str(syllable)
    if role == "wordStart" and not is_first_in_phrase:
        text = " " + text
    return text


def chart_to_ultrastar_text(
    chart: Any,
    bpm_header: float = DEFAULT_BPM_HEADER,
    gap_ms: Optional[float] = None,
    extra_headers: Optional[Dict[str, str]] = None,
    line_ending: str = "\r\n",
    melisma_text: str = MELISMA_TEXT,
    prevent_overlap: bool = True,
) -> str:
    """Render a Chart as UltraStar ``.txt`` source (no BOM, caller writes UTF-8).

    ``gap_ms`` defaults to the first note's start in milliseconds, per DESIGN.md.
    """
    notes = _ordered_notes(chart)
    if gap_ms is None:
        gap_ms = round(float(get_field(notes[0], "start_seconds", 0.0)) * 1000.0, 3) \
            if notes else 0.0

    beat_seconds = beat_duration_seconds(bpm_header)

    headers: Dict[str, str] = {}
    title = _song_attr(chart, "title") or "Untitled"
    artist = _song_attr(chart, "artist") or "Unknown"
    audio = (_song_attr(chart, "backing_audio", "backingAudio")
             or _song_attr(chart, "reference_audio", "referenceAudio")
             or "")
    headers["TITLE"] = str(title)
    headers["ARTIST"] = str(artist)
    if audio:
        headers["MP3"] = os.path.basename(str(audio).replace("\\", "/"))
    headers["BPM"] = ("%d" % bpm_header) if float(bpm_header).is_integer() \
        else ("%g" % bpm_header)
    headers["GAP"] = ("%d" % round(gap_ms))
    if extra_headers:
        for key, value in extra_headers.items():
            headers[key.upper().lstrip("#")] = str(value)

    lines: List[str] = ["#%s:%s" % (k, v) for k, v in headers.items()]

    groups = _phrase_groups(chart, notes)
    flat: List[Tuple[Any, bool]] = []
    for group in groups:
        for i, note in enumerate(group):
            flat.append((note, i == 0))

    for group_index, group in enumerate(groups):
        if group_index > 0:
            prev_group = groups[group_index - 1]
            prev_end = _note_end(prev_group[-1])
            next_start = float(get_field(group[0], "start_seconds", 0.0))
            break_seconds = 0.5 * (prev_end + next_start)
            break_beat = int(round(seconds_to_beat(break_seconds, bpm_header, gap_ms)))
            lines.append("- %d" % break_beat)

        for i, note in enumerate(group):
            start = float(get_field(note, "start_seconds", 0.0))
            duration = float(get_field(note, "duration_seconds", 0.0))
            beat = int(round(seconds_to_beat(start, bpm_header, gap_ms)))
            length = int(round(duration / beat_seconds))
            length = max(1, length)
            if prevent_overlap:
                following = None
                if i + 1 < len(group):
                    following = group[i + 1]
                elif group_index + 1 < len(groups):
                    following = groups[group_index + 1][0]
                if following is not None:
                    next_beat = int(round(seconds_to_beat(
                        float(get_field(following, "start_seconds", 0.0)),
                        bpm_header, gap_ms)))
                    if next_beat > beat:
                        length = min(length, next_beat - beat)
            note_type = str(get_field(note, "type") or "normal")
            char = NOTE_TYPE_TO_CHAR.get(note_type, ":")
            midi = int(get_field(note, "midi", 60) or 0)
            text = _export_text(note, i == 0, melisma_text)
            lines.append("%s %d %d %d %s"
                         % (char, beat, length, midi_to_ultrastar_pitch(midi), text))

    lines.append("E")
    return line_ending.join(lines) + line_ending


def write_ultrastar(chart: Any, path: str, **kwargs: Any) -> str:
    """Write ``chart.ultrastar.txt``.  UTF-8, **no BOM**, line endings preserved.

    Atomic (temp + os.replace), like chart.json: this file is hand-edited in
    external tools, and half a file is worse than the old file."""
    import os

    text = chart_to_ultrastar_text(chart, **kwargs)
    tmp = str(path) + ".tmp"
    with open(tmp, "w", encoding="utf-8", newline="") as handle:
        handle.write(text)
    os.replace(tmp, path)
    return text


# ---------------------------------------------------------------------------
# Import
# ---------------------------------------------------------------------------


_NOTE_RE = re.compile(
    r"^(?P<type>[:*FRG])[ \t]+(?P<beat>-?\d+)[ \t]+(?P<length>-?\d+)[ \t]+"
    r"(?P<pitch>-?\d+)[ \t]?(?P<text>.*)$"
)
_BREAK_RE = re.compile(r"^-[ \t]+(?P<beat>-?\d+)(?:[ \t]+(?P<beat2>-?\d+))?[ \t]*$")
_HEADER_RE = re.compile(r"^#(?P<key>[A-Za-z0-9_]+):(?P<value>.*)$")


def parse_ultrastar_text(text: str) -> UltrastarSong:
    """Parse UltraStar ``.txt`` source into an :class:`UltrastarSong`.

    Tolerates a UTF-8 BOM, CRLF or LF, and both the one- and two-argument forms
    of the ``-`` phrase-break line.  ``#RELATIVE:yes`` is rejected loudly rather
    than silently mistimed.
    """
    if text.startswith("﻿"):
        text = text[1:]

    song = UltrastarSong()
    line_index = 0
    in_body = False

    for raw in text.splitlines():
        line = raw.rstrip("\r\n")
        stripped = line.strip()
        if not stripped:
            continue
        if stripped == "E" and in_body:
            break

        header = _HEADER_RE.match(stripped)
        if header and not in_body:
            song.headers[header.group("key").upper()] = header.group("value").strip()
            continue

        note_match = _NOTE_RE.match(line.lstrip("\t "))
        if note_match:
            in_body = True
            song.notes.append(UltrastarNote(
                type_char=note_match.group("type").upper(),
                beat=int(note_match.group("beat")),
                length=int(note_match.group("length")),
                pitch=int(note_match.group("pitch")),
                text=note_match.group("text"),
                line_index=line_index,
            ))
            continue

        break_match = _BREAK_RE.match(stripped)
        if break_match:
            in_body = True
            song.phrase_break_beats.append(int(break_match.group("beat")))
            line_index += 1
            continue
        # Unknown line: ignore rather than fail; hand-edited files collect junk.

    relative = song.headers.get("RELATIVE", "").strip().lower()
    if relative in ("yes", "true", "1"):
        raise ValueError(
            "#RELATIVE:yes files use per-line relative beats; convert to absolute "
            "before importing (UltraStar Manager can do this)."
        )
    return song


def read_ultrastar(path: str) -> UltrastarSong:
    with open(path, "r", encoding="utf-8-sig", newline="") as handle:
        return parse_ultrastar_text(handle.read())


def _text_to_syllable(text: str, is_first_in_phrase: bool,
                      melisma_text: str) -> Tuple[Optional[str], str]:
    """Return ``(syllable, syllableRole)`` from a raw UltraStar text field."""
    leading_space = text.startswith(" ") or text.startswith("\t")
    body = text.strip()
    if body in ("", melisma_text, "~"):
        return None, "melisma"
    if leading_space or is_first_in_phrase:
        return body, "wordStart"
    return body, "continuation"


def _contour_stats(contour: Any, start: float, end: float) -> Tuple[Optional[float],
                                                                    Optional[float]]:
    """Mean confidence and median fractional MIDI of the contour over [start, end]."""
    if contour is None:
        return None, None
    rate = float(get_field(contour, "rate_hz", 100.0) or 100.0)
    origin = float(get_field(contour, "start_seconds", 0.0) or 0.0)
    frac = get_field(contour, "frac_midi") or []
    conf = get_field(contour, "confidence") or []
    if not frac:
        return None, None
    lo = max(0, int((start - origin) * rate))
    hi = min(len(frac), int((end - origin) * rate) + 1)
    if hi <= lo:
        return None, None
    values = [v for v in frac[lo:hi] if v is not None]
    confs = [c for c in conf[lo:hi] if c is not None] if conf else []
    mean_conf = (sum(confs) / len(confs)) if confs else None
    median_frac = None
    if values:
        values = sorted(values)
        mid = len(values) // 2
        median_frac = values[mid] if len(values) % 2 else 0.5 * (values[mid - 1] + values[mid])
    return mean_conf, median_frac


def _match_notes(new_spans: List[Tuple[float, float]],
                 old_notes: List[Any],
                 tolerance: float) -> Dict[int, Any]:
    """Greedy one-to-one nearest-start matching between imported and existing notes."""
    pairs: List[Tuple[float, int, int]] = []
    for i, (start, _dur) in enumerate(new_spans):
        for j, old in enumerate(old_notes):
            distance = abs(start - float(get_field(old, "start_seconds", 0.0)))
            if distance <= tolerance:
                pairs.append((distance, i, j))
    pairs.sort()
    used_new, used_old = set(), set()
    matched: Dict[int, Any] = {}
    for _distance, i, j in pairs:
        if i in used_new or j in used_old:
            continue
        used_new.add(i)
        used_old.add(j)
        matched[i] = old_notes[j]
    return matched


def merge_ultrastar_into_chart(
    chart: Any,
    source: Any,
    match_tolerance_seconds: float = 0.25,
    melisma_text: str = MELISMA_TEXT,
    rebuild_phrases: bool = True,
) -> Any:
    """Merge hand-edited timing/pitch/text from a ``.txt`` back into a Chart.

    Everything the ``.txt`` cannot express is preserved: ``contour``,
    ``warnings``, ``song``, ``timing``, ``generator``.  Per-note ``confidence``
    survives via nearest-start matching against the existing notes, and falls
    back to the contour for notes the editor added.  ``centsOffset`` is kept only
    where the pitch is unchanged - if the human moved the note, the old deviation
    is a lie - and is otherwise re-derived from the contour when one exists.

    Returns a **new** Chart; the input is not mutated.
    """
    song = source if isinstance(source, UltrastarSong) else parse_ultrastar_text(str(source))

    old_notes = list(get_field(chart, "notes") or [])
    contour = get_field(chart, "contour")

    spans: List[Tuple[float, float]] = [song.note_seconds(n) for n in song.notes]
    matched = _match_notes(spans, old_notes, match_tolerance_seconds)

    new_notes: List[Any] = []
    phrase_members: Dict[int, List[int]] = {}
    first_of_line: Dict[int, bool] = {}

    for index, (us_note, (start, duration)) in enumerate(zip(song.notes, spans)):
        is_first = not first_of_line.get(us_note.line_index, False)
        first_of_line[us_note.line_index] = True

        midi = ultrastar_pitch_to_midi(us_note.pitch)
        syllable, role = _text_to_syllable(us_note.text, is_first, melisma_text)

        old = matched.get(index)
        confidence = None
        cents_offset = None
        if old is not None:
            confidence = get_field(old, "confidence")
            if int(get_field(old, "midi", midi) or 0) == midi:
                cents_offset = get_field(old, "cents_offset")

        if confidence is None or cents_offset is None:
            mean_conf, median_frac = _contour_stats(contour, start, start + duration)
            if confidence is None:
                confidence = mean_conf if mean_conf is not None else 0.0
            if cents_offset is None:
                cents_offset = round((median_frac - midi) * 100.0, 2) \
                    if median_frac is not None else 0.0

        new_notes.append(build_note(
            id=index,
            start_seconds=round(start, 6),
            duration_seconds=round(duration, 6),
            midi=midi,
            cents_offset=float(cents_offset),
            type=us_note.note_type,
            syllable=syllable,
            syllable_role=role,
            phrase_id=us_note.line_index,
            confidence=float(confidence),
        ))
        phrase_members.setdefault(us_note.line_index, []).append(index)

    updates: Dict[str, Any] = {"notes": new_notes}

    if rebuild_phrases:
        phrases = []
        for phrase_index, (line_index, ids) in enumerate(sorted(phrase_members.items())):
            members = [new_notes[i] for i in ids]
            words: List[str] = []
            for i in ids:
                note = new_notes[i]
                syllable = get_field(note, "syllable")
                if syllable is None:
                    continue
                if get_field(note, "syllable_role") == "wordStart" or not words:
                    words.append(str(syllable))
                else:
                    words[-1] = words[-1] + str(syllable)
            phrases.append(build_phrase(
                id=phrase_index,
                start_seconds=min(float(get_field(n, "start_seconds", 0.0)) for n in members),
                end_seconds=max(_note_end(n) for n in members),
                text=" ".join(words),
                note_ids=list(ids),
            ))
            for i in ids:
                new_notes[i] = set_fields(new_notes[i], phrase_id=phrase_index)
        updates["phrases"] = phrases
        updates["notes"] = new_notes

    return set_fields(chart, **updates)


def merge_ultrastar_file_into_chart(chart: Any, path: str, **kwargs: Any) -> Any:
    return merge_ultrastar_into_chart(chart, read_ultrastar(path), **kwargs)
