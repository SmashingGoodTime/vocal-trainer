"""vocalchart - the offline chart-preparation CLI.

    vocalchart add-song <audioFile>  EVERYTHING: one file in, a playable song out
    vocalchart import-stems <dir>    same, from a multitrack folder (better)

    vocalchart separate <songDir>  stage 0 (vocal/instrumental split)
    vocalchart f0       <songDir>  stage 1 only
    vocalchart notes    <songDir>  stage 2 (segmentation)
    vocalchart lyrics   <songDir>  stage 3 (word timing)
    vocalchart build    <songDir>   stages 0-5 -> chart.json + chart.ultrastar.txt
    vocalchart inspect  <songDir>   human summary of what came out

Every stage caches to songs/<songId>/cache/ and is independently re-runnable;
--force bypasses the cache, -v prints progress. Chart prep is a once-per-song
cost with expected interactive fixups, so caching matters more than throughput
(DESIGN.md section 4).

`build` and `add-song` also take --progress-json, which switches stdout to the
one-object-per-line protocol in progress.py. That is what the plugin's
"Add song..." button consumes; nothing else should need it.
"""

from __future__ import annotations

import json
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path
from typing import Callable, Optional

import click

from . import __version__
from .chart import Chart, Contour, Generator, Note, Phrase, Song, Timing, Warning
from .chart import ChartValidationError
from .f0 import VOCAL_FMAX_HZ, VOCAL_FMIN_HZ, VOICING_CONFIDENCE, F0Result
from .paths import AUDIO_EXTENSIONS, SongPaths, sha1_of_file
from .progress import Reporter

CONTOUR_RATE_HZ = 100.0        # DESIGN.md section 3: contour.rateHz
DEFAULT_PHRASE_GAP = 0.60      # seconds of silence that starts a new phrase

_NOTE_NAMES = ("C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B")


# --------------------------------------------------------------------------- #
# small helpers
# --------------------------------------------------------------------------- #

def _midi_name(midi: int) -> str:
    return f"{_NOTE_NAMES[int(midi) % 12]}{int(midi) // 12 - 1}"


def _fmt_time(seconds: float) -> str:
    seconds = max(0.0, float(seconds))
    return f"{int(seconds) // 60}:{seconds % 60:06.3f}"


def _progress(verbose: bool) -> Callable[[str], None]:
    if not verbose:
        return lambda _msg: None

    def say(msg: str) -> None:
        # ASCII only: the default Windows console is cp1252 and click.echo of a
        # box-drawing bullet raises UnicodeEncodeError there.
        click.echo(click.style("  * ", fg="cyan") + msg, err=True)

    return say


def _resolve_song(song_dir: str) -> SongPaths:
    song = SongPaths(Path(song_dir))
    if not song.exists():
        raise click.ClickException(f"no such song folder: {song.root}")
    return song


class Cancelled(click.ClickException):
    """The caller asked for the run to stop. Exit code 130, like a Ctrl-C."""

    exit_code = 130

    def __init__(self, message: str = "cancelled") -> None:
        super().__init__(message)


def _cancel_check(path: Optional[Path]) -> Callable[[], bool]:
    """A "should I stop?" predicate backed by the existence of a file.

    A file rather than a signal because the work that needs stopping is a
    GRANDCHILD process (the separator, in its own venv). Killing this CLI from
    outside would orphan a multi-minute GPU job with nothing left holding its
    handle; a flag file lets the process that owns it do the killing.
    """
    if path is None:
        return lambda: False
    target = Path(path)
    return lambda: target.exists()


def _abort_if_cancelled(cancel: Callable[[], bool]) -> None:
    if cancel():
        raise Cancelled("cancelled before the next stage")


def _guard(rep: "Reporter", run: Callable[[], "Chart"]) -> "Chart":
    """Run a pipeline, making sure a failure reaches the machine protocol.

    Without this a crash under --progress-json produces a traceback on stderr
    and *silence* on stdout, which the plugin cannot distinguish from a job
    still grinding away on a long separation.
    """
    try:
        return run()
    except click.ClickException as exc:
        rep.error(exc.format_message())
        raise
    except Exception as exc:                          # noqa: BLE001 - re-raised
        import traceback

        rep.error(f"{type(exc).__name__}: {exc}", traceback.format_exc())
        raise


def _load_segmenter():
    """Import stage 2 lazily so the rest of the CLI works before it exists."""
    try:
        from .segment import SegmentConfig, segment_notes  # type: ignore
    except ImportError as exc:  # pragma: no cover - depends on sibling module
        raise click.ClickException(
            f"stage 2 (note segmentation) is unavailable: {exc}\n"
            f"vocalchart.segment must provide segment_notes(f0, cfg) and SegmentConfig."
        ) from exc
    return SegmentConfig, segment_notes


def _load_ultrastar_writer():
    try:
        from .ultrastar import write_ultrastar  # type: ignore
    except ImportError as exc:  # pragma: no cover - depends on sibling module
        raise click.ClickException(
            f"the UltraStar exporter is unavailable: {exc}\n"
            f"vocalchart.ultrastar must provide write_ultrastar(chart, path)."
        ) from exc
    return write_ultrastar


def _audio_duration_seconds(path: Path, fallback: float) -> float:
    try:
        import librosa

        return float(librosa.get_duration(path=str(path)))
    except Exception:
        return float(fallback)


# --------------------------------------------------------------------------- #
# stage 2 cache
# --------------------------------------------------------------------------- #

def _write_notes_cache(path: Path, notes: list[Note], f0: F0Result) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    payload = {
        "tool": "vocalchart",
        "version": __version__,
        "generatedAt": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "tuningHz": f0.tuning_hz,
        "hopSeconds": f0.hop_seconds,
        "notes": [n.to_dict() for n in notes],
    }
    path.write_text(
        json.dumps(payload, indent=2, ensure_ascii=False, allow_nan=False) + "\n",
        encoding="utf-8",
        newline="\n",
    )


def _read_notes_cache(path: Path) -> list[Note]:
    payload = json.loads(path.read_text(encoding="utf-8"))
    return [Note.from_dict(n) for n in payload.get("notes", [])]


# --------------------------------------------------------------------------- #
# pipeline pieces
# --------------------------------------------------------------------------- #

def _run_separate(
    song: SongPaths,
    force: bool,
    rep: "Reporter",
    *,
    model: Optional[str] = None,
    python: Optional[str] = None,
    required: bool = False,
    cancel: Optional[Callable[[], bool]] = None,
) -> str:
    """Stage 0. Returns the value for chart.generator.separator.

    Non-fatal by default. A song folder that already holds a real stem does not
    need this stage at all, and a missing torch venv should not stop someone
    charting the multitracks they already have - so unless `required`, a failure
    degrades to "chart whatever audio is present" with a warning.
    """
    from .separate import (DEFAULT_MODEL, SeparationCancelled, SeparationError,
                           separate_song)

    if song.vocals_wav.exists() and not force:
        # Fast path: don't even look for the interpreter. This is the common
        # case for import-stems folders and it must stay free.
        from .separate import _read_provenance      # noqa: PLC0415 - internal

        prov = _read_provenance(song)
        if prov is None:
            rep.log("stage 0 skipped: stems/vocals.wav is a real stem, not a separation")
            return "none"

    rep.stage("separate")
    try:
        result = separate_song(
            song,
            force=force,
            model=model or DEFAULT_MODEL,
            python=python,
            progress=rep.say,
            on_percent=lambda pct, msg: rep.percent(pct, msg),
            cancel=cancel,
        )
    except SeparationCancelled as exc:
        # Never degrade a cancellation into "carry on without separation" - the
        # user asked for the job to stop, not for a worse chart.
        raise Cancelled(str(exc)) from exc
    except SeparationError as exc:
        if required:
            raise click.ClickException(str(exc)) from exc
        rep.log(f"WARNING: stage 0 unavailable ({exc.args[0].splitlines()[0]})")
        rep.log("continuing with whatever audio is already in the song folder")
        return "none"

    if result.skipped:
        rep.log(f"stage 0 skipped: {result.reason}")
    else:
        rep.log(f"stage 0 done on {result.device}")
    return result.separator_id if song.vocals_wav.exists() else "none"


