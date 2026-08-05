"""Stage 0 - source separation (DESIGN.md section 4).

Splits an arbitrary mixed recording into a vocal stem for charting and an
instrumental for singing over. Same job Ultimate Vocal Remover 5 does through
its GUI; this drives the same class of model headlessly so a whole song folder
can be produced by one command.

    source.mp3  ->  stems/vocals.wav   (charting source)
                ->  backing.wav        (what you sing over)

**Model: BS-Roformer `ep_317_sdr_12.9755`.** ~12.9 dB vocal SDR against
htdemucs_ft's ~8.9-9.2 and Spleeter's ~5.9. It is also the model UVR5 users
reach for, so the result should sound familiar.

**Separation is the fallback, not the goal.** DESIGN.md section 4 measures
14.19% word error downstream on a genuine isolated stem versus 21.08% on a
separated one - separation artifacts eat essentially all of the separator's
benefit. So this module never overwrites a stem it did not produce itself
(see `_provenance`), and `import-stems` remains the better path when real
multitracks exist.

### Why a second interpreter

`audio-separator` requires `torch >= 2.3`; the analysis venv is deliberately
ONNX-only and pyproject.toml says so in as many words. Worse, DESIGN.md:375
records that installing torch *after* faster-whisper is what produces the
"Could not locate cudnn_ops64_9.dll" failure - and faster-whisper is already
installed there. So torch lives in its own venv and is reached by subprocess.
Nothing in this module imports torch.
"""

from __future__ import annotations

import json
import os
import subprocess
import sys
import threading
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path
from typing import Callable, Iterable, Optional

from .paths import SongPaths, sha1_of_file

__all__ = [
    "DEFAULT_MODEL",
    "SEPARATOR_ID",
    "SeparationError",
    "SeparationResult",
    "default_model_dir",
    "find_separator_python",
    "separator_available",
    "separate_song",
]

# The checkpoint named in DESIGN.md section 4. audio-separator resolves it from
# its own model registry and downloads on first use.
DEFAULT_MODEL = "model_bs_roformer_ep_317_sdr_12.9755.ckpt"

# Goes into chart.generator.separator. Keep in sync with the comment on
# Generator.separator in chart.py.
SEPARATOR_ID = "bs-roformer ep_317"

# Peak the instrumental is scaled to. Matches `import-stems`: the backing sits
# UNDER a live voice, so a mix slammed to 0 dBFS leaves the singer nothing to
# sit on. Two import paths must not produce wildly different monitoring levels.
BACKING_PEAK = 0.7

_ENV_PYTHON = "VOCALCHART_SEPARATOR_PYTHON"
_ENV_MODEL_DIR = "VOCALCHART_SEPARATOR_MODELS"

_PROVENANCE_NAME = ".separated.json"

Progress = Callable[[str], None]
PercentSink = Callable[[float, str], None]
CancelCheck = Callable[[], bool]


class SeparationError(RuntimeError):
    """Stage 0 failed, or its environment is missing."""


class SeparationCancelled(SeparationError):
    """The caller asked for the job to stop."""


@dataclass
class SeparationResult:
    """What stage 0 left on disk."""

    vocals: Path
    instrumental: Optional[Path]
    model: str
    separator_id: str = SEPARATOR_ID
    device: str = "unknown"
    skipped: bool = False
    reason: str = ""

    @property
    def ran(self) -> bool:
        return not self.skipped


# --------------------------------------------------------------------------- #
# locating the isolated environment
# --------------------------------------------------------------------------- #

def _repo_root() -> Path:
    """vocal-trainer/ - two levels above python/vocalchart/."""
    return Path(__file__).resolve().parents[2]


def _venv_python(venv: Path) -> Path:
    return venv / ("Scripts/python.exe" if os.name == "nt" else "bin/python")


def _candidate_pythons() -> Iterable[Path]:
    override = os.environ.get(_ENV_PYTHON)
    if override:
        yield Path(override)
    for base in (_repo_root(), Path.cwd(), Path.cwd().parent):
        yield _venv_python(base / ".venv-separate")


