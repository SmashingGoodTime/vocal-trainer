"""Standalone driver that runs audio-separator inside the ISOLATED venv.

This file is executed by a different interpreter from the rest of vocalchart:

    <.venv-separate>/Scripts/python.exe -P _sep_driver.py --input ... --output-dir ...

Two consequences shape everything in here.

1. **It may import nothing from `vocalchart`.** That package is not installed in
   the separation venv and never will be - the whole point of the split is that
   torch (2.5 GB, CUDA) stays out of the ONNX-only analysis venv. Stdlib plus
   `audio_separator` only.

2. **It is invoked with `-P`** so that this file's own directory does NOT go on
   `sys.path`. Its siblings are `chart.py`, `cli.py`, `paths.py`, `segment.py` -
   names generic enough to shadow a third-party import somewhere inside
   audio-separator's dependency tree. `-P` needs Python >= 3.11, which both venvs
   are pinned to.

Protocol: one JSON object per line on STDOUT, nothing else.

    {"event": "status", "message": "loading BS-Roformer"}
    {"event": "result", "outputs": ["...(Vocals)....wav", "...(Instrumental)....wav"],
     "device": "cuda", "model": "..."}
    {"event": "error", "message": "...", "traceback": "..."}

Everything audio-separator itself logs goes to STDERR untouched, so the caller
can scrape tqdm percentages off it without corrupting the protocol.
"""

from __future__ import annotations

import argparse
import json
import logging
import os
import sys
import traceback
from pathlib import Path


def emit(**payload: object) -> None:
    """One JSON object, one line, flushed. The caller reads this incrementally."""
    sys.stdout.write(json.dumps(payload, ensure_ascii=False) + "\n")
    sys.stdout.flush()


def detect_device() -> str:
    try:
        import torch
    except Exception:                                  # noqa: BLE001
        return "unknown"
    try:
        if torch.cuda.is_available():
            return f"cuda ({torch.cuda.get_device_name(0)})"
    except Exception:                                  # noqa: BLE001
        pass
    return "cpu"


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description="audio-separator driver (isolated venv)")
    ap.add_argument("--input", required=True)
    ap.add_argument("--output-dir", required=True)
    ap.add_argument("--model", required=True)
    ap.add_argument("--model-dir", required=True)
    args = ap.parse_args(argv)

    out_dir = Path(args.output_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    Path(args.model_dir).mkdir(parents=True, exist_ok=True)

    try:
        from audio_separator.separator import Separator
    except Exception as exc:                           # noqa: BLE001
        emit(event="error",
             message=f"audio-separator is not importable in {sys.executable}: {exc}",
             traceback=traceback.format_exc())
        return 2

    device = detect_device()
    emit(event="status", message=f"separation device: {device}")

    try:
        # Only the four arguments that have been stable across audio-separator
        # releases are passed. Everything else keeps its default, so a signature
        # change upstream cannot break the call.
        separator = Separator(
            log_level=logging.INFO,
            model_file_dir=str(args.model_dir),
            output_dir=str(out_dir),
            output_format="WAV",
        )

        emit(event="status", message=f"loading {args.model}")
        # First run downloads the checkpoint (~600 MB) into --model-dir; every
        # run after that is a cache hit.
        separator.load_model(model_filename=args.model)

        emit(event="status", message="separating")
        outputs = separator.separate(str(args.input))
    except Exception as exc:                           # noqa: BLE001
        emit(event="error", message=f"{type(exc).__name__}: {exc}",
             traceback=traceback.format_exc())
        return 1

    # separate() returns bare file names in some releases and absolute paths in
    # others. Normalise to absolute paths against the output dir either way.
    resolved: list[str] = []
    for name in outputs or []:
        p = Path(name)
        if not p.is_absolute():
            p = out_dir / p
        resolved.append(str(p))

    emit(event="result", outputs=resolved, device=device, model=args.model)
    return 0


if __name__ == "__main__":
    # Unbuffered-ish stdout: the caller streams our lines while we are still
    # running, and a buffered pipe would hold status lines back for minutes.
    os.environ.setdefault("PYTHONUNBUFFERED", "1")
    sys.exit(main())