def _run_f0(song: SongPaths, force: bool, say: Callable[[str], None]) -> F0Result:
    from .f0 import f0_for_song

    return f0_for_song(song, force=force, progress=say)


def _run_notes(
    song: SongPaths,
    f0: F0Result,
    force: bool,
    say: Callable[[str], None],
) -> list[Note]:
    from .paths import is_cache_fresh

    if not force and is_cache_fresh(song.notes_json, song.f0_npz, song.analysis_input()):
        try:
            notes = _read_notes_cache(song.notes_json)
        except Exception as exc:
            say(f"notes cache unreadable ({exc}); resegmenting")
        else:
            say(f"notes cache hit: {song.notes_json} ({len(notes)} notes)")
            return notes

    SegmentConfig, segment_notes = _load_segmenter()
    say("segmenting notes (stage 2)")

    # SPECTRAL FLUX IS NOT OPTIONAL IN PRACTICE.
    #
    # Without it the onset signal is only |d(cents)/dt| + (1 - confidence), and
    # NEITHER of those fires when consecutive syllables are sung at the SAME
    # pitch with clean legato. Measured on the Mr Brightside lead vocal, whose
    # verse melody is near-monotone: 79 of 167 notes came out at MIDI 61,
    # totalling 94 s of the 148 s sung, with single "notes" up to 4.2 s long.
    # The pitch was right; the rhythm was gone, because every repeated-pitch
    # syllable had fused into one bar.
    #
    # Flux is what detects a new syllable at an unchanged pitch. Degrade
    # gracefully if the audio cannot be read - a slightly fused chart still
    # beats no chart.
    flux = flux_times = None

    level_db = level_times = None

    try:
        from .f0 import load_audio_16k
        from .segment import frame_level_db_from_audio, spectral_flux_from_audio

        audio = load_audio_16k(song.analysis_input())
        flux, flux_times = spectral_flux_from_audio(audio, 16000, times=f0.times)
        say("computed spectral flux for onset detection")

        # Step 11 needs per-frame level to reject reverb tails, which SwiftF0
        # tracks confidently because they ARE pitched.
        level_db, level_times = frame_level_db_from_audio(audio, 16000, times=f0.times)
    except Exception as exc:                      # noqa: BLE001 - non-fatal by design
        say(f"WARNING: no spectral flux ({exc}); repeated same-pitch syllables "
            f"will fuse into single long notes")

    # SONG-ADAPTIVE PLAUSIBILITY BAND.
    #
    # SwiftF0's fmin/fmax do NOT clamp pitch_hz - verified by introspection, and
    # documented in f0.py: they gate only the `voicing` flag we re-derive. So
    # pitch_hz really does contain values from 58 Hz to 2093 Hz, and the raw
    # octave errors are all still in there.
    #
    # Step 2 already masks against cfg.min_hz/max_hz, but the 60-1100 Hz default
    # spans MIDI 34-86 and is far too wide to catch anything. Narrowing it to the
    # singer's actual range is the offline twin of the runtime chart-informed
    # range constraint, and it is the only mechanism available here: SwiftF0
    # cannot be told to search a narrower band.
    #
    # The band is derived from the INTERQUARTILE core, not the full range, so the
    # octave errors cannot widen the band that is meant to exclude them. +/- 12
    # semitones of margin keeps genuine octave leaps.
    cfg = SegmentConfig()

    try:
        import numpy as np

        voiced = f0.confidence >= cfg.confidence_threshold

        if int(np.count_nonzero(voiced)) >= 50:
            midi = 69.0 + 12.0 * np.log2(np.clip(f0.hz[voiced], 1e-6, None) / f0.tuning_hz)
            lo_midi = float(np.percentile(midi, 25)) - 12.0
            hi_midi = float(np.percentile(midi, 75)) + 12.0
            cfg.min_hz = max(cfg.min_hz, float(f0.tuning_hz * 2.0 ** ((lo_midi - 69.0) / 12.0)))
            cfg.max_hz = min(cfg.max_hz, float(f0.tuning_hz * 2.0 ** ((hi_midi - 69.0) / 12.0)))
            say(f"vocal band {cfg.min_hz:.0f}-{cfg.max_hz:.0f} Hz "
                f"(MIDI {lo_midi:.0f}-{hi_midi:.0f}) from this singer's range")
    except Exception as exc:                      # noqa: BLE001 - non-fatal by design
        say(f"WARNING: could not derive a vocal band ({exc}); using the wide default")

    produced = segment_notes(f0, cfg,
                             spectral_flux=flux, spectral_flux_times=flux_times,
                             level_db=level_db, level_db_times=level_times)

    if getattr(produced, "rejected_silent", 0):
        say(f"rejected {produced.rejected_silent} silent notes "
            f"(>{abs(cfg.reject_level_db):.0f} dB below this song's median note level "
            f"of {produced.level_reference_db:.1f} dBFS) - reverb tails, not singing")
    # segment_notes returns a SegmentationResult carrying the notes plus every
    # intermediate array for the tuning harness. Accept a bare list too, so the
    # two modules stay independently replaceable.
    notes = list(getattr(produced, "notes", produced))
    # Renumber defensively: ids are cross-referenced by phrases[].noteIds and by
    # the C++ runtime, so a duplicate or gap here is a downstream crash.
    for index, note in enumerate(sorted(notes, key=lambda n: n.startSeconds)):
        note.id = index
    notes = sorted(notes, key=lambda n: n.id)
    _write_notes_cache(song.notes_json, notes, f0)
    say(f"wrote {song.notes_json} ({len(notes)} notes)")
    return notes


def _build_phrases(notes: list[Note], gap_seconds: float) -> list[Phrase]:
    """Provisional phrases from silence gaps.

    Real phrasing comes from stage 3 (forced alignment) in a later milestone.
    Until then a gap-based split keeps every note's phraseId resolvable, which
    validate() requires, and gives the highway something to scroll by.
    """
    phrases: list[Phrase] = []
    current: list[Note] = []

    def flush() -> None:
        if not current:
            return
        pid = len(phrases)
        for n in current:
            n.phraseId = pid
        phrases.append(
            Phrase(
                id=pid,
                startSeconds=current[0].startSeconds,
                endSeconds=current[-1].endSeconds,
                text="",
                noteIds=[n.id for n in current],
            )
        )
        current.clear()

    for note in sorted(notes, key=lambda n: n.startSeconds):
        if current and note.startSeconds - current[-1].endSeconds > gap_seconds:
            flush()
        current.append(note)
    flush()
    return phrases


def _build_contour(f0: F0Result, duration_seconds: float) -> Contour:
    frac_midi, confidence = f0.resample_frac_midi(
        rate_hz=CONTOUR_RATE_HZ,
        start_seconds=0.0,
        duration_seconds=duration_seconds,
        confidence_threshold=VOICING_CONFIDENCE,
    )
    return Contour.from_arrays(
        frac_midi.tolist(), confidence.tolist(),
        rate_hz=CONTOUR_RATE_HZ, start_seconds=0.0,
    )