def find_separator_python(explicit: Optional[str | Path] = None) -> Path:
    """The interpreter that has audio-separator, or a SeparationError saying how
    to make one.

    Order: explicit argument, $VOCALCHART_SEPARATOR_PYTHON, ./.venv-separate,
    then this interpreter if audio-separator happens to be installed in it
    (true for a single-venv setup, which is supported but not recommended).
    """
    if explicit:
        p = Path(explicit)
        if not p.exists():
            raise SeparationError(f"separator interpreter not found: {p}")
        return p

    for candidate in _candidate_pythons():
        if candidate.exists():
            return candidate

    # Single-venv fallback: maybe torch lives right here.
    try:
        import audio_separator  # type: ignore  # noqa: F401
    except Exception:                                  # noqa: BLE001
        pass
    else:
        return Path(sys.executable)

    raise SeparationError(
        "no separation environment found.\n"
        "\n"
        "Stage 0 needs torch, which is deliberately kept OUT of the analysis venv\n"
        "(pyproject.toml, and DESIGN.md:375 on the cuDNN/faster-whisper ordering trap).\n"
        "Create the isolated one once:\n"
        "\n"
        f"    py -3.11 -m venv {_repo_root() / '.venv-separate'}\n"
        f"    {_venv_python(_repo_root() / '.venv-separate')} -m pip install "
        "torch==2.8.0 torchaudio==2.8.0 --index-url https://download.pytorch.org/whl/cu126\n"
        f"    {_venv_python(_repo_root() / '.venv-separate')} -m pip install "
        '"audio-separator[gpu]==0.44.5"\n'
        "\n"
        "No NVIDIA GPU? Use the /whl/cpu index and audio-separator[cpu] instead;\n"
        "separation then takes 20-40 min per song rather than 1-3.\n"
        f"Already have one elsewhere? Point {_ENV_PYTHON} at its python.exe."
    )


def separator_available(explicit: Optional[str | Path] = None) -> bool:
    """True when stage 0 could run. Never raises - callers use it to decide
    whether to offer separation, not to run it."""
    try:
        find_separator_python(explicit)
    except SeparationError:
        return False
    return True


def default_model_dir() -> Path:
    """Where checkpoints are cached. LOCALAPPDATA, not APPDATA: these are large
    (~600 MB for BS-Roformer) and must not follow a roaming profile."""
    override = os.environ.get(_ENV_MODEL_DIR)
    if override:
        return Path(override)
    local = os.environ.get("LOCALAPPDATA")
    if local:
        return Path(local) / "VocalTrainer" / "separator-models"
    xdg = os.environ.get("XDG_CACHE_HOME")
    base = Path(xdg) if xdg else Path.home() / ".cache"
    return base / "VocalTrainer" / "separator-models"


# --------------------------------------------------------------------------- #
# provenance - "did we make this stem, or did the user supply it?"
# --------------------------------------------------------------------------- #

def _provenance_path(song: SongPaths) -> Path:
    return song.stems_dir / _PROVENANCE_NAME


def _read_provenance(song: SongPaths) -> Optional[dict]:
    path = _provenance_path(song)
    if not path.exists():
        return None
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except Exception:                                  # noqa: BLE001
        return None


def _write_provenance(song: SongPaths, *, model: str, source_sha1: str,
                      device: str, wrote_backing: bool) -> None:
    """Marks stems/vocals.wav (and possibly backing.wav) as OURS.

    Without this the module cannot tell a separated stem from a real one the
    user placed there by hand, and would happily overwrite a studio multitrack
    with a lossy reconstruction of itself.
    """
    payload = {
        "tool": "vocalchart",
        "stage": "separate",
        "model": model,
        "separator": SEPARATOR_ID,
        "device": device,
        "sourceSha1": source_sha1,
        "wroteBacking": wrote_backing,
        "generatedAt": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
    }
    _provenance_path(song).write_text(
        json.dumps(payload, indent=2) + "\n", encoding="utf-8", newline="\n")


