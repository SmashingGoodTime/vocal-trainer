"""vocalchart — offline pipeline that turns a song into a vocal note chart.

Milestone 2 of the Vocal Trainer project. See docs/DESIGN.md sections 3 (chart
format, normative) and 4 (offline pipeline).

    from vocalchart import Chart, Note, SongPaths
    chart = Chart.read_json("songs/amazing-grace-demo/chart.json")

`vocalchart.f0` pulls in numpy/librosa/swift_f0 and is therefore imported lazily:
`from vocalchart import f0` and `vocalchart.f0.analyse_f0(...)` both work, but
merely importing this package does not pay for the analysis stack.
"""

from __future__ import annotations

from typing import TYPE_CHECKING, Any

__version__ = "0.1.1"

from .chart import (
    LYRIC_SOURCES,
    MIDI_MAX,
    MIDI_MIN,
    NOTE_TYPES,
    SCHEMA_URI,
    SCHEMA_VERSION,
    SYLLABLE_ROLES,
    Chart,
    ChartValidationError,
    Contour,
    Generator,
    Note,
    Phrase,
    Song,
    Timing,
    Warning,
)
from .paths import SongPaths, default_library_root, is_cache_fresh, sha1_of_file

if TYPE_CHECKING:  # pragma: no cover
    from .f0 import F0Result, analyse_f0

_LAZY = {
    "F0Result": ("vocalchart.f0", "F0Result"),
    "analyse_f0": ("vocalchart.f0", "analyse_f0"),
    "f0": ("vocalchart.f0", None),
}


def __getattr__(name: str) -> Any:
    """Defer the numpy/librosa/onnxruntime import until something needs it."""
    target = _LAZY.get(name)
    if target is None:
        raise AttributeError(f"module {__name__!r} has no attribute {name!r}")
    import importlib

    module = importlib.import_module(target[0])
    return module if target[1] is None else getattr(module, target[1])


__all__ = [
    "__version__",
    # chart model
    "Chart",
    "ChartValidationError",
    "Contour",
    "Generator",
    "Note",
    "Phrase",
    "Song",
    "Timing",
    "Warning",
    "SCHEMA_URI",
    "SCHEMA_VERSION",
    "NOTE_TYPES",
    "SYLLABLE_ROLES",
    "LYRIC_SOURCES",
    "MIDI_MIN",
    "MIDI_MAX",
    # layout
    "SongPaths",
    "default_library_root",
    "is_cache_fresh",
    "sha1_of_file",
    # lazy
    "F0Result",
    "analyse_f0",
]