def _low_confidence_warnings(
    f0: F0Result,
    threshold: float = 0.40,
    min_span_seconds: float = 1.0,
) -> list[Warning]:
    """Flag sustained low-confidence stretches - breathy singing, or backing
    vocals bleeding through a bad separation. Advisory only."""
    import numpy as np

    if len(f0) == 0:
        return []
    low = f0.confidence < threshold
    warnings: list[Warning] = []
    start_idx: Optional[int] = None
    for i, is_low in enumerate(np.append(low, False)):
        if is_low and start_idx is None:
            start_idx = i
        elif not is_low and start_idx is not None:
            t0 = float(f0.times[start_idx])
            t1 = float(f0.times[min(i, len(f0) - 1)])
            if t1 - t0 >= min_span_seconds:
                mean_conf = float(np.mean(f0.confidence[start_idx:i]))
                warnings.append(
                    Warning(
                        code="LOW_CONFIDENCE_REGION",
                        startSeconds=round(t0, 3),
                        endSeconds=round(t1, 3),
                        message=(
                            f"Mean f0 confidence {mean_conf:.2f} - likely breathy or "
                            f"backing-vocal contamination."
                        ),
                    )
                )
            start_idx = None
    return warnings


def _contamination_warning(notes: list[Note]) -> Optional[Warning]:
    """Flag a chart whose pitches are BIMODAL - two sources in one stem.

    A vocal stem is supposed to contain one singer. When a large share of the
    charted time sits more than an octave from the median AND there is a clear
    empty gap between the two clusters, what is usually in the file is bleed
    from another instrument.

    Measured example: the Seven Nation Army lead-vocal stem charts 155 notes at
    MIDI 60-69 and 27 at MIDI 30-39 with NOTHING in between - the bass riff. A
    unimodal spread (a genuinely wide-ranging singer) has no such gap, which is
    what separates the two cases. Better to say so than to let the singer find
    out mid-song that the highway is charting a guitar.
    """
    import numpy as np

    if len(notes) < 20:
        return None

    midi = np.array([n.midi for n in notes], dtype=float)
    dur = np.array([n.durationSeconds for n in notes], dtype=float)
    total = float(dur.sum())

    if total <= 0.0:
        return None

    median = float(np.median(midi))
    far = np.abs(midi - median) > 12.0
    share = float(dur[far].sum() / total)

    if share < 0.08:
        return None

    # Is there a genuine empty gap, or is this one wide-ranging singer?
    occupied = np.unique(np.round(midi).astype(int))
    gaps = np.diff(occupied)
    biggest_gap = int(gaps.max()) if gaps.size else 0

    if biggest_gap < 6:
        return None      # continuous spread - a wide range, not contamination

    outlier_side = "below" if np.median(midi[far]) < median else "above"

    return Warning(
        code="SUSPECTED_STEM_CONTAMINATION",
        startSeconds=0.0,
        endSeconds=round(float(max(n.startSeconds + n.durationSeconds for n in notes)), 3),
        message=(
            f"{share * 100:.0f}% of the charted time sits more than an octave {outlier_side} "
            f"the median pitch (MIDI {median:.0f}), with an empty {biggest_gap}-semitone gap "
            f"between the two clusters. That pattern means another instrument is bleeding "
            f"into this vocal stem, not that the singer has a huge range. Check the stem "
            f"before trusting this chart."
        ),
    )


def _assemble_chart(
    song: SongPaths,
    f0: F0Result,
    notes: list[Note],
    phrase_gap: float,
    say: Callable[[str], None],
    separator: str = "none",
) -> Chart:
    audio_path = song.analysis_input()
    duration = _audio_duration_seconds(audio_path, f0.duration_seconds)

    phrases = _build_phrases(notes, phrase_gap)
    say(f"{len(phrases)} provisional phrases from {phrase_gap * 1000:.0f} ms silence gaps")

    chart = Chart(
        song=Song(
            id=song.song_id,
            title=None,
            artist=None,
            durationSeconds=round(duration, 3),
            referenceAudio=song.reference_audio_relative(),
            backingAudio=song.backing_audio_relative(),
            referenceAudioSha1=sha1_of_file(audio_path),
            tuningHz=round(f0.tuning_hz, 4),
            detectedKey=None,
            vocalRangeMidi=None,
        ),
        generator=Generator(
            tool="vocalchart",
            version=__version__,
            pitchModel="swift-f0 0.1.2",
            lyricSource="none",     # overwritten by the caller once stage 3 ran
            lyricModel=None,
            separator=separator,
            generatedAt=datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        ),
        timing=Timing(
            unit="seconds",
            epoch="referenceAudioStart",
            arrangementAnchorSeconds=None,   # captured in the plugin, not here
            rateFactor=1.0,
            tempoMap=[],
        ),
        phrases=phrases,
        notes=sorted(notes, key=lambda n: n.id),
        contour=_build_contour(f0, duration),
        warnings=_low_confidence_warnings(f0),
    )

    if f0.meta.get("tuningReliable") is False and not f0.meta.get("tuningExplicit", False):
        cents = f0.meta.get("tuningMeasuredCentsFrom440", 0.0)
        secs = f0.meta.get("tuningSeconds", 0.0)
        if not f0.meta.get("tuningFromMix", False):
            reason = (
                f"it was measured from '{f0.meta.get('tuningSource', '?')}' rather than a "
                f"full mix, and vibrato smears the spectral peaks librosa.estimate_tuning "
                f"relies on"
            )
        else:
            reason = (
                f"it was measured from only {secs:.1f} s of audio, too little for a stable "
                f"histogram"
            )
        chart.warnings.insert(0, Warning(
            code="UNRELIABLE_TUNING",
            startSeconds=0.0,
            endSeconds=round(duration, 3),
            message=(
                f"Tuning measured at {cents:+.1f} cents from A=440, but {reason}. "
                f"That estimate was REJECTED and A=440 used instead, because a wrong "
                f"reference offsets every note in the chart. If this record is genuinely "
                f"not at concert pitch, supply source.* (the full mix) alongside the "
                f"stem, or set song.tuningHz by hand."
            ),
        ))
    if (contamination := _contamination_warning(chart.notes)) is not None:
        chart.warnings.insert(0, contamination)

    chart.song.vocalRangeMidi = chart.vocal_range_midi()
    return chart


# --------------------------------------------------------------------------- #
# CLI
# --------------------------------------------------------------------------- #

_song_arg = click.argument(
    "song_dir",
    type=click.Path(file_okay=False, dir_okay=True, path_type=str),
)


def _flags(func):
    """--force / -v on every command, in addition to the group-level pair."""
    func = click.option(
        "-v", "--verbose", is_flag=True, default=False, help="Print stage progress."
    )(func)
    func = click.option(
        "-f", "--force", is_flag=True, default=False, help="Ignore caches and recompute."
    )(func)
    return func


def _merged(ctx: click.Context, force: bool, verbose: bool) -> tuple[bool, bool]:
    obj = ctx.obj or {}
    return bool(force or obj.get("force")), bool(verbose or obj.get("verbose"))


@click.group(context_settings={"help_option_names": ["-h", "--help"]})
@click.version_option(__version__, prog_name="vocalchart")
@click.option("-v", "--verbose", is_flag=True, default=False, help="Print stage progress.")
@click.option("-f", "--force", is_flag=True, default=False, help="Ignore caches and recompute.")
@click.pass_context
def main(ctx: click.Context, verbose: bool, force: bool) -> None:
    """Turn a song into a vocal note chart.

    SONG_DIR is a songs/<songId>/ folder containing source.* and/or stems/vocals.wav.
    """
    ctx.ensure_object(dict)
    ctx.obj["verbose"] = verbose
    ctx.obj["force"] = force