# --------------------------------------------------------------------------- #
# subprocess plumbing
# --------------------------------------------------------------------------- #

def _pump_json_lines(stream, on_event: Callable[[dict], None],
                     on_text: Callable[[str], None]) -> None:
    """Read the driver's stdout. One JSON object per line; anything else is
    surfaced as text rather than swallowed."""
    for raw in stream:
        line = raw.decode("utf-8", "replace").strip()
        if not line:
            continue
        try:
            event = json.loads(line)
        except json.JSONDecodeError:
            on_text(line)
            continue
        if isinstance(event, dict):
            on_event(event)
        else:
            on_text(line)


def _pump_progress(stream, on_line: Callable[[str], None]) -> None:
    """Read the driver's stderr, splitting on BOTH \\n and \\r.

    tqdm redraws its bar with a carriage return and no newline, so readline()
    would sit on a full progress bar for the entire separation and deliver it in
    one lump at the end. os.read returns as soon as any byte is available.
    """
    fd = stream.fileno()
    buffer = b""
    while True:
        try:
            chunk = os.read(fd, 4096)
        except OSError:
            break
        if not chunk:
            break
        buffer += chunk
        while True:
            cut = min((i for i in (buffer.find(b"\r"), buffer.find(b"\n")) if i >= 0),
                      default=-1)
            if cut < 0:
                break
            piece, buffer = buffer[:cut], buffer[cut + 1:]
            text = piece.decode("utf-8", "replace").strip()
            if text:
                on_line(text)
    tail = buffer.decode("utf-8", "replace").strip()
    if tail:
        on_line(tail)


def _percent_in(text: str) -> Optional[float]:
    """The tqdm percentage in a progress line, if there is one.

    Deliberately best-effort: losing the number costs a smooth progress bar and
    nothing else, so this must never raise or reject the line.
    """
    idx = text.find("%")
    if idx <= 0:
        return None
    start = idx
    while start > 0 and (text[start - 1].isdigit() or text[start - 1] == "."):
        start -= 1
    if start == idx:
        return None
    try:
        value = float(text[start:idx])
    except ValueError:
        return None
    return value if 0.0 <= value <= 100.0 else None


def _run_driver(python: Path, *, audio: Path, out_dir: Path, model: str,
                model_dir: Path, say: Progress,
                on_percent: Optional[PercentSink],
                cancel: Optional[CancelCheck] = None) -> dict:
    """Run _sep_driver.py under `python` and return its result event.

    Cancellation is handled HERE rather than by the caller killing us, because
    only this process holds a handle to the driver. Killing the CLI from
    outside would orphan a multi-minute GPU job with nothing left to stop it.
    """
    driver = Path(__file__).resolve().parent / "_sep_driver.py"
    if not driver.exists():                            # pragma: no cover
        raise SeparationError(f"driver missing: {driver}")

    # -P keeps the driver's own directory off sys.path; see the note at the top
    # of _sep_driver.py.
    cmd = [str(python), "-P", str(driver),
           "--input", str(audio),
           "--output-dir", str(out_dir),
           "--model", model,
           "--model-dir", str(model_dir)]

    env = dict(os.environ)
    env["PYTHONUNBUFFERED"] = "1"

    kwargs = {}
    if os.name == "nt":
        # The plugin spawns this chain; a console window flashing up mid-song
        # would be a bug report.
        kwargs["creationflags"] = getattr(subprocess, "CREATE_NO_WINDOW", 0)

    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                            env=env, **kwargs)

    result: dict = {}
    failure: dict = {}
    last_status = "separating"

    def on_event(event: dict) -> None:
        nonlocal result, failure, last_status
        kind = event.get("event")
        if kind == "result":
            result = event
        elif kind == "error":
            failure = event
        elif kind == "status":
            last_status = str(event.get("message", "")) or last_status
            say(last_status)

    def on_stderr(line: str) -> None:
        pct = _percent_in(line)
        if pct is not None and on_percent is not None:
            on_percent(pct, last_status)

    threads = [
        threading.Thread(target=_pump_json_lines, args=(proc.stdout, on_event, say),
                         daemon=True),
        threading.Thread(target=_pump_progress, args=(proc.stderr, on_stderr),
                         daemon=True),
    ]
    for t in threads:
        t.start()

    cancelled = False
    while True:
        try:
            code = proc.wait(timeout=0.25)
            break
        except subprocess.TimeoutExpired:
            if cancel is not None and cancel():
                cancelled = True
                proc.kill()
                code = proc.wait()
                break

    for t in threads:
        t.join(timeout=5.0)

    if cancelled:
        raise SeparationCancelled("separation cancelled")
    if failure:
        detail = failure.get("traceback") or ""
        raise SeparationError(
            f"separation failed: {failure.get('message', 'unknown error')}"
            + (f"\n\n{detail}" if detail else ""))
    if code != 0:
        raise SeparationError(
            f"separation driver exited {code} with no error event.\n"
            f"Command: {' '.join(cmd)}")
    if not result:
        raise SeparationError("separation driver produced no result event")
    return result


