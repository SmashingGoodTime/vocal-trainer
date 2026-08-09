"""Stage 1 — f0 extraction with SwiftF0.

DESIGN.md section 4, stage 1: SwiftF0 is 95,842 parameters, ONNX only (the model
is vendored in the wheel — no TensorFlow, no torch). 91.80% harmonic-mean accuracy
at 10 dB SNR, 12 points above CREPE, with 2.27 points degradation from clean audio
(arXiv:2508.18440). Constrained here to 60–1100 Hz for vocals.

The API used below was verified by introspecting the installed swift_f0 0.1.2:

    SwiftF0(confidence_threshold=None, fmin=None, fmax=None)
    SwiftF0.detect_from_array(audio_array: np.ndarray, sample_rate: int) -> PitchResult
    PitchResult(pitch_hz, confidence, timestamps, voicing)   # all np.ndarray
    SwiftF0.TARGET_SAMPLE_RATE = 16000, HOP_LENGTH = 256, FRAME_LENGTH = 1024
    SwiftF0.MODEL_FMIN = 46.875, MODEL_FMAX = 2093.75, CENTER_OFFSET = 127.5

Two behaviours worth knowing, read out of swift_f0.core:
  * `fmin`/`fmax` and `confidence_threshold` affect ONLY the boolean `voicing`
    array. `pitch_hz` is never masked or clamped. We therefore keep the raw Hz
    and re-derive voicing ourselves, so downstream stages can re-threshold
    without re-running the model.
  * timestamps are frame CENTRES: (i * 256 + 127.5) / 16000, so frame 0 sits at
    7.97 ms, not 0. Do not assume times[0] == 0.
"""

from __future__ import annotations

import json
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Callable, Optional

import numpy as np

__all__ = [
    "VOCAL_FMIN_HZ",
    "VOCAL_FMAX_HZ",
    "VOICING_CONFIDENCE",
    "F0_CACHE_FORMAT",
    "F0Result",
    "estimate_tuning_hz",
    "load_audio_16k",
    "analyse_f0",
    "load_f0_cache",
    "save_f0_cache",
]

# DESIGN.md section 4 stage 1: "Constrain to 60-1100 Hz for vocals."
VOCAL_FMIN_HZ = 60.0
VOCAL_FMAX_HZ = 1100.0

# DESIGN.md section 4 stage 2 step 2: "Mask frames with confidence < 0.50 as unvoiced."
VOICING_CONFIDENCE = 0.50

# Bumped whenever the meaning of the arrays in f0.npz changes, so an old cache is
# recomputed instead of silently misinterpreted.
F0_CACHE_FORMAT = 1

# librosa.estimate_tuning needs enough material to build a stable histogram.
# Measured on synthetic equal-tempered material: 10 s of harmonic tones tracks
# ground truth to 1-2 cents, while 6 s of a five-note figure produced a 47-cent
# error. Below this we still report an estimate but flag it as unreliable rather
# than baking a bogus global offset into every note of the chart.
TUNING_MIN_SECONDS = 15.0

_TARGET_SR = 16000


# --------------------------------------------------------------------------- #
# result
# --------------------------------------------------------------------------- #

