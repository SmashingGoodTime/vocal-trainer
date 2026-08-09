"""Chart data model — the normative on-disk format.

Mirrors DESIGN.md section 3 exactly. Field names on the dataclasses are the JSON
keys, camelCase and all, deliberately: a mismatch between the Python attribute and
the JSON key is the single easiest way to silently emit a chart the C++ runtime
cannot read, and there is no schema validator on the C++ side to catch it.

Units are seconds, always, absolute, relative to the reference audio file's own
timeline (DESIGN.md section 3 gives the five reasons beats were rejected).

Pure stdlib on purpose — numpy is not imported here so that the runtime tooling,
the tests and any future editor can load the model without the analysis stack.
"""

from __future__ import annotations

import json
import math
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Iterable, Optional

__all__ = [
    "SCHEMA_URI",
    "SCHEMA_VERSION",
    "NOTE_TYPES",
    "SYLLABLE_ROLES",
    "LYRIC_SOURCES",
    "MIDI_MIN",
    "MIDI_MAX",
    "ChartValidationError",
    "Generator",
    "Song",
    "Timing",
    "Phrase",
    "Note",
    "Contour",
    "Warning",
    "Chart",
]

SCHEMA_URI = "https://vocaltrainer.local/schema/chart-1.0.json"
SCHEMA_VERSION = "1.0"

NOTE_TYPES = ("normal", "golden", "freestyle", "rap", "breath")
SYLLABLE_ROLES = ("wordStart", "continuation", "melisma", "none")
LYRIC_SOURCES = ("forced-alignment", "asr", "manual", "none")

# Piano range. Anything outside this from a vocal f0 track is an octave error,
# not a singer.
MIDI_MIN = 21
MIDI_MAX = 108

# Absolute slop allowed when comparing float seconds. 1 us: far below the 18.75 ms
# UltraStar beat grid and the 30 ms onset-perception threshold, but large enough
# that float round-tripping through JSON never trips the overlap check.
_EPS = 1e-6


class ChartValidationError(ValueError):
    """Raised by Chart.validate() when the chart is internally inconsistent."""


# --------------------------------------------------------------------------- #
# helpers
# --------------------------------------------------------------------------- #

def _f(value: Any) -> float:
    """Coerce to a finite float or raise."""
    out = float(value)
    if not math.isfinite(out):
        raise ValueError(f"expected a finite number, got {value!r}")
    return out


def _opt_f(value: Any) -> Optional[float]:
    if value is None:
        return None
    return _f(value)


def _json_float(value: Any) -> Optional[float]:
    """NaN/inf/None -> None, so json.dump(allow_nan=False) never explodes."""
    if value is None:
        return None
    out = float(value)
    if not math.isfinite(out):
        return None
    return out


def _round(value: Optional[float], digits: int) -> Optional[float]:
    return None if value is None else round(float(value), digits)


# --------------------------------------------------------------------------- #
# generator
# --------------------------------------------------------------------------- #

@dataclass
class Generator:
    """Provenance. Which tools, which versions, produced this chart."""

    tool: str = "vocalchart"
    version: str = "0.1.0"
    pitchModel: str = "swift-f0 0.1.2"
    lyricSource: str = "none"          # forced-alignment | asr | manual | none
    lyricModel: Optional[str] = None
    separator: str = "none"            # none | bs-roformer ep_317 | htdemucs_ft
    generatedAt: Optional[str] = None  # ISO-8601 UTC, e.g. 2026-08-01T14:22:10Z

    def to_dict(self) -> dict[str, Any]:
        return {
            "tool": self.tool,
            "version": self.version,
            "pitchModel": self.pitchModel,
            "lyricSource": self.lyricSource,
            "lyricModel": self.lyricModel,
            "separator": self.separator,
            "generatedAt": self.generatedAt,
        }

    @classmethod
    def from_dict(cls, d: dict[str, Any]) -> "Generator":
        return cls(
            tool=d.get("tool", "vocalchart"),
            version=d.get("version", "0.1.0"),
            pitchModel=d.get("pitchModel", ""),
            lyricSource=d.get("lyricSource", "none"),
            lyricModel=d.get("lyricModel"),
            separator=d.get("separator", "none"),
            generatedAt=d.get("generatedAt"),
        )


# --------------------------------------------------------------------------- #
# song
# --------------------------------------------------------------------------- #