# --------------------------------------------------------------------------- #
# output identification
# --------------------------------------------------------------------------- #

def _classify(outputs: list[str]) -> tuple[Optional[Path], Optional[Path]]:
    """(vocals, instrumental) from audio-separator's output names.

    It labels stems in the file name - `..._(Vocals)_model....wav`. Matching the
    label is the only safe way round: guessing by order or file size and getting
    it backwards would chart the INSTRUMENTAL, and the failure would show up as
    a mysteriously terrible chart rather than as an error.
    """
    vocals = instrumental = None
    for name in outputs:
        path = Path(name)
        low = path.name.lower()
        if "(vocals)" in low or "_vocals" in low:
            vocals = vocals or path
        elif "(instrumental)" in low or "_instrumental" in low or "(no vocals)" in low:
            instrumental = instrumental or path
    return vocals, instrumental


def _write_scaled_wav(src: Path, dest: Path, peak: float) -> None:
    """Copy a stem to `dest` scaled to `peak`, preserving channels and rate."""
    import numpy as np
    import soundfile as sf

    audio, rate = sf.read(str(src), dtype="float32", always_2d=True)
    top = float(np.abs(audio).max()) if audio.size else 0.0
    if top > 1e-9:
        audio = audio * (peak / top)
    dest.parent.mkdir(parents=True, exist_ok=True)
    sf.write(str(dest), audio, rate)


def _write_mono_wav(src: Path, dest: Path) -> None:
    """Copy a stem to `dest` as mono, at its own sample rate.

    Every analysis stage downmixes to mono anyway (f0.load_audio_16k), so doing
    it once here halves the size of a file that gets read four times.
    """
    import numpy as np
    import soundfile as sf

    audio, rate = sf.read(str(src), dtype="float32", always_2d=True)
    mono = audio.mean(axis=1) if audio.shape[1] > 1 else audio[:, 0]
    top = float(np.abs(mono).max()) if mono.size else 0.0
    if top > 1.0:
        mono = mono / top
    dest.parent.mkdir(parents=True, exist_ok=True)
    sf.write(str(dest), mono, rate)


# --------------------------------------------------------------------------- #
# the stage
# --------------------------------------------------------------------------- #