# ------------------------------------------------------------------- stage 1

@main.command("f0")
@_song_arg
@_flags
@click.option("--fmin", default=VOCAL_FMIN_HZ, show_default=True,
              help="Lower detection bound in Hz (SwiftF0 model floor is 46.875).")
@click.option("--fmax", default=VOCAL_FMAX_HZ, show_default=True,
              help="Upper detection bound in Hz (SwiftF0 model ceiling is 2093.75).")
@click.pass_context
def cmd_f0(ctx: click.Context, song_dir: str, force: bool, verbose: bool,
           fmin: float, fmax: float) -> None:
    """Stage 1 - SwiftF0 pitch extraction to cache/f0.npz."""
    from .f0 import f0_for_song

    force, verbose = _merged(ctx, force, verbose)
    say = _progress(verbose)
    song = _resolve_song(song_dir)
    song.ensure_dirs()

    result = f0_for_song(song, force=force, fmin_hz=fmin, fmax_hz=fmax, progress=say)
    voiced = result.voiced(VOICING_CONFIDENCE)
    click.echo(
        f"{song.f0_npz}: {len(result)} frames, hop {result.hop_seconds * 1000:.1f} ms, "
        f"{float(voiced.mean()) * 100 if len(result) else 0.0:.1f}% voiced, "
        f"tuning A4 = {result.tuning_hz:.2f} Hz"
    )


# ------------------------------------------------------------------- stage 2

@main.command("notes")
@_song_arg
@_flags
@click.pass_context
def cmd_notes(ctx: click.Context, song_dir: str, force: bool, verbose: bool) -> None:
    """Stage 2 - note segmentation to cache/notes.json."""
    force, verbose = _merged(ctx, force, verbose)
    say = _progress(verbose)
    song = _resolve_song(song_dir)
    song.ensure_dirs()

    f0 = _run_f0(song, force, say)
    notes = _run_notes(song, f0, force, say)

    if not notes:
        click.echo(f"{song.notes_json}: 0 notes - check that the input actually has vocals.")
        return
    lo = min(n.midi for n in notes)
    hi = max(n.midi for n in notes)
    click.echo(
        f"{song.notes_json}: {len(notes)} notes, "
        f"{_midi_name(lo)}..{_midi_name(hi)} (MIDI {lo}..{hi})"
    )


# ------------------------------------------------------------------ full run

def _pipeline(
    song: SongPaths,
    rep: Reporter,
    *,
    force: bool,
    phrase_gap: float,
    no_ultrastar: bool,
    no_lyrics: bool,
    model: str,
    separate: bool,
    separator_model: Optional[str] = None,
    separator_python: Optional[str] = None,
    title: Optional[str] = None,
    artist: Optional[str] = None,
    cancel: Optional[Callable[[], bool]] = None,
) -> Chart:
    """Stages 0-5. Shared by `build` and `add-song` so there is exactly one
    ordering of the pipeline in the codebase."""
    song.ensure_dirs()
    stop = cancel or (lambda: False)

    separator = "none"
    if separate:
        separator = _run_separate(song, force, rep, model=separator_model,
                                  python=separator_python, cancel=stop)

    # Stage boundaries are the cancellation points. Nothing between them is long
    # enough to be worth interrupting, and every one of them leaves a valid
    # cache behind, so a cancelled run resumes rather than restarts.
    _abort_if_cancelled(stop)
    rep.stage("f0")
    f0 = _run_f0(song, force, rep.say)

    _abort_if_cancelled(stop)
    rep.stage("notes")
    notes = _run_notes(song, f0, force, rep.say)

    # Stage 3. Deliberately AFTER segmentation and deliberately non-fatal: the
    # syllables decorate notes that already exist, so a transcription failure
    # costs the lyrics and nothing else.
    if not no_lyrics:
        _abort_if_cancelled(stop)
        rep.stage("lyrics")
        words, lyric_source = _run_lyrics(song, force, rep.say, model)

        if words:
            from .lyrics import assign_syllables_to_notes

            assign_syllables_to_notes(notes, words, progress=rep.say)
    else:
        lyric_source = "none"

    rep.stage("chart")
    chart = _assemble_chart(song, f0, notes, phrase_gap, rep.say, separator=separator)
    chart.generator.lyricSource = lyric_source
    if title:
        chart.song.title = title
    if artist:
        chart.song.artist = artist

    if lyric_source != "none":
        chart.generator.lyricModel = f"faster-whisper {model}"

    try:
        chart.validate()
    except ChartValidationError as exc:
        raise click.ClickException(str(exc)) from exc

    chart.write_json(song.chart_json)
    rep.log(f"wrote {song.chart_json} ({len(chart.notes)} notes, "
            f"{len(chart.phrases)} phrases)")

    if not no_ultrastar:
        write_ultrastar = _load_ultrastar_writer()
        write_ultrastar(chart, str(song.chart_ultrastar_txt))
        rep.log(f"wrote {song.chart_ultrastar_txt}")

    return chart


def _report_chart(song: SongPaths, chart: Chart, rep: Reporter) -> None:
    """Final human summary + the machine `done` event."""
    rep.done(
        songId=song.song_id,
        songDir=str(song.root),
        chart=str(song.chart_json),
        notes=len(chart.notes),
        phrases=len(chart.phrases),
        warnings=len(chart.warnings),
        lyricSource=chart.generator.lyricSource,
        separator=chart.generator.separator,
        durationSeconds=chart.song.durationSeconds,
    )
    if rep.json_mode:
        return

    click.echo(f"wrote {song.chart_json} ({len(chart.notes)} notes, "
               f"{len(chart.phrases)} phrases)")
    if song.chart_ultrastar_txt.exists():
        click.echo(f"wrote {song.chart_ultrastar_txt}")
    if chart.warnings:
        click.echo(click.style(f"{len(chart.warnings)} warning(s) - see "
                               f"'vocalchart inspect {song.root}'", fg="yellow"))


def _separation_options(func):
    """--no-separate / --separator-model / --separator-python, shared by the
    commands that can run stage 0."""
    func = click.option("--separator-python", default=None,
                        help="python.exe of the venv holding audio-separator. "
                             "Defaults to ./.venv-separate, or "
                             "$VOCALCHART_SEPARATOR_PYTHON.")(func)
    func = click.option("--separator-model", default=None,
                        help="audio-separator checkpoint. Defaults to "
                             "BS-Roformer ep_317 (~12.9 dB vocal SDR).")(func)
    func = click.option("--no-separate", is_flag=True, default=False,
                        help="Never run stage 0, even with no vocal stem present. "
                             "Charts straight off the full mix - worse, but instant.")(func)
    return func


def _lyric_options(func):
    func = click.option("--model", default="medium", show_default=True,
                        help="Whisper model for lyrics: tiny/base/small/medium/large-v3.")(func)
    func = click.option("--no-lyrics", is_flag=True, default=False,
                        help="Skip stage 3 entirely (faster; charts get syllable: null).")(func)
    return func


@main.command("build")
@_song_arg
@_flags
@click.option("--phrase-gap", default=DEFAULT_PHRASE_GAP, show_default=True,
              help="Silence in seconds that starts a new phrase (provisional, "
                   "until forced alignment lands).")
@click.option("--no-ultrastar", is_flag=True, default=False,
              help="Skip the lossy chart.ultrastar.txt sidecar.")