@dataclass
class Song:
    """Identity of the material the chart describes.

    `id` is the folder name and the ONLY thing that goes into plugin state
    (DESIGN.md section 3: absolute paths serialised into the .als break
    permanently when the library moves).
    """

    id: str
    title: Optional[str] = None
    artist: Optional[str] = None
    durationSeconds: float = 0.0
    referenceAudio: str = "stems/vocals.wav"   # relative to chart.json
    backingAudio: Optional[str] = None         # relative; Standalone target
    referenceAudioSha1: Optional[str] = None   # detects a swapped/re-rendered stem
    tuningHz: float = 440.0                    # librosa.estimate_tuning; NEVER assume 440
    detectedKey: Optional[str] = None
    vocalRangeMidi: Optional[dict[str, int]] = None  # {"min": 55, "max": 67}

    def __post_init__(self) -> None:
        self.durationSeconds = _f(self.durationSeconds)
        self.tuningHz = _f(self.tuningHz)
        if self.vocalRangeMidi is not None:
            self.vocalRangeMidi = {
                "min": int(self.vocalRangeMidi["min"]),
                "max": int(self.vocalRangeMidi["max"]),
            }

    def to_dict(self) -> dict[str, Any]:
        return {
            "id": self.id,
            "title": self.title,
            "artist": self.artist,
            "durationSeconds": self.durationSeconds,
            "referenceAudio": self.referenceAudio,
            "backingAudio": self.backingAudio,
            "referenceAudioSha1": self.referenceAudioSha1,
            "tuningHz": self.tuningHz,
            "detectedKey": self.detectedKey,
            "vocalRangeMidi": (
                None if self.vocalRangeMidi is None
                else {"min": self.vocalRangeMidi["min"], "max": self.vocalRangeMidi["max"]}
            ),
        }

    @classmethod
    def from_dict(cls, d: dict[str, Any]) -> "Song":
        return cls(
            id=d["id"],
            title=d.get("title"),
            artist=d.get("artist"),
            durationSeconds=d.get("durationSeconds", 0.0),
            referenceAudio=d.get("referenceAudio", "stems/vocals.wav"),
            backingAudio=d.get("backingAudio"),
            referenceAudioSha1=d.get("referenceAudioSha1"),
            tuningHz=d.get("tuningHz", 440.0),
            detectedKey=d.get("detectedKey"),
            vocalRangeMidi=d.get("vocalRangeMidi"),
        )


# --------------------------------------------------------------------------- #
# timing
# --------------------------------------------------------------------------- #

@dataclass
class Timing:
    """Seconds are normative. tempoMap is advisory, bar lines only."""

    unit: str = "seconds"
    epoch: str = "referenceAudioStart"
    arrangementAnchorSeconds: Optional[float] = None  # HINT ONLY; APVTS is authoritative
    rateFactor: float = 1.0
    tempoMap: list[dict[str, Any]] = field(default_factory=list)

    def __post_init__(self) -> None:
        self.arrangementAnchorSeconds = _opt_f(self.arrangementAnchorSeconds)
        self.rateFactor = _f(self.rateFactor)
        # Normalise so that a JSON round trip compares equal (tuples -> lists).
        normalised: list[dict[str, Any]] = []
        for point in self.tempoMap:
            entry: dict[str, Any] = {
                "timeSeconds": _f(point["timeSeconds"]),
                "bpm": _f(point["bpm"]),
            }
            sig = point.get("timeSignature")
            entry["timeSignature"] = None if sig is None else [int(sig[0]), int(sig[1])]
            normalised.append(entry)
        self.tempoMap = normalised

    def to_dict(self) -> dict[str, Any]:
        return {
            "unit": self.unit,
            "epoch": self.epoch,
            "arrangementAnchorSeconds": self.arrangementAnchorSeconds,
            "rateFactor": self.rateFactor,
            "tempoMap": [
                {
                    "timeSeconds": p["timeSeconds"],
                    "bpm": p["bpm"],
                    "timeSignature": (
                        None if p["timeSignature"] is None else list(p["timeSignature"])
                    ),
                }
                for p in self.tempoMap
            ],
        }

    @classmethod
    def from_dict(cls, d: dict[str, Any]) -> "Timing":
        return cls(
            unit=d.get("unit", "seconds"),
            epoch=d.get("epoch", "referenceAudioStart"),
            arrangementAnchorSeconds=d.get("arrangementAnchorSeconds"),
            rateFactor=d.get("rateFactor", 1.0),
            tempoMap=list(d.get("tempoMap") or []),
        )