@dataclass
class F0Result:
    """Stage 1 output.

    times          frame centres in seconds, ascending
    hz             raw estimated f0 per frame, NEVER masked (see module docstring)
    confidence     model confidence 0..1 per frame
    hop_seconds    time between frames (0.016 s for swift-f0 0.1.2)
    tuning_hz      concert-A reference measured from THIS recording, not assumed

    `fmin_hz` / `fmax_hz` are the plausibility band the detector was configured
    with; they are extra to the agreed five-field contract and carry defaults, so
    F0Result(times, hz, confidence, hop_seconds, tuning_hz) still constructs.
    """

    times: np.ndarray
    hz: np.ndarray
    confidence: np.ndarray
    hop_seconds: float
    tuning_hz: float
    fmin_hz: float = VOCAL_FMIN_HZ
    fmax_hz: float = VOCAL_FMAX_HZ
    meta: dict[str, Any] = field(default_factory=dict)

    def __post_init__(self) -> None:
        self.times = np.asarray(self.times, dtype=np.float64)
        self.hz = np.asarray(self.hz, dtype=np.float64)
        self.confidence = np.asarray(self.confidence, dtype=np.float64)
        self.hop_seconds = float(self.hop_seconds)
        self.tuning_hz = float(self.tuning_hz)
        if not (self.times.shape == self.hz.shape == self.confidence.shape):
            raise ValueError(
                f"F0Result arrays disagree: times {self.times.shape}, hz {self.hz.shape}, "
                f"confidence {self.confidence.shape}"
            )

    # ------------------------------------------------------------------ views

    def __len__(self) -> int:
        return int(self.times.shape[0])

    @property
    def duration_seconds(self) -> float:
        """End of the last frame, i.e. the analysed span rather than the last centre."""
        if len(self) == 0:
            return 0.0
        return float(self.times[-1] + self.hop_seconds / 2.0)

    def voiced(self, confidence_threshold: float = VOICING_CONFIDENCE) -> np.ndarray:
        """Boolean mask: confident enough AND inside the vocal plausibility band.

        Recomputed rather than cached from SwiftF0's own `voicing`, because
        SwiftF0 defaults to a 0.9 threshold and DESIGN.md calls for 0.50.
        """
        with np.errstate(invalid="ignore"):
            return (
                (self.confidence >= confidence_threshold)
                & np.isfinite(self.hz)
                & (self.hz >= self.fmin_hz)
                & (self.hz <= self.fmax_hz)
            )

    def frac_midi(self, confidence_threshold: Optional[float] = None) -> np.ndarray:
        """Continuous fractional MIDI: m = 69 + 12*log2(f / tuning_hz).

        NaN where unvoiced. Note the divisor is the MEASURED tuning, not 440 —
        skipping that bakes a global pitch offset into every chart
        (DESIGN.md section 4, stage 2 step 1).
        """
        out = np.full(self.hz.shape, np.nan, dtype=np.float64)
        positive = np.isfinite(self.hz) & (self.hz > 0.0)
        np.log2(self.hz, out=out, where=positive)
        out = 69.0 + 12.0 * (out - np.log2(self.tuning_hz))
        mask = positive
        if confidence_threshold is not None:
            mask = mask & self.voiced(confidence_threshold)
        return np.where(mask, out, np.nan)

    def cents(self, confidence_threshold: Optional[float] = None) -> np.ndarray:
        """frac_midi in cents. Convenience for the segmentation heuristics."""
        return self.frac_midi(confidence_threshold) * 100.0

    def resample_frac_midi(
        self,
        rate_hz: float = 100.0,
        start_seconds: float = 0.0,
        duration_seconds: Optional[float] = None,
        confidence_threshold: float = VOICING_CONFIDENCE,
    ) -> tuple[np.ndarray, np.ndarray]:
        """Nearest-frame resample onto a uniform grid, for chart.contour.

        Returns (frac_midi with NaN where unvoiced, confidence). Nearest-frame,
        not interpolation: interpolating across an unvoiced gap invents pitch
        that the reference singer never produced, and the ghost line is supposed
        to be evidence, not a guess.
        """
        if duration_seconds is None:
            duration_seconds = max(0.0, self.duration_seconds - start_seconds)
        n = int(round(duration_seconds * rate_hz))
        if n <= 0 or len(self) == 0:
            return np.empty(0, dtype=np.float64), np.empty(0, dtype=np.float64)

        grid = start_seconds + np.arange(n, dtype=np.float64) / rate_hz
        idx = np.searchsorted(self.times, grid)
        idx = np.clip(idx, 1, len(self) - 1) if len(self) > 1 else np.zeros(n, dtype=int)
        if len(self) > 1:
            left = idx - 1
            take_left = np.abs(grid - self.times[left]) <= np.abs(self.times[idx] - grid)
            idx = np.where(take_left, left, idx)

        # A grid point more than one hop away from any real frame has no data.
        too_far = np.abs(grid - self.times[idx]) > self.hop_seconds
        fm = self.frac_midi(confidence_threshold)[idx]
        conf = self.confidence[idx]
        fm = np.where(too_far, np.nan, fm)
        conf = np.where(too_far, 0.0, conf)
        return fm, conf

    # ------------------------------------------------------------------- i/o

    def to_npz_dict(self) -> dict[str, Any]:
        return {
            "times": self.times.astype(np.float64),
            "hz": self.hz.astype(np.float32),
            "confidence": self.confidence.astype(np.float32),
            "hop_seconds": np.float64(self.hop_seconds),
            "tuning_hz": np.float64(self.tuning_hz),
            "fmin_hz": np.float64(self.fmin_hz),
            "fmax_hz": np.float64(self.fmax_hz),
            "meta": np.array(json.dumps(self.meta)),
        }