@_lyric_options
@_separation_options
@click.option("--progress-json", is_flag=True, default=False,
              help="Emit machine-readable progress on stdout (used by the plugin).")
@click.option("--cancel-file", default=None, type=click.Path(path_type=Path),
              help="Stop cleanly as soon as this file exists. The plugin uses it to "
                   "cancel: killing this process from outside would orphan the "
                   "separator running in its own venv.")
@click.pass_context
def cmd_build(ctx: click.Context, song_dir: str, force: bool, verbose: bool,
              phrase_gap: float, no_ultrastar: bool, no_lyrics: bool,
              model: str, no_separate: bool, separator_model: Optional[str],
              separator_python: Optional[str], progress_json: bool,
              cancel_file: Optional[Path]) -> None:
    """Full pipeline - writes chart.json and chart.ultrastar.txt.

    Runs stage 0 (separation) automatically when the folder has a source
    recording but no vocal stem. Folders built by `import-stems` already have a
    real stem, so stage 0 costs them nothing.
    """
    force, verbose = _merged(ctx, force, verbose)
    song = _resolve_song(song_dir)

    stages = ["f0", "notes", "chart"]
    separate = not no_separate and (song.source() is not None)
    if separate:
        stages.insert(0, "separate")
    if not no_lyrics:
        stages.insert(-1, "lyrics")

    rep = Reporter(stages, json_mode=progress_json, verbose=verbose)
    stop = _cancel_check(cancel_file)
    chart = _guard(rep, lambda: _pipeline(
        song, rep, force=force, phrase_gap=phrase_gap, no_ultrastar=no_ultrastar,
        no_lyrics=no_lyrics, model=model, separate=separate,
        separator_model=separator_model, separator_python=separator_python,
        cancel=stop))
    _report_chart(song, chart, rep)


# ------------------------------------------------------------------- inspect

@main.command("inspect")
@_song_arg
@click.option("--width", default=96, show_default=True, help="Sparkline width in columns.")
@click.option("--ascii", "ascii_only", is_flag=True, default=False,
              help="Force a pure-ASCII sparkline (auto-detected from the console otherwise).")