# --------------------------------------------------------------------------- #
# phrase
# --------------------------------------------------------------------------- #

@dataclass
class Phrase:
    id: int
    startSeconds: float
    endSeconds: float
    text: str = ""
    noteIds: list[int] = field(default_factory=list)

    def __post_init__(self) -> None:
        self.id = int(self.id)
        self.startSeconds = _f(self.startSeconds)
        self.endSeconds = _f(self.endSeconds)
        self.noteIds = [int(n) for n in self.noteIds]

    @property
    def durationSeconds(self) -> float:
        return self.endSeconds - self.startSeconds

    def to_dict(self) -> dict[str, Any]:
        return {
            "id": self.id,
            "startSeconds": self.startSeconds,
            "endSeconds": self.endSeconds,
            "text": self.text,
            "noteIds": list(self.noteIds),
        }

    @classmethod
    def from_dict(cls, d: dict[str, Any]) -> "Phrase":
        return cls(
            id=d["id"],
            startSeconds=d["startSeconds"],
            endSeconds=d["endSeconds"],
            text=d.get("text", ""),
            noteIds=list(d.get("noteIds") or []),
        )


# --------------------------------------------------------------------------- #
# note
# --------------------------------------------------------------------------- #

@dataclass
class Note:
    """One note event on the highway.

    `syllable` is Optional[str] and None is legal EVERYWHERE. Whisper
    systematically deletes non-lexical vocables regardless of separation quality
    (arXiv:2506.15514) and those are exactly the long sustained notes the highway
    most wants to show. Any code that assumes a string here is a bug.
    """

    id: int
    startSeconds: float
    durationSeconds: float
    midi: int
    centsOffset: float = 0.0          # reference singer's deviation from `midi`
    type: str = "normal"              # normal | golden | freestyle | rap | breath
    syllable: Optional[str] = None
    syllableRole: str = "none"        # wordStart | continuation | melisma | none
    phraseId: Optional[int] = None
    confidence: float = 0.0           # mean SwiftF0 confidence over the note

    def __post_init__(self) -> None:
        self.id = int(self.id)
        self.startSeconds = _f(self.startSeconds)
        self.durationSeconds = _f(self.durationSeconds)
        self.midi = int(self.midi)
        self.centsOffset = _f(self.centsOffset)
        self.phraseId = None if self.phraseId is None else int(self.phraseId)
        self.confidence = _f(self.confidence)

    @property
    def endSeconds(self) -> float:
        return self.startSeconds + self.durationSeconds

    @property
    def fracMidi(self) -> float:
        """The reference singer's actual pitch, semitone + measured deviation."""
        return self.midi + self.centsOffset / 100.0

    def to_dict(self) -> dict[str, Any]:
        return {
            "id": self.id,
            "startSeconds": self.startSeconds,
            "durationSeconds": self.durationSeconds,
            "midi": self.midi,
            "centsOffset": self.centsOffset,
            "type": self.type,
            "syllable": self.syllable,
            "syllableRole": self.syllableRole,
            "phraseId": self.phraseId,
            "confidence": self.confidence,
        }

    @classmethod
    def from_dict(cls, d: dict[str, Any]) -> "Note":
        return cls(
            id=d["id"],
            startSeconds=d["startSeconds"],
            durationSeconds=d["durationSeconds"],
            midi=d["midi"],
            centsOffset=d.get("centsOffset", 0.0),
            type=d.get("type", "normal"),
            syllable=d.get("syllable"),
            syllableRole=d.get("syllableRole", "none"),
            phraseId=d.get("phraseId"),
            confidence=d.get("confidence", 0.0),
        )


# --------------------------------------------------------------------------- #
# contour
# --------------------------------------------------------------------------- #