def separate_song(
    song: SongPaths,
    *,
    force: bool = False,
    model: str = DEFAULT_MODEL,
    python: Optional[str | Path] = None,
    model_dir: Optional[str | Path] = None,
    write_backing: bool = True,
    keep_intermediates: bool = False,
    progress: Optional[Progress] = None,
    on_percent: Optional[PercentSink] = None,
    cancel: Optional[CancelCheck] = None,
) -> SeparationResult:
    """Split `song`'s source recording into stems/vocals.wav + backing.wav.

    Returns a SeparationResult with `skipped=True` when a usable stem is already
    in place - which is the common case and the preferred one.
    """
    say: Progress = progress or (lambda _msg: None)

    source = song.source()
    if source is None:
        raise SeparationError(
            f"no source recording in {song.root}.\n"
            f"Stage 0 separates source.* into stems; there is nothing to separate. "
            f"Use 'vocalchart add-song <audiofile> {song.root}' to put one there.")

    provenance = _read_provenance(song)
    source_sha1 = sha1_of_file(source)

    # ---- is there already a stem, and whose is it?
    if song.vocals_wav.exists() and not force:
        if provenance is None:
            return SeparationResult(
                vocals=song.vocals_wav, instrumental=(
                    song.backing_wav if song.backing_wav.exists() else None),
                model="none", separator_id="none", skipped=True,
                reason=("stems/vocals.wav was supplied by hand, not separated. A genuine "
                        "stem beats any separator (14.19% vs 21.08% downstream word "
                        "error), so it is left alone. Pass --force to overwrite it."))
        if provenance.get("sourceSha1") == source_sha1:
            return SeparationResult(
                vocals=song.vocals_wav, instrumental=(
                    song.backing_wav if song.backing_wav.exists() else None),
                model=provenance.get("model", model),
                device=provenance.get("device", "unknown"),
                skipped=True,
                reason=f"already separated from this exact source with "
                       f"{provenance.get('model', model)}")
        say("source recording changed since the last separation; re-separating")

    interpreter = find_separator_python(python)
    models = Path(model_dir) if model_dir else default_model_dir()
    say(f"stage 0: separating {source.name} with {model}")
    say(f"  interpreter {interpreter}")

    work = song.cache_dir / "separate"
    work.mkdir(parents=True, exist_ok=True)
    for stale in work.glob("*"):
        if stale.is_file():
            stale.unlink()

    result = _run_driver(interpreter, audio=source, out_dir=work, model=model,
                         model_dir=models, say=say, on_percent=on_percent,
                         cancel=cancel)

    outputs = [str(p) for p in result.get("outputs", [])]
    device = str(result.get("device", "unknown"))
    vocals_src, instrumental_src = _classify(outputs)

    if vocals_src is None or not Path(vocals_src).exists():
        raise SeparationError(
            "separation produced no file labelled '(Vocals)'.\n"
            f"Got: {', '.join(Path(o).name for o in outputs) or '(nothing)'}\n"
            f"Model '{model}' may not be a two-stem vocal model.")

    song.ensure_dirs()
    _write_mono_wav(Path(vocals_src), song.vocals_wav)
    say(f"  wrote {song.vocals_wav.name}")

    wrote_backing = False
    if write_backing and instrumental_src is not None and Path(instrumental_src).exists():
        # A hand-made backing.wav (from import-stems) outranks a separated one,
        # for the same reason a real stem outranks a separated stem.
        backing_is_ours = provenance is not None and provenance.get("wroteBacking")
        if song.backing_wav.exists() and not backing_is_ours and not force:
            say("  kept the existing backing.wav (not ours to overwrite; --force to replace)")
        else:
            _write_scaled_wav(Path(instrumental_src), song.backing_wav, BACKING_PEAK)
            wrote_backing = True
            say(f"  wrote {song.backing_wav.name} (instrumental, peak {BACKING_PEAK})")
    elif write_backing:
        say("  no '(Instrumental)' output; backing.wav not written")

    _write_provenance(song, model=model, source_sha1=source_sha1, device=device,
                      wrote_backing=wrote_backing)

    if not keep_intermediates:
        for leftover in work.glob("*"):
            if leftover.is_file():
                leftover.unlink()
        try:
            work.rmdir()
        except OSError:                                # pragma: no cover
            pass

    return SeparationResult(
        vocals=song.vocals_wav,
        instrumental=song.backing_wav if wrote_backing else (
            song.backing_wav if song.backing_wav.exists() else None),
        model=model,
        device=device,
        skipped=False,
        reason="",
    )