# --------------------------------------------------------------------------- #
# audio + tuning
# --------------------------------------------------------------------------- #

def load_audio_16k(path: str | Path) -> np.ndarray:
    """Decode to mono float32 at 16 kHz — SwiftF0's native rate.

    librosa.load handles every format soundfile can open and falls back to
    audioread/ffmpeg for the rest, so mp3 works without a separate code path.
    Resampling here rather than inside detect_from_array means the same samples
    feed both the tuning estimate and the model.
    """
    import librosa

    audio, _ = librosa.load(str(path), sr=_TARGET_SR, mono=True)
    audio = np.asarray(audio, dtype=np.float32)
    if audio.size == 0:
        raise ValueError(f"{path} decoded to zero samples")
    return audio


def estimate_tuning_hz(audio: np.ndarray, sample_rate: int = _TARGET_SR) -> float:
    """Concert-A reference of this recording, in Hz.

    DESIGN.md section 4, stage 2 step 1: estimate this FIRST. Many pop records
    are not at A=440 (tape speed, deliberate detuning, a piano nobody tuned), and
    skipping this bakes a global pitch offset into every note of every chart.

    librosa.estimate_tuning returns a deviation in fractions of a semitone bin
    within [-0.5, 0.5), which maps to Hz as 440 * 2**(tuning / 12).

    FEED THIS THE FULL MIX, NOT THE VOCAL STEM. estimate_tuning peak-picks the
    spectrum, and vibrato smears every partial across bins, which wrecks it.
    Measured on synthetic material with 8 cents of 5.5 Hz vibrato: a solo vocal
    gave 37 to -49 cents against a known -30 to +35 cent ground truth (i.e.
    noise), while the same vocal over fixed-pitch backing chords came within
    0-2 cents. A record's concert-A is a property of its fretted and keyed
    instruments, not of the singer, so the mix is also the semantically correct
    input. See f0_for_song(), which routes source.* here for exactly this reason.

    bins_per_octave is pinned to 12 deliberately. With 24 the returned value
    wraps at +/-25 cents, so a record tuned 40 cents flat reads as +10 cents
    sharp - measured, not theorised.
    """
    import librosa

    tuning = float(librosa.estimate_tuning(y=np.asarray(audio, dtype=np.float32),
                                           sr=sample_rate,
                                           bins_per_octave=12))
    if not np.isfinite(tuning):
        return 440.0
    # estimate_tuning is bounded by construction; clamp anyway so a pathological
    # input can never produce a nonsense reference.
    tuning = float(np.clip(tuning, -0.5, 0.5))
    return 440.0 * (2.0 ** (tuning / 12.0))


# --------------------------------------------------------------------------- #
# stage 1
# --------------------------------------------------------------------------- #