@dataclass
class Contour:
    """The reference singer's raw f0 — the ghost line behind the note bars.

    The single best hedge in this format (DESIGN.md section 3): it sidesteps
    "did I segment that portamento correctly" for the part of the UI where it
    matters most. `fracMidi` entries are None where unvoiced.
    """

    rateHz: float = 100.0
    startSeconds: float = 0.0
    fracMidi: list[Optional[float]] = field(default_factory=list)
    confidence: list[float] = field(default_factory=list)

    def __post_init__(self) -> None:
        self.rateHz = _f(self.rateHz)
        self.startSeconds = _f(self.startSeconds)
        self.fracMidi = [_json_float(v) for v in self.fracMidi]
        self.confidence = [float(_json_float(v) or 0.0) for v in self.confidence]

    def __len__(self) -> int:
        return len(self.fracMidi)

    @property
    def durationSeconds(self) -> float:
        return len(self.fracMidi) / self.rateHz if self.rateHz > 0 else 0.0

    def time_of(self, index: int) -> float:
        return self.startSeconds + index / self.rateHz

    @classmethod
    def from_arrays(
        cls,
        frac_midi: Iterable[Optional[float]],
        confidence: Iterable[float],
        rate_hz: float = 100.0,
        start_seconds: float = 0.0,
        midi_digits: int = 3,
        confidence_digits: int = 3,
    ) -> "Contour":
        """Build from raw sequences, rounding as we go.

        Rounding happens here rather than in to_dict() so that to_dict/from_dict
        stays an exact round trip. ~17 KB per minute of JSON at these digits.
        """
        return cls(
            rateHz=rate_hz,
            startSeconds=start_seconds,
            fracMidi=[_round(_json_float(v), midi_digits) for v in frac_midi],
            confidence=[_round(_json_float(v) or 0.0, confidence_digits) or 0.0
                        for v in confidence],
        )

    def to_dict(self) -> dict[str, Any]:
        return {
            "rateHz": self.rateHz,
            "startSeconds": self.startSeconds,
            "fracMidi": [_json_float(v) for v in self.fracMidi],
            "confidence": [_json_float(v) or 0.0 for v in self.confidence],
        }

    @classmethod
    def from_dict(cls, d: dict[str, Any]) -> "Contour":
        return cls(
            rateHz=d.get("rateHz", 100.0),
            startSeconds=d.get("startSeconds", 0.0),
            fracMidi=list(d.get("fracMidi") or []),
            confidence=list(d.get("confidence") or []),
        )


# --------------------------------------------------------------------------- #
# warning
# --------------------------------------------------------------------------- #

@dataclass
class Warning:
    """A region the pipeline is not confident about. Advisory, never fatal."""

    code: str
    message: str = ""
    startSeconds: Optional[float] = None
    endSeconds: Optional[float] = None

    def __post_init__(self) -> None:
        self.startSeconds = _opt_f(self.startSeconds)
        self.endSeconds = _opt_f(self.endSeconds)

    def to_dict(self) -> dict[str, Any]:
        return {
            "code": self.code,
            "startSeconds": self.startSeconds,
            "endSeconds": self.endSeconds,
            "message": self.message,
        }

    @classmethod
    def from_dict(cls, d: dict[str, Any]) -> "Warning":
        return cls(
            code=d["code"],
            message=d.get("message", ""),
            startSeconds=d.get("startSeconds"),
            endSeconds=d.get("endSeconds"),
        )


# --------------------------------------------------------------------------- #
# chart
# --------------------------------------------------------------------------- #