@click.pass_context
def cmd_inspect(ctx: click.Context, song_dir: str, width: int, ascii_only: bool) -> None:
    """Print a human summary of a built chart."""
    song = _resolve_song(song_dir)
    if not song.chart_json.exists():
        raise click.ClickException(
            f"no chart at {song.chart_json} - run 'vocalchart build {song.root}' first."
        )
    chart = Chart.read_json(song.chart_json)

    click.echo(click.style(f"{chart.song.id}", bold=True)
               + (f" - {chart.song.title}" if chart.song.title else "")
               + (f" - {chart.song.artist}" if chart.song.artist else ""))
    click.echo(f"  schema        {chart.schemaVersion}   "
               f"generated {chart.generator.generatedAt or 'unknown'}")
    click.echo(f"  reference     {chart.song.referenceAudio}")
    click.echo(f"  duration      {_fmt_time(chart.song.durationSeconds)} "
               f"({chart.song.durationSeconds:.3f} s)")
    click.echo(f"  tuning        A4 = {chart.song.tuningHz:.2f} Hz "
               f"({1200.0 * _log2(chart.song.tuningHz / 440.0):+.1f} cents from 440)")

    notes = chart.notes
    click.echo(f"  notes         {len(notes)} in {len(chart.phrases)} phrase(s)")
    if notes:
        lo = min(n.midi for n in notes)
        hi = max(n.midi for n in notes)
        sung = sum(n.durationSeconds for n in notes)
        durations = sorted(n.durationSeconds for n in notes)
        median = durations[len(durations) // 2]
        click.echo(f"  range         {_midi_name(lo)}..{_midi_name(hi)} "
                   f"(MIDI {lo}..{hi}, {hi - lo} semitones)")
        share = (f" ({sung / chart.song.durationSeconds * 100:.0f}% of the song)"
                 if chart.song.durationSeconds > 0 else "")
        click.echo(f"  sung          {_fmt_time(sung)}{share}")
        click.echo(f"  note length   median {median * 1000:.0f} ms, "
                   f"shortest {durations[0] * 1000:.0f} ms, "
                   f"longest {durations[-1] * 1000:.0f} ms")
        click.echo(f"  confidence    mean {chart.mean_confidence():.3f}")
        with_syllable = sum(1 for n in notes if n.syllable)
        click.echo(f"  syllables     {with_syllable}/{len(notes)} notes carry text "
                   f"(null is legal and expected)")

    if chart.contour is not None and len(chart.contour) > 0:
        click.echo("")
        block = _sparkline_block(chart.contour, width=max(16, width), ascii_only=ascii_only)
        try:
            click.echo(block)
        except UnicodeEncodeError:
            # Belt and braces: some Windows consoles misreport their encoding.
            click.echo(_sparkline_block(chart.contour, width=max(16, width), ascii_only=True))

    click.echo("")
    if chart.warnings:
        click.echo(click.style(f"  {len(chart.warnings)} warning(s):", fg="yellow"))
        for w in chart.warnings:
            span = ""
            if w.startSeconds is not None and w.endSeconds is not None:
                span = f" [{_fmt_time(w.startSeconds)}..{_fmt_time(w.endSeconds)}]"
            click.echo(f"    {w.code}{span}  {w.message}")
    else:
        click.echo("  no warnings")

    try:
        chart.validate()
    except ChartValidationError as exc:
        click.echo(click.style(f"  INVALID: {exc}", fg="red"), err=True)
        ctx.exit(1)


def _log2(x: float) -> float:
    import math

    return math.log2(x) if x > 0 else 0.0


def _sparkline_block(contour: Contour, width: int, ascii_only: bool = False) -> str:
    """Pitch sparkline over the whole contour.

    Unicode blocks where the console can take them, an ASCII ramp otherwise -
    the default Windows console is cp1252 and would raise on U+2581.
    """
    ramp_unicode = "▁▂▃▄▅▆▇█"
    ramp_ascii = "_.-~=+*#"
    values = contour.fracMidi
    if not values:
        return ""

    buckets: list[Optional[float]] = []
    n = len(values)
    for col in range(width):
        lo = col * n // width
        hi = max(lo + 1, (col + 1) * n // width)
        window = [v for v in values[lo:hi] if v is not None]
        if not window:
            buckets.append(None)
        else:
            window.sort()
            buckets.append(window[len(window) // 2])

    voiced = [v for v in buckets if v is not None]
    if not voiced:
        return "  (no voiced frames)"
    vmin, vmax = min(voiced), max(voiced)
    span = max(1e-6, vmax - vmin)

    def render(ramp: str, blank: str) -> str:
        chars = []
        for v in buckets:
            if v is None:
                chars.append(blank)
            else:
                level = int((v - vmin) / span * (len(ramp) - 1) + 0.5)
                chars.append(ramp[level])
        return "".join(chars)

    line = render(ramp_ascii, " ") if ascii_only else render(ramp_unicode, " ")
    if not ascii_only:
        try:
            line.encode(sys.stdout.encoding or "utf-8")
        except (UnicodeEncodeError, LookupError, AttributeError):
            line = render(ramp_ascii, " ")

    header = (f"  pitch  {_midi_name(round(vmax))} ({vmax:.1f}) top, "
              f"{_midi_name(round(vmin))} ({vmin:.1f}) bottom, "
              f"blank = unvoiced")
    ruler = f"  0:00{' ' * max(0, width - 10)}{_fmt_time(contour.durationSeconds)[:5]}"
    return f"{header}\n  {line}\n{ruler}"


# ------------------------------------------------- later milestones, declared

@main.command("separate")
@_song_arg
@_flags
@_separation_options
@click.option("--no-backing", is_flag=True, default=False,
              help="Discard the instrumental instead of writing backing.wav.")
@click.option("--keep-intermediates", is_flag=True, default=False,
              help="Leave the raw separator output in cache/separate/ for inspection.")
@click.pass_context
def cmd_separate(ctx: click.Context, song_dir: str, force: bool, verbose: bool,
                 no_separate: bool, separator_model: Optional[str],
                 separator_python: Optional[str], no_backing: bool,
                 keep_intermediates: bool) -> None:
    """Stage 0 - split source.* into stems/vocals.wav + backing.wav.

    Uses BS-Roformer ep_317 through audio-separator, the same model family
    Ultimate Vocal Remover 5 runs, driven headlessly from an isolated venv.

    A REAL stem always beats a separated one: DESIGN.md section 4 measures
    14.19% downstream word error on a genuine isolated stem against 21.08% on a
    separated one. So this refuses to overwrite a stem it did not produce
    itself - pass --force if you really mean it.
    """
    from .separate import DEFAULT_MODEL, SeparationError, separate_song

    force, verbose = _merged(ctx, force, verbose)
    song = _resolve_song(song_dir)
    song.ensure_dirs()

    rep = Reporter(["separate"], json_mode=False, verbose=verbose or True)

    try:
        result = separate_song(
            song,
            force=force,
            model=separator_model or DEFAULT_MODEL,
            python=separator_python,
            write_backing=not no_backing,
            keep_intermediates=keep_intermediates,
            progress=rep.say,
            on_percent=lambda pct, msg: rep.percent(pct, msg),
        )
    except SeparationError as exc:
        raise click.ClickException(str(exc)) from exc

    if result.skipped:
        click.echo(click.style(f"skipped: {result.reason}", fg="yellow"))
        return

    click.echo(f"separated {song.root} with {result.model} on {result.device}")
    click.echo(f"  stems/vocals.wav  (charting source)")
    if result.instrumental is not None:
        click.echo(f"  backing.wav       (instrumental)")
    click.echo(f"\nnext:  vocalchart build {song.root}")


def _run_lyrics(
    song: SongPaths,
    force: bool,
    say: Callable[[str], None],
    model: str = "medium",
) -> tuple[list, str]:
    """Stage 3. Returns (words, lyricSource). Never raises — a chart without
    lyrics is still a perfectly good chart, so a transcription failure degrades
    rather than aborting the build."""
    from .paths import is_cache_fresh

    try:
        from .lyrics import (Word, align_to_known_lyrics, read_lyrics_file,
                             transcribe_words)
    except ImportError as exc:
        say(f"lyrics unavailable ({exc}); charting without text")
        return [], "none"

    if not force and is_cache_fresh(song.words_json, song.analysis_input(), song.lyrics_txt):
        try:
            payload = json.loads(song.words_json.read_text(encoding="utf-8"))
            words = [Word(**w) for w in payload.get("words", [])]
            say(f"words cache hit: {len(words)} words")
            return words, payload.get("lyricSource", "asr")
        except Exception as exc:                      # noqa: BLE001
            say(f"words cache unreadable ({exc}); re-transcribing")

    try:
        words = transcribe_words(song.analysis_input(), model_size=model, progress=say)
    except Exception as exc:                          # noqa: BLE001
        say(f"WARNING: transcription failed ({type(exc).__name__}: {exc}); "
            f"charting without lyrics")
        return [], "none"

    if not words:
        return [], "none"

    source = "asr"
    known = read_lyrics_file(song.lyrics_txt)

    if known:
        # ASR is good at WHEN and bad at WHAT on sung audio; lyrics.txt is the
        # reverse. Aligning the two gets both without a forced aligner.
        words = align_to_known_lyrics(words, known, progress=say)
        source = "forced-alignment"
    else:
        say("no lyrics.txt — words come from ASR and WILL contain errors "
            "(~30% word error on sung vocals). Drop a lyrics.txt in the song "
            "folder and rebuild to fix the words while keeping the timing.")

    song.words_json.parent.mkdir(parents=True, exist_ok=True)
    song.words_json.write_text(
        json.dumps({"lyricSource": source,
                    "model": model,
                    "words": [w.__dict__ for w in words]},
                   indent=2, ensure_ascii=False) + "\n",
        encoding="utf-8", newline="\n")

    return words, source


@main.command("lyrics")
@_song_arg
@_flags
@click.option("--model", default="medium", show_default=True,
              help="Whisper model size: tiny/base/small/medium/large-v3. "
                   "Bigger is more accurate and slower.")
def cmd_lyrics(song_dir: str, force: bool, verbose: bool, model: str) -> None:
    """Stage 3 - word-level lyric timing via faster-whisper (no torch needed)."""
    song = _resolve_song(song_dir)
    say = _progress(verbose or True)

    words, source = _run_lyrics(song, force, say, model)

    if not words:
        raise click.ClickException("no words produced — see the warnings above")

    click.echo(f"{len(words)} words ({source}) -> {song.words_json}")
    click.echo("  " + " ".join(w.text for w in words[:16]) + " ...")


# --------------------------------------------------------------------------- #
# add-song - one file in, a playable song folder out
# --------------------------------------------------------------------------- #

# Containers ffmpeg has to demux before anything else can read them. Users drag
# whatever they have at a "drop an audio file here" target, and a rip sitting in
# an .mp4 is not an error worth making them fix by hand.
VIDEO_EXTENSIONS = (".mp4", ".m4v", ".mkv", ".webm", ".mov", ".avi", ".wmv", ".flv")


def _slugify(text: str) -> str:
    """'Don'\''t Stop Believin'\'' (Remaster).mp3' -> 'dont-stop-believin-remaster'.

    Apostrophes are DELETED rather than turned into separators, so the folder
    names match the ones already in songs/ (dont-stop-believing, i-cant-help-
    myself). Everything else non-alphanumeric becomes a single hyphen.
    """
    import re
    import unicodedata

    text = unicodedata.normalize("NFKD", text)
    text = "".join(c for c in text if not unicodedata.combining(c))
    text = text.replace("'", "").replace("\u2019", "")
    text = re.sub(r"[^a-zA-Z0-9]+", "-", text).strip("-").lower()
    return text or "untitled"


def _clean_stem(name: str) -> str:
    """Strip a leading track number off a file stem: '03 - Mr Brightside'."""
    import re

    return re.sub(r"^\s*\d{1,3}\s*[-._)]\s*", "", name).strip()


def _guess_title_artist(path: Path) -> tuple[str, Optional[str]]:
    """(title, artist) from the file name. 'Artist - Title.mp3' is the one
    convention common enough to be worth honouring; anything else is a title."""
    stem = _clean_stem(path.stem)
    if " - " in stem:
        left, _, right = stem.partition(" - ")
        if left.strip() and right.strip():
            return right.strip(), left.strip()
    return stem, None


def _default_library() -> Path:
    """songs/ next to the repo when it exists, else the plugin's library root.

    The 21 songs already imported live in the repo, and a new one landing
    somewhere else entirely would be a surprise.
    """
    repo_songs = Path(__file__).resolve().parents[2] / "songs"
    if repo_songs.is_dir():
        return repo_songs
    from .paths import default_library_root

    return default_library_root()


def _ffmpeg_extract(src: Path, dest: Path, say: Callable[[str], None]) -> None:
    """Demux/transcode `src` to a 44.1 kHz stereo WAV at `dest`.

    Stereo on purpose: BS-Roformer separates a stereo mix appreciably better
    than a mono fold-down, and the downmix to mono happens later anyway.
    """
    import shutil

    exe = shutil.which("ffmpeg")
    if exe is None:
        raise click.ClickException(
            f"{src.suffix} needs ffmpeg to extract the audio, and ffmpeg is not on PATH.\n"
            f"    winget install --id Gyan.FFmpeg -e")

    say(f"extracting audio from {src.name} with ffmpeg")
    cmd = [exe, "-y", "-loglevel", "error", "-i", str(src),
           "-vn", "-acodec", "pcm_s16le", "-ar", "44100", "-ac", "2", str(dest)]
    proc = subprocess.run(cmd, capture_output=True, text=True)
    if proc.returncode != 0 or not dest.exists():
        raise click.ClickException(
            f"ffmpeg could not read {src.name} (exit {proc.returncode}):\n"
            f"{proc.stderr.strip()[:800]}")


def _ingest_audio(audio: Path, song: SongPaths, force: bool,
                  say: Callable[[str], None]) -> Path:
    """Put `audio` in the song folder as source.*, and return that path."""
    suffix = audio.suffix.lower()
    existing = song.source()

    if suffix in VIDEO_EXTENSIONS:
        dest = song.root / "source.wav"
    elif suffix in AUDIO_EXTENSIONS:
        dest = song.root / f"source{suffix}"
    else:
        say(f"unrecognised extension '{suffix}'; letting ffmpeg try it")
        dest = song.root / "source.wav"

    if existing is not None and not force:
        # Re-running add-song on the same file must be free, not an error: it is
        # the natural way to resume after a crash or to pick up a new stage.
        if sha1_of_file(existing) == sha1_of_file(audio):
            say(f"source already present and identical: {existing.name}")
            return existing

        # Different audio, same slug. Overwriting would leave the caches, the
        # stems and the chart describing a recording that is no longer there.
        raise click.ClickException(
            f"{song.root} already holds a different recording ({existing.name}).\n"
            f"Use --force to replace it, pass a SONG_DIR to build a separate folder, "
            f"or --id to name a new one.")

    song.root.mkdir(parents=True, exist_ok=True)

    if existing is not None and existing != dest:
        say(f"replacing {existing.name}")
        existing.unlink()

    if dest.suffix == ".wav" and suffix not in (".wav",):
        _ffmpeg_extract(audio, dest, say)
    else:
        import shutil

        say(f"copying {audio.name} -> {dest.name}")
        shutil.copy2(audio, dest)

    return dest


@main.command("add-song")
@click.argument("audio_file", type=click.Path(exists=True, dir_okay=False, path_type=Path))
@click.argument("song_dir", required=False,
                type=click.Path(file_okay=False, path_type=Path))
@_flags
@click.option("--id", "song_id", default=None,
              help="Folder name under the library. Default: a slug of the file name.")
@click.option("--library", default=None, type=click.Path(file_okay=False, path_type=Path),
              help="Library root the song folder is created in. Default: ./songs.")
@click.option("--lyrics", "lyrics_file", default=None,
              type=click.Path(exists=True, dir_okay=False, path_type=Path),
              help="Plain-text lyrics. Turns stage 3 from ~30%% word error ASR into "
                   "a pure timing problem - STRONGLY recommended when you have them.")
@click.option("--title", default=None, help="Overrides the title guessed from the file name.")
@click.option("--artist", default=None, help="Overrides the artist guessed from the file name.")
@click.option("--phrase-gap", default=DEFAULT_PHRASE_GAP, show_default=True,
              help="Silence in seconds that starts a new phrase.")
@click.option("--no-ultrastar", is_flag=True, default=False,
              help="Skip the lossy chart.ultrastar.txt sidecar.")
@_lyric_options
@_separation_options
@click.option("--progress-json", is_flag=True, default=False,
              help="Emit machine-readable progress on stdout (used by the plugin).")
@click.option("--cancel-file", default=None, type=click.Path(path_type=Path),
              help="Stop cleanly as soon as this file exists. The plugin uses it to "
                   "cancel: killing this process from outside would orphan the "
                   "separator running in its own venv.")
@click.pass_context
def cmd_add_song(ctx: click.Context, audio_file: Path, song_dir: Optional[Path],
                 force: bool, verbose: bool, song_id: Optional[str],
                 library: Optional[Path], lyrics_file: Optional[Path],
                 title: Optional[str], artist: Optional[str], phrase_gap: float,
                 no_ultrastar: bool, no_lyrics: bool, model: str, no_separate: bool,
                 separator_model: Optional[str], separator_python: Optional[str],
                 progress_json: bool, cancel_file: Optional[Path]) -> None:
    """One audio file in, a playable song folder out.

        vocalchart add-song "D:\\music\\Mr Brightside.mp3"

    Creates songs/<slug>/, separates the recording into a vocal stem and an
    instrumental, tracks pitch, segments notes, transcribes lyrics and writes
    chart.json - everything the plugin's Load chart... needs.

    Video containers (.mp4, .mkv, ...) are demuxed with ffmpeg first, so a rip
    can be dropped in directly.

    If you have the lyrics as text, pass --lyrics. It is the single largest
    quality difference available: known text turns a 20-38%% word error
    transcription problem into a pure timing problem.
    """
    force, verbose = _merged(ctx, force, verbose)

    guessed_title, guessed_artist = _guess_title_artist(audio_file)
    # Slug from the title alone, not "artist-title": the folders already in
    # songs/ are named for the song (mr-brightside, eye-of-the-tiger).
    slug = song_id or _slugify(guessed_title)
    root = Path(song_dir) if song_dir else (
        (Path(library) if library else _default_library()) / slug)
    song = SongPaths(root)

    stages = ["ingest", "f0", "notes", "chart"]
    if not no_separate:
        stages.insert(1, "separate")
    if not no_lyrics:
        stages.insert(-1, "lyrics")

    rep = Reporter(stages, json_mode=progress_json, verbose=verbose or not progress_json)
    stop = _cancel_check(cancel_file)

    def run() -> Chart:
        rep.stage("ingest", f"preparing {song.root.name}")
        song.ensure_dirs()
        _ingest_audio(audio_file, song, force, rep.say)

        if lyrics_file is not None:
            import shutil

            shutil.copy2(lyrics_file, song.lyrics_txt)
            rep.log(f"copied lyrics -> {song.lyrics_txt.name}")
        elif not song.lyrics_txt.exists() and not no_lyrics:
            rep.log("no lyrics.txt: words come from ASR and WILL contain errors "
                    "(~30% on sung vocals). Re-run with --lyrics to fix the words "
                    "while keeping the timing.")

        # Stage 0 is wanted here even when a stem exists from an earlier run -
        # separate_song() decides for itself whether that stem is one of ours.
        return _pipeline(song, rep, force=force, phrase_gap=phrase_gap,
                         no_ultrastar=no_ultrastar, no_lyrics=no_lyrics, model=model,
                         separate=not no_separate, separator_model=separator_model,
                         separator_python=separator_python,
                         title=title or guessed_title,
                         artist=artist or guessed_artist,
                         cancel=stop)

    chart = _guard(rep, run)

    _report_chart(song, chart, rep)

    if not progress_json:
        click.echo(f"\nsong folder: {song.root}")
        click.echo(f"load this in the plugin:  {song.chart_json}")


@main.command("import-stems")
@click.argument("stem_dir", type=click.Path(exists=True, file_okay=False, path_type=Path))
@click.argument("song_dir", type=click.Path(file_okay=False, path_type=Path))
@click.option("--vocal-match", default="lead_vocal", show_default=True,
              help="Case-insensitive substring identifying the LEAD VOCAL stem.")
@click.option("--exclude", multiple=True, default=("click",), show_default=True,
              help="Substrings of stems to leave OUT of the backing mix. Repeatable.")
@click.option("--keep-click", is_flag=True, default=False,
              help="Keep the click track in the backing mix.")
@click.pass_context
def import_stems(ctx, stem_dir: Path, song_dir: Path, vocal_match: str,
                 exclude: tuple, keep_click: bool) -> None:
    """Build a song folder from a multitrack stem directory.

    Takes a folder of per-instrument stems (the shape sold by backing-track
    services: Bass, Drum_Kit, Lead_Vocal, Click, ...) and produces

        SONG_DIR/stems/vocals.wav   the lead vocal, for charting
        SONG_DIR/backing.wav        EVERYTHING ELSE, mixed, for singing over

    The lead vocal is deliberately EXCLUDED from the backing mix. A backing
    track containing the tune teaches you to follow it, not to pitch it - and
    it would also mean the mic hears the reference vocal, so the tracker could
    lock onto the record instead of you.
    """
    import numpy as np
    import soundfile as sf

    _, verbose = _merged(ctx, False, False)
    say = _progress(verbose)

    audio_files = sorted(p for p in stem_dir.iterdir()
                         if p.suffix.lower() in (".wav", ".mp3", ".flac", ".aiff", ".aif", ".ogg"))

    if not audio_files:
        raise click.ClickException(f"No audio files in {stem_dir}")

    lead = [p for p in audio_files if vocal_match.lower() in p.name.lower()]

    if not lead:
        raise click.ClickException(
            f"No stem matching '{vocal_match}' in {stem_dir}.\n"
            f"Found: " + ", ".join(p.name for p in audio_files[:8]))

    # PICKING THE RIGHT ONE MATTERS, AND ALPHABETICAL ORDER GETS IT WRONG.
    #
    # Journey's "Any Way You Want It" ships BOTH `Lead_Vocal` and
    # `Lead_Vocal_(ad_lib)`. '(' sorts before '_', so plain alphabetical order
    # picks the AD-LIB — a stem that is silent for most of the song. The chart
    # that came out had 13% voiced audio, 66 notes for 3.5 minutes, and Whisper
    # hallucinating "you you you you you" over the silence.
    #
    # Rank by qualifier penalty first, then by name length: the main vocal is
    # the one with the FEWEST extra words in its name.
    _QUALIFIERS = ("ad_lib", "ad lib", "adlib", "backing", "harmony", "harmonies",
                   "double", "octave", "spoken", "whisper", "stack", "alt")

    def _stem_penalty(path: Path):
        low = path.name.lower()
        return (sum(1 for q in _QUALIFIERS if q in low), len(path.name))

    lead.sort(key=_stem_penalty)

    # EQUAL PENALTY MEANS CO-LEAD SINGERS, NOT A MAIN AND AN AD-LIB.
    #
    #   Lead_Vocal            vs  Lead_Vocal_(ad_lib)     -> penalties DIFFER,
    #                                                        pick the main one.
    #   Lead_Vocal_Estelle    vs  Lead_Vocal_Kanye        -> penalties EQUAL,
    #                                                        it is a duet.
    #
    # Charting one singer of a duet silently drops half the song: I Gotta
    # Feeling charted from Fergie's stem alone gave 132 notes for 4.9 minutes.
    # The singer wants to sing the whole lead line, so merge co-leads. They
    # mostly alternate rather than overlap, which is what monophonic pitch
    # tracking needs.
    base_penalty = _stem_penalty(lead[0])[0]
    co_leads = [p for p in lead if _stem_penalty(p)[0] == base_penalty]

    if len(lead) > 1:
        say(f"{len(lead)} stems match '{vocal_match}'")

        for other in lead:
            if other not in co_leads:
                say(f"    ignored (qualifier): {other.name}")

    if len(co_leads) > 1:
        say(f"    MERGING {len(co_leads)} co-lead vocals (duet):")
        for p in co_leads:
            say(f"      + {p.name}")
    else:
        say(f"    chose {lead[0].name}")

    drops = tuple(e.lower() for e in exclude) if not keep_click else ()
    backing_files = [p for p in audio_files
                     if p not in co_leads
                     and vocal_match.lower() not in p.name.lower()
                     and not any(d in p.name.lower() for d in drops)]

    song_dir.mkdir(parents=True, exist_ok=True)
    (song_dir / "stems").mkdir(exist_ok=True)

    # ---- lead vocal -> stems/vocals.wav
    vocal, sr = _read_mono(co_leads[0])

    for extra in co_leads[1:]:
        other, _ = _read_mono(extra, target_sr=sr)

        if other.size > vocal.size:
            vocal = np.pad(vocal, (0, other.size - vocal.size))

        vocal[:other.size] += other

    # Co-lead stems rarely overlap, so summing keeps headroom; normalise only if
    # the sum actually clipped.
    peak = float(np.abs(vocal).max())

    if peak > 1.0:
        vocal /= peak

    sf.write(song_dir / "stems" / "vocals.wav", vocal, sr)

    # A "lead vocal" that is mostly silence is almost always the WRONG STEM.
    # Cheap to detect, and it stops a whole song being charted from a file that
    # barely contains any singing.
    frame = 1024

    if vocal.size > frame:
        blocks = vocal[: (vocal.size // frame) * frame].reshape(-1, frame)
        loud = float(np.mean(np.sqrt((blocks ** 2).mean(axis=1)) > 0.005))

        if loud < 0.25:
            click.secho(
                f"  WARNING: only {loud * 100:.0f}% of '{lead[0].name}' carries signal. "
                f"That is usually an ad-lib or doubling stem, not the main vocal. "
                f"Check the stem list and re-run with --vocal-match if so.",
                fg="yellow", err=True)

    # ---- everything else -> backing.wav
    if not backing_files:
        say("WARNING: no other stems found; no backing track written "
            "(silent practice is still legal)")
        click.echo(f"imported {song_dir} (vocal only, {len(vocal)/sr:.1f}s)")
        return

    say(f"mixing {len(backing_files)} stems into the backing track")
    mix = None

    for p in backing_files:
        say(f"  + {p.name}")
        a, a_sr = _read_mono(p, target_sr=sr)

        if mix is None:
            mix = np.zeros(max(len(a), len(vocal)), dtype=np.float64)

        if len(a) > len(mix):
            mix = np.pad(mix, (0, len(a) - len(mix)))

        mix[:len(a)] += a

    # Headroom, not normalisation to full scale: the backing sits UNDER a live
    # voice, and a mix slammed to 0 dBFS leaves the singer nothing to sit on.
    peak = float(np.abs(mix).max())

    if peak > 1e-9:
        mix *= 0.7 / peak

    sf.write(song_dir / "backing.wav", mix.astype(np.float32), sr)

    click.echo(f"imported {song_dir}")
    click.echo(f"  stems/vocals.wav  {len(vocal)/sr:6.1f}s   (charting source)")
    click.echo(f"  backing.wav       {len(mix)/sr:6.1f}s   ({len(backing_files)} stems, "
               f"lead vocal excluded)")
    click.echo(f"\nnext:  vocalchart build {song_dir}")


def _read_mono(path: Path, target_sr: Optional[int] = None):
    """Decode any supported file to mono float64, optionally resampled."""
    import librosa
    import numpy as np

    audio, sr = librosa.load(str(path), sr=target_sr, mono=True)
    return audio.astype(np.float64), int(sr)


if __name__ == "__main__":  # pragma: no cover
    main()