def analyse_f0(
    audio_path: str | Path,
    fmin_hz: float = VOCAL_FMIN_HZ,
    fmax_hz: float = VOCAL_FMAX_HZ,
    confidence_threshold: float = VOICING_CONFIDENCE,
    tuning_hz: Optional[float] = None,
    tuning_audio_path: Optional[str | Path] = None,
    progress: Optional[Callable[[str], None]] = None,
) -> F0Result:
    """Run SwiftF0 over one audio file. No caching - see f0_for_song().

    `tuning_audio_path` is the file the tuning estimate is measured from, which
    should be the full mix even though pitch is tracked on the vocal stem. See
    estimate_tuning_hz() for the measurements behind that split.
    """
    from swift_f0 import SwiftF0

    say = progress or (lambda _msg: None)
    path = Path(audio_path)

    say(f"decoding {path.name} to mono 16 kHz")
    audio = load_audio_16k(path)
    audio_seconds = audio.size / _TARGET_SR

    tuning_source = path
    tuning_seconds = audio_seconds
    tuning_from_mix = False
    tuning_explicit = tuning_hz is not None
    if tuning_hz is None:
        tuning_audio = audio
        if tuning_audio_path is not None and Path(tuning_audio_path) != path:
            tuning_source = Path(tuning_audio_path)
            say(f"decoding {tuning_source.name} for the tuning estimate (full mix)")
            tuning_audio = load_audio_16k(tuning_source)
            tuning_seconds = tuning_audio.size / _TARGET_SR
            tuning_from_mix = True
        say("estimating tuning (librosa.estimate_tuning)")
        tuning_hz = estimate_tuning_hz(tuning_audio, _TARGET_SR)

    tuning_reliable = tuning_seconds >= TUNING_MIN_SECONDS and tuning_from_mix
    tuning_measured_hz = float(tuning_hz)
    cents_off_440 = 1200.0 * float(np.log2(tuning_measured_hz / 440.0))
    say(f"tuning A4 = {tuning_measured_hz:.2f} Hz ({cents_off_440:+.1f} cents from 440), "
        f"from {tuning_source.name}")

    if tuning_explicit:
        # The caller stated the reference. Never second-guess it.
        pass
    elif not tuning_reliable:
        # DO NOT USE A NUMBER WE HAVE ALREADY DECIDED WE DISTRUST.
        #
        # Measured: a 7.7 s synthetic vocal generated at exactly A=440 estimated
        # to 429.70 Hz, i.e. -41 cents. Using it shifted centsOffset on EVERY
        # note by +35 to +43 cents, and a bare 9 more cents of error would have
        # pushed every note across a semitone boundary and transposed the whole
        # chart. Falling back to 440 is wrong only for genuinely off-tuning
        # records, and it is wrong by a bounded, predictable amount rather than
        # by whatever noise the estimator happened to emit.
        reason = (f"only {tuning_seconds:.1f} s of audio (need "
                  f"{TUNING_MIN_SECONDS:.0f} s)" if tuning_seconds < TUNING_MIN_SECONDS
                  else "no full mix was available, and vibrato wrecks estimate_tuning "
                       "on a solo vocal")
        say(f"WARNING: ignoring the measured tuning ({cents_off_440:+.1f} cents) - "
            f"{reason}. Falling back to A=440. Supply source.* (the full mix) "
            f"alongside the stem, or set tuning_hz explicitly, if this record is "
            f"genuinely not at concert pitch.")
        tuning_hz = 440.0

    say(f"running SwiftF0 over {audio.size / _TARGET_SR:.1f} s, band {fmin_hz:g}-{fmax_hz:g} Hz")
    t0 = time.perf_counter()
    detector = SwiftF0(
        confidence_threshold=confidence_threshold,
        fmin=fmin_hz,
        fmax=fmax_hz,
    )
    result = detector.detect_from_array(audio, _TARGET_SR)
    elapsed = time.perf_counter() - t0

    times = np.asarray(result.timestamps, dtype=np.float64)
    hop = float(np.median(np.diff(times))) if times.size > 1 else \
        float(SwiftF0.HOP_LENGTH) / float(SwiftF0.TARGET_SAMPLE_RATE)

    f0 = F0Result(
        times=times,
        hz=np.asarray(result.pitch_hz, dtype=np.float64),
        confidence=np.asarray(result.confidence, dtype=np.float64),
        hop_seconds=hop,
        tuning_hz=float(tuning_hz),
        fmin_hz=float(fmin_hz),
        fmax_hz=float(fmax_hz),
        meta={
            "format": F0_CACHE_FORMAT,
            "model": "swift-f0 0.1.2",
            "source": path.name,
            "sourceMtime": path.stat().st_mtime,
            "sourceBytes": path.stat().st_size,
            "sampleRate": _TARGET_SR,
            "fminHz": float(fmin_hz),
            "fmaxHz": float(fmax_hz),
            "confidenceThreshold": float(confidence_threshold),
            "analysisSeconds": elapsed,
            "audioSeconds": audio_seconds,
            "tuningSource": tuning_source.name,
            "tuningSeconds": tuning_seconds,
            "tuningFromMix": bool(tuning_from_mix),
            # What the estimator measured, kept for diagnostics even when it was
            # rejected. tuning_hz above is what was actually USED.
            "tuningMeasuredHz": tuning_measured_hz,
            "tuningMeasuredCentsFrom440": cents_off_440,
            "tuningReliable": bool(tuning_reliable),
            "tuningExplicit": bool(tuning_explicit),
            "tuningRejected": bool(not tuning_explicit and not tuning_reliable),
        },
    )
    voiced_frac = float(np.mean(f0.voiced(confidence_threshold))) if len(f0) else 0.0
    say(
        f"{len(f0)} frames at {hop * 1000:.1f} ms hop, {voiced_frac * 100:.1f}% voiced, "
        f"{elapsed:.1f} s"
    )
    return f0


