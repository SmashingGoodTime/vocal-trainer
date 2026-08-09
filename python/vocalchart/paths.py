"""Song folder layout and cache freshness.

DESIGN.md section 4:

    songs/<songId>/
      source.mp3            # input
      lyrics.txt            # pasted plain text (STRONGLY recommended)
      stems/vocals.wav      # stage 0 output (or user-supplied - always prefer this)
      cache/f0.npz          # stage 1
      cache/notes.json      # stage 2
      cache/words.json      # stage 3
      chart.json            # stage 5 - normative
      chart.ultrastar.txt   # stage 5 - editing sidecar
      backing.wav

Every path in a chart is stored relative to chart.json. The library root lives in
the plugin's PropertiesFile, never in the chart and never in plugin state.
"""

from __future__ import annotations

import hashlib
import os
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable, Optional

__all__ = [
    "AUDIO_EXTENSIONS",
    "SongPaths",
    "atomic_write_text",
    "default_library_root",
    "file_fingerprint",
    "newest_mtime",
    "is_cache_fresh",
    "sha1_of_file",
]

# Extensions accepted for `source.*`, in preference order (lossless first).
AUDIO_EXTENSIONS = (".wav", ".flac", ".mp3", ".m4a", ".aac", ".ogg", ".opus", ".wma")


def default_library_root() -> Path:
    """%APPDATA%\\VocalTrainer\\songs on Windows, ~/.local/share/... elsewhere.

    Matches DESIGN.md section 3: the plugin resolves its library root from a
    juce::PropertiesFile under userApplicationDataDirectory/VocalTrainer.
    """
    appdata = os.environ.get("APPDATA")
    if appdata:
        return Path(appdata) / "VocalTrainer" / "songs"
    xdg = os.environ.get("XDG_DATA_HOME")
    base = Path(xdg) if xdg else Path.home() / ".local" / "share"
    return base / "VocalTrainer" / "songs"


def sha1_of_file(path: str | Path, chunk_size: int = 1 << 20) -> str:
    """SHA-1 of a file. Used for song.referenceAudioSha1 — detects a swapped or
    re-rendered stem, which would silently invalidate every timestamp."""
    h = hashlib.sha1()
    with open(path, "rb") as fh:
        for chunk in iter(lambda: fh.read(chunk_size), b""):
            h.update(chunk)
    return h.hexdigest()


def newest_mtime(paths: Iterable[str | Path]) -> Optional[float]:
    """Newest mtime among the paths that exist. None if none of them do."""
    stamps = [Path(p).stat().st_mtime for p in paths if Path(p).exists()]
    return max(stamps) if stamps else None


def is_cache_fresh(cache: str | Path, *inputs: str | Path) -> bool:
    """True when `cache` exists, is non-empty, and is newer than every input.

    Inputs that do not exist are ignored rather than treated as infinitely old:
    a missing lyrics.txt should not permanently invalidate an f0 cache.

    Mtime ORDERING alone cannot catch a file replaced by a copy that preserved
    the source's mtime (shutil.copy2, Windows Explorer). Callers that can record
    a fingerprint of their inputs should ALSO compare it — see file_fingerprint.
    """
    cache_path = Path(cache)
    if not cache_path.exists() or cache_path.stat().st_size == 0:
        return False
    newest = newest_mtime(inputs)
    if newest is None:
        return True
    return cache_path.stat().st_mtime >= newest


def file_fingerprint(path: str | Path) -> Optional[dict]:
    """Cheap identity for a cache input: size + mtime, JSON-safe.

    Equality against a RECORDED fingerprint catches what mtime ordering cannot:
    a file swapped for a copy that kept the original's mtime. None when the
    file does not exist, so "no lyrics.txt" compares equal to "still none".
    The mtime is rounded so the value survives a JSON round trip exactly.
    """
    p = Path(path)
    if not p.exists():
        return None
    st = p.stat()
    return {"bytes": st.st_size, "mtime": round(st.st_mtime, 3)}


def atomic_write_text(path: str | Path, text: str, encoding: str = "utf-8") -> Path:
    """Write via a sibling temp file and os.replace, so a crash mid-write (or a
    reader arriving mid-write — the JUCE app re-reads chart.json) can never see
    a truncated file where a good one was."""
    p = Path(path)
    p.parent.mkdir(parents=True, exist_ok=True)
    tmp = p.with_name(p.name + ".tmp")
    with open(tmp, "w", encoding=encoding, newline="\n") as fh:
        fh.write(text)
    os.replace(tmp, p)
    return p