@dataclass
class Chart:
    song: Song
    generator: Generator = field(default_factory=Generator)
    timing: Timing = field(default_factory=Timing)
    phrases: list[Phrase] = field(default_factory=list)
    notes: list[Note] = field(default_factory=list)
    contour: Optional[Contour] = None
    warnings: list[Warning] = field(default_factory=list)
    schemaVersion: str = SCHEMA_VERSION

    # ---------------------------------------------------------------- lookups

    def note_by_id(self, note_id: int) -> Optional[Note]:
        for n in self.notes:
            if n.id == note_id:
                return n
        return None

    def phrase_by_id(self, phrase_id: int) -> Optional[Phrase]:
        for p in self.phrases:
            if p.id == phrase_id:
                return p
        return None

    def vocal_range_midi(self) -> Optional[dict[str, int]]:
        pitched = [n.midi for n in self.notes if n.type != "breath"]
        if not pitched:
            return None
        return {"min": min(pitched), "max": max(pitched)}

    def mean_confidence(self) -> float:
        if not self.notes:
            return 0.0
        return sum(n.confidence for n in self.notes) / len(self.notes)

    def rebuild_phrase_note_ids(self) -> None:
        """Recompute every phrase's noteIds from the notes' phraseId back-links."""
        buckets: dict[int, list[int]] = {p.id: [] for p in self.phrases}
        for n in sorted(self.notes, key=lambda x: (x.startSeconds, x.id)):
            if n.phraseId is not None and n.phraseId in buckets:
                buckets[n.phraseId].append(n.id)
        for p in self.phrases:
            p.noteIds = buckets[p.id]

    # ------------------------------------------------------------- validation

    def validate(self) -> "Chart":
        """Raise ChartValidationError listing every internal inconsistency.

        All problems are collected and reported together — fixing one at a time
        across ten CLI runs is not a workflow.
        """
        problems: list[str] = []

        if self.schemaVersion != SCHEMA_VERSION:
            problems.append(
                f"schemaVersion is {self.schemaVersion!r}, this build writes/reads "
                f"{SCHEMA_VERSION!r}"
            )
        if self.timing.unit != "seconds":
            problems.append(
                f"timing.unit is {self.timing.unit!r}; only 'seconds' is normative "
                f"(DESIGN.md section 3)"
            )
        if self.song.durationSeconds < 0.0:
            problems.append(f"song.durationSeconds is negative ({self.song.durationSeconds})")
        if not (self.song.tuningHz > 0.0):
            problems.append(f"song.tuningHz must be > 0, got {self.song.tuningHz}")
        if self.generator.lyricSource not in LYRIC_SOURCES:
            problems.append(
                f"generator.lyricSource {self.generator.lyricSource!r} is not one of "
                f"{LYRIC_SOURCES}"
            )

        # --- phrases
        phrase_ids: set[int] = set()
        for p in self.phrases:
            if p.id in phrase_ids:
                problems.append(f"duplicate phrase id {p.id}")
            phrase_ids.add(p.id)
            if p.endSeconds < p.startSeconds - _EPS:
                problems.append(
                    f"phrase {p.id} ends before it starts "
                    f"({p.startSeconds:.3f} -> {p.endSeconds:.3f})"
                )
            if p.startSeconds < -_EPS:
                problems.append(f"phrase {p.id} has negative startSeconds ({p.startSeconds})")

        # --- notes
        note_ids: set[int] = set()
        for n in self.notes:
            if n.id in note_ids:
                problems.append(f"duplicate note id {n.id}")
            note_ids.add(n.id)

            if n.durationSeconds < 0.0:
                problems.append(
                    f"note {n.id} has negative durationSeconds ({n.durationSeconds})"
                )
            elif n.durationSeconds == 0.0:
                problems.append(f"note {n.id} has zero durationSeconds")
            if n.startSeconds < -_EPS:
                problems.append(f"note {n.id} has negative startSeconds ({n.startSeconds})")
            if not (MIDI_MIN <= n.midi <= MIDI_MAX):
                problems.append(
                    f"note {n.id} midi {n.midi} is outside {MIDI_MIN}..{MIDI_MAX} "
                    f"(almost certainly an octave error, not a singer)"
                )
            if n.type not in NOTE_TYPES:
                problems.append(f"note {n.id} type {n.type!r} is not one of {NOTE_TYPES}")
            if n.syllableRole not in SYLLABLE_ROLES:
                problems.append(
                    f"note {n.id} syllableRole {n.syllableRole!r} is not one of "
                    f"{SYLLABLE_ROLES}"
                )
            if not (0.0 <= n.confidence <= 1.0):
                problems.append(
                    f"note {n.id} confidence {n.confidence} is outside 0..1"
                )
            if abs(n.centsOffset) > 100.0:
                problems.append(
                    f"note {n.id} centsOffset {n.centsOffset:.1f} exceeds +/-100 cents; "
                    f"it should have been snapped to a different semitone"
                )
            if n.phraseId is not None and n.phraseId not in phrase_ids:
                problems.append(
                    f"note {n.id} references phraseId {n.phraseId}, which does not exist"
                )
            if self.song.durationSeconds > 0.0 and n.endSeconds > self.song.durationSeconds + 0.5:
                problems.append(
                    f"note {n.id} ends at {n.endSeconds:.3f}s, past song.durationSeconds "
                    f"({self.song.durationSeconds:.3f}s)"
                )

        # --- overlaps (monophonic melody: notes may abut, never overlap)
        ordered = sorted(self.notes, key=lambda x: (x.startSeconds, x.id))
        for prev, cur in zip(ordered, ordered[1:]):
            if cur.startSeconds < prev.endSeconds - _EPS:
                problems.append(
                    f"notes {prev.id} and {cur.id} overlap: {prev.id} ends at "
                    f"{prev.endSeconds:.3f}s, {cur.id} starts at {cur.startSeconds:.3f}s"
                )

        # --- phrase <-> note cross references
        for p in self.phrases:
            seen: set[int] = set()
            for nid in p.noteIds:
                if nid in seen:
                    problems.append(f"phrase {p.id} lists note {nid} more than once")
                seen.add(nid)
                note = self.note_by_id(nid)
                if note is None:
                    problems.append(
                        f"phrase {p.id} lists note id {nid}, which does not exist"
                    )
                elif note.phraseId != p.id:
                    problems.append(
                        f"phrase {p.id} lists note {nid}, but that note's phraseId is "
                        f"{note.phraseId}"
                    )

        # --- contour
        if self.contour is not None:
            c = self.contour
            if not (c.rateHz > 0.0):
                problems.append(f"contour.rateHz must be > 0, got {c.rateHz}")
            if len(c.confidence) != len(c.fracMidi):
                problems.append(
                    f"contour fracMidi has {len(c.fracMidi)} entries but confidence has "
                    f"{len(c.confidence)}"
                )
            if c.rateHz > 0.0 and self.song.durationSeconds > 0.0 and len(c.fracMidi) > 0:
                expected = (self.song.durationSeconds - c.startSeconds) * c.rateHz
                tolerance = max(2.0, 0.01 * expected)
                if abs(len(c.fracMidi) - expected) > tolerance:
                    problems.append(
                        f"contour has {len(c.fracMidi)} frames but "
                        f"({self.song.durationSeconds:.3f}s - {c.startSeconds:.3f}s) at "
                        f"{c.rateHz} Hz implies ~{expected:.0f} "
                        f"(tolerance {tolerance:.0f})"
                    )

        if problems:
            raise ChartValidationError(
                f"chart {self.song.id!r} failed validation with {len(problems)} problem(s):\n  - "
                + "\n  - ".join(problems)
            )
        return self

    # ------------------------------------------------------------ (de)serialise

    def to_dict(self) -> dict[str, Any]:
        """Stable key order, matching the DESIGN.md section 3 schema listing."""
        return {
            "$schema": SCHEMA_URI,
            "schemaVersion": self.schemaVersion,
            "generator": self.generator.to_dict(),
            "song": self.song.to_dict(),
            "timing": self.timing.to_dict(),
            "phrases": [p.to_dict() for p in self.phrases],
            "notes": [n.to_dict() for n in self.notes],
            "contour": None if self.contour is None else self.contour.to_dict(),
            "warnings": [w.to_dict() for w in self.warnings],
        }

    @classmethod
    def from_dict(cls, d: dict[str, Any]) -> "Chart":
        contour = d.get("contour")
        return cls(
            song=Song.from_dict(d["song"]),
            generator=Generator.from_dict(d.get("generator") or {}),
            timing=Timing.from_dict(d.get("timing") or {}),
            phrases=[Phrase.from_dict(p) for p in (d.get("phrases") or [])],
            notes=[Note.from_dict(n) for n in (d.get("notes") or [])],
            contour=None if contour is None else Contour.from_dict(contour),
            warnings=[Warning.from_dict(w) for w in (d.get("warnings") or [])],
            schemaVersion=d.get("schemaVersion", SCHEMA_VERSION),
        )

    def to_json(self, indent: int = 2) -> str:
        return json.dumps(
            self.to_dict(),
            indent=indent,
            ensure_ascii=False,
            sort_keys=False,   # key order comes from to_dict, not from sorting
            allow_nan=False,   # NaN/Infinity are not JSON; _json_float already scrubbed
        )

    def write_json(self, path: str | Path, indent: int = 2) -> Path:
        """Write chart.json. UTF-8, no BOM, LF, trailing newline.

        Atomic (temp file + os.replace): chart.json is read by the JUCE app,
        which may re-load it while a rebuild is running — it must never be able
        to observe a truncated file where a good one was."""
        from .paths import atomic_write_text

        return atomic_write_text(path, self.to_json(indent=indent) + "\n")

    @classmethod
    def read_json(cls, path: str | Path) -> "Chart":
        return cls.from_dict(json.loads(Path(path).read_text(encoding="utf-8")))