# --------------------------------------------------------------------------- #
# cache
# --------------------------------------------------------------------------- #

def save_f0_cache(f0: F0Result, cache_path: str | Path) -> Path:
    out = Path(cache_path)
    out.parent.mkdir(parents=True, exist_ok=True)
    np.savez_compressed(out, **f0.to_npz_dict())
    return out


def load_f0_cache(cache_path: str | Path) -> F0Result:
    with np.load(Path(cache_path), allow_pickle=False) as z:
        meta: dict[str, Any] = {}
        if "meta" in z:
            try:
                meta = json.loads(str(z["meta"]))
            except (ValueError, TypeError):
                meta = {}
        return F0Result(
            times=z["times"],
            hz=z["hz"],
            confidence=z["confidence"],
            hop_seconds=float(z["hop_seconds"]),
            tuning_hz=float(z["tuning_hz"]),
            fmin_hz=float(z["fmin_hz"]) if "fmin_hz" in z else VOCAL_FMIN_HZ,
            fmax_hz=float(z["fmax_hz"]) if "fmax_hz" in z else VOCAL_FMAX_HZ,
            meta=meta,
        )


def _source_changed(meta: dict, audio_path: str | Path) -> bool:
    """True when the audio no longer matches what the cache was computed from.

    Mtime ORDERING alone cannot catch this: shutil.copy2 and a Windows Explorer
    copy both preserve the source file's original mtime, so a hand-replaced
    stems/vocals.wav can be OLDER than the cache while being different audio.
    Equality against the stat recorded at analysis time is what detects a swap.
    """
    recorded_mtime = meta.get("sourceMtime")
    recorded_bytes = meta.get("sourceBytes")

    if recorded_mtime is None or recorded_bytes is None:
        return False                    # legacy cache: no fingerprint, no opinion

    try:
        st = Path(audio_path).stat()
    except OSError:
        return True

    return (int(recorded_bytes) != st.st_size
            or abs(float(recorded_mtime) - st.st_mtime) > 1e-3)


def f0_for_song(
    song,                       # paths.SongPaths, imported lazily to avoid a cycle
    force: bool = False,
    fmin_hz: float = VOCAL_FMIN_HZ,
    fmax_hz: float = VOCAL_FMAX_HZ,
    confidence_threshold: float = VOICING_CONFIDENCE,
    progress: Optional[Callable[[str], None]] = None,
) -> F0Result:
    """Stage 1 with the cache/f0.npz cache.

    Recomputes when: --force, no cache, the cache is older than the audio, the
    cache format changed, or the detection band changed. Anything else reuses it
    — chart prep is a once-per-song cost with expected interactive fixups, so
    caching matters more than throughput (DESIGN.md section 4).
    """
    from .paths import is_cache_fresh

    say = progress or (lambda _msg: None)
    audio_path = song.analysis_input()
    cache_path = song.f0_npz

    # Pitch comes off the vocal stem; tuning comes off the full mix. Splitting
    # them is not fussiness - estimate_tuning is close to useless on a solo
    # vibrato vocal (see estimate_tuning_hz for the numbers).
    tuning_audio_path = song.source() or audio_path

    if not force and is_cache_fresh(cache_path, audio_path, tuning_audio_path):
        try:
            cached = load_f0_cache(cache_path)
        except Exception as exc:  # corrupt or truncated npz - just redo the work
            say(f"f0 cache unreadable ({exc}); recomputing")
        else:
            stale = (
                cached.meta.get("format") != F0_CACHE_FORMAT
                or abs(cached.fmin_hz - fmin_hz) > 1e-9
                or abs(cached.fmax_hz - fmax_hz) > 1e-9
                or _source_changed(cached.meta, audio_path)
            )
            if not stale:
                say(f"f0 cache hit: {cache_path} ({len(cached)} frames)")
                return cached
            say("f0 cache does not match the current audio/settings; recomputing")

    f0 = analyse_f0(
        audio_path,
        fmin_hz=fmin_hz,
        fmax_hz=fmax_hz,
        confidence_threshold=confidence_threshold,
        tuning_audio_path=tuning_audio_path,
        progress=progress,
    )
    save_f0_cache(f0, cache_path)
    say(f"wrote {cache_path}")
    return f0