@dataclass(frozen=True)
class SongPaths:
    """Resolved layout for one songs/<songId>/ folder."""

    root: Path

    # ------------------------------------------------------------ construction

    def __post_init__(self) -> None:
        object.__setattr__(self, "root", Path(self.root).expanduser().resolve())

    @classmethod
    def for_song(cls, song_id: str, library_root: Optional[str | Path] = None) -> "SongPaths":
        base = Path(library_root) if library_root is not None else default_library_root()
        return cls(base / song_id)

    # ------------------------------------------------------------------ pieces

    @property
    def song_id(self) -> str:
        """The folder name. The ONLY thing stored in plugin state."""
        return self.root.name

    @property
    def stems_dir(self) -> Path:
        return self.root / "stems"

    @property
    def cache_dir(self) -> Path:
        return self.root / "cache"

    @property
    def lyrics_txt(self) -> Path:
        return self.root / "lyrics.txt"

    @property
    def vocals_wav(self) -> Path:
        return self.stems_dir / "vocals.wav"

    @property
    def backing_wav(self) -> Path:
        return self.root / "backing.wav"

    @property
    def f0_npz(self) -> Path:
        return self.cache_dir / "f0.npz"

    @property
    def notes_json(self) -> Path:
        return self.cache_dir / "notes.json"

    @property
    def words_json(self) -> Path:
        return self.cache_dir / "words.json"

    @property
    def chart_json(self) -> Path:
        return self.root / "chart.json"

    @property
    def chart_ultrastar_txt(self) -> Path:
        return self.root / "chart.ultrastar.txt"

    # ------------------------------------------------------------- resolution

    def source(self) -> Optional[Path]:
        """The `source.*` input file, lossless extensions preferred. None if absent."""
        for ext in AUDIO_EXTENSIONS:
            candidate = self.root / f"source{ext}"
            if candidate.exists():
                return candidate
        # Tolerate an unexpected extension rather than failing outright.
        for candidate in sorted(self.root.glob("source.*")):
            if candidate.is_file():
                return candidate
        return None

    def analysis_input(self) -> Path:
        """The audio every analysis stage should read.

        Always prefer a real vocal stem: DESIGN.md section 4 measures 14.19% word
        error on a genuine isolated stem versus 20.99% on the raw mix, and
        separation artifacts eat almost all of the separator's benefit.
        """
        if self.vocals_wav.exists():
            return self.vocals_wav
        src = self.source()
        if src is not None:
            return src
        raise FileNotFoundError(
            f"no analysis input in {self.root}: expected stems/vocals.wav or source.* "
            f"(one of {', '.join(AUDIO_EXTENSIONS)})"
        )

    def reference_audio_relative(self) -> str:
        """analysis_input() expressed relative to chart.json, forward slashes."""
        return self.analysis_input().relative_to(self.root).as_posix()

    def backing_audio_relative(self) -> Optional[str]:
        if not self.backing_wav.exists():
            return None
        return self.backing_wav.relative_to(self.root).as_posix()

    # ---------------------------------------------------------------- checks

    def exists(self) -> bool:
        return self.root.is_dir()

    def ensure_dirs(self) -> "SongPaths":
        self.root.mkdir(parents=True, exist_ok=True)
        self.cache_dir.mkdir(parents=True, exist_ok=True)
        self.stems_dir.mkdir(parents=True, exist_ok=True)
        return self

    def f0_cache_fresh(self) -> bool:
        return is_cache_fresh(self.f0_npz, self.analysis_input())

    def notes_cache_fresh(self) -> bool:
        return is_cache_fresh(self.notes_json, self.f0_npz, self.analysis_input())

    def words_cache_fresh(self) -> bool:
        return is_cache_fresh(self.words_json, self.analysis_input(), self.lyrics_txt)

    def chart_fresh(self) -> bool:
        return is_cache_fresh(
            self.chart_json, self.f0_npz, self.notes_json, self.words_json, self.lyrics_txt
        )

    def __str__(self) -> str:  # pragma: no cover - cosmetic
        return str(self.root)
