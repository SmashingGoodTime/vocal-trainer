"""Progress reporting for long pipeline runs.

Two consumers, one source of truth:

- a human at a prompt, who wants `  * separating` lines on stderr;
- the plugin's "Add song..." button, which spawns this CLI and needs a number
  to put in a progress bar.

The machine protocol is **one JSON object per line on stdout**, emitted only
under `--progress-json`. stdout carries nothing else in that mode, so the C++
side can parse line by line without a JSON tokeniser.

    {"event":"stage","name":"separate","index":1,"total":5,"message":"..."}
    {"event":"progress","percent":37.5,"message":"separating 68%"}
    {"event":"log","message":"..."}
    {"event":"done","songDir":"...","chart":"...","notes":167,...}
    {"event":"error","message":"..."}

`percent` is always **overall** progress across the whole run, never
stage-local - a bar that resets to zero five times reads as a hang.

Stage weights are wall-clock shares measured on an RTX-class GPU (DESIGN.md
section 4's runtime table). They are deliberately not exact: the point is that
the bar advances at a roughly even rate, not that it predicts the finish.
"""

from __future__ import annotations

import json
import sys
from typing import Callable, Iterable, Optional

import click

__all__ = ["STAGE_WEIGHTS", "Reporter"]

# name -> relative cost. Separation and lyric transcription dominate; the numpy
# stages are noise by comparison.
STAGE_WEIGHTS: dict[str, float] = {
    "ingest": 2.0,
    "separate": 55.0,
    "f0": 6.0,
    "notes": 4.0,
    "lyrics": 28.0,
    "chart": 5.0,
}


class Reporter:
    """Emits progress as human text, machine JSON, or nothing at all."""

    def __init__(self, stages: Iterable[str], *, json_mode: bool = False,
                 verbose: bool = False) -> None:
        self.stages = [s for s in stages]
        self.json_mode = bool(json_mode)
        self.verbose = bool(verbose) or self.json_mode
        self._weights = [STAGE_WEIGHTS.get(s, 5.0) for s in self.stages]
        self._total = sum(self._weights) or 1.0
        self._index = -1
        self._last_emitted = -1.0

    # ------------------------------------------------------------------ output

    def _emit(self, **payload: object) -> None:
        if not self.json_mode:
            return
        sys.stdout.write(json.dumps(payload, ensure_ascii=False) + "\n")
        sys.stdout.flush()

    # ------------------------------------------------------------------ stages

    def stage(self, name: str, message: str = "") -> None:
        if name in self.stages:
            self._index = self.stages.index(name)
        self._emit(event="stage", name=name, index=self._index + 1,
                   total=len(self.stages), message=message or name)
        self._emit_percent(self._base(), message or name)
        if message:
            self.log(message)

    def _base(self) -> float:
        """Overall percent at the START of the current stage."""
        if self._index <= 0:
            return 0.0
        return 100.0 * sum(self._weights[:self._index]) / self._total

    def _span(self) -> float:
        if self._index < 0:
            return 0.0
        return 100.0 * self._weights[self._index] / self._total

    # ---------------------------------------------------------------- progress

    def percent(self, stage_percent: float, message: str = "") -> None:
        """Report progress WITHIN the current stage, 0-100."""
        clamped = max(0.0, min(100.0, float(stage_percent)))
        self._emit_percent(self._base() + self._span() * clamped / 100.0, message)

    def _emit_percent(self, overall: float, message: str) -> None:
        overall = max(0.0, min(100.0, overall))
        # A bar that only ever moves forward is worth more than a precise one:
        # stage estimates overlap, and a number that goes backwards looks broken.
        if overall < self._last_emitted:
            return
        # Sub-0.5% updates are pure noise on a pipe the plugin polls.
        if self._last_emitted >= 0.0 and overall - self._last_emitted < 0.5:
            return
        self._last_emitted = overall
        self._emit(event="progress", percent=round(overall, 1), message=message)

    # -------------------------------------------------------------------- text

    def log(self, message: str) -> None:
        if self.json_mode:
            self._emit(event="log", message=message)
        elif self.verbose:
            # ASCII only: the default Windows console is cp1252 and a
            # box-drawing bullet raises UnicodeEncodeError there.
            click.echo(click.style("  * ", fg="cyan") + message, err=True)

    @property
    def say(self) -> Callable[[str], None]:
        """A plain `say(msg)` callable, for the stage functions that take one."""
        return self.log

    # ------------------------------------------------------------- termination

    def done(self, **fields: object) -> None:
        self._emit_percent(100.0, "done")
        self._emit(event="done", **fields)

    def error(self, message: str, detail: Optional[str] = None) -> None:
        self._emit(event="error", message=message, detail=detail or "")
