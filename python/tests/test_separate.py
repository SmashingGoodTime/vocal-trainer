"""Tests for stage 0 (separation) and the add-song ingest path.

Three things here are worth breaking a build over:

  * ``_classify`` must never mix up the two stems. Getting them backwards
    charts the INSTRUMENTAL, and the failure surfaces as a mysteriously awful
    chart rather than as an error - hours of "the pitch tracker is broken".
  * ``separate_song`` must not overwrite a stem it did not produce. DESIGN.md
    section 4 measures 14.19% downstream word error on a genuine isolated stem
    against 21.08% on a separated one, so clobbering a real multitrack with a
    reconstruction of itself is a silent quality regression.
  * ``Reporter`` percentages must be monotonic and overall. A bar that resets
    once per stage reads as a hang, and a bar that goes backwards reads as a
    bug.
"""

from __future__ import annotations

import json

import pytest

from vocalchart import separate
from vocalchart.cli import _clean_stem, _guess_title_artist, _slugify
from vocalchart.paths import SongPaths
from vocalchart.progress import Reporter


# --------------------------------------------------------------------------- #
# naming
# --------------------------------------------------------------------------- #

@pytest.mark.parametrize("raw, expected", [
    ("Mr Brightside", "mr-brightside"),
    ("Don't Stop Believin'", "dont-stop-believin"),
    ("I Can\u2019t Help Myself", "i-cant-help-myself"),
    ("Fight For Your Right (To Party)", "fight-for-your-right-to-party"),
    ("  Eye  of the   Tiger  ", "eye-of-the-tiger"),
    ("Se\u00f1orita", "senorita"),
    ("!!!", "untitled"),
])
def test_slugify_matches_the_existing_song_folders(raw, expected):
    assert _slugify(raw) == expected


@pytest.mark.parametrize("stem, expected", [
    ("03 - Mr Brightside", "Mr Brightside"),
    ("3. Mr Brightside", "Mr Brightside"),
    ("01_Mr Brightside", "Mr Brightside"),
    ("Mr Brightside", "Mr Brightside"),
    ("99 Luftballons", "99 Luftballons"),       # a number that IS the title
    ("1999", "1999"),
])
def test_clean_stem_strips_leading_track_numbers(stem, expected):
    assert _clean_stem(stem) == expected


def test_guess_title_artist_splits_on_the_one_common_convention(tmp_path):
    audio = tmp_path / "The Killers - Mr Brightside.mp3"
    audio.touch()
    assert _guess_title_artist(audio) == ("Mr Brightside", "The Killers")


def test_guess_title_artist_leaves_a_plain_name_alone(tmp_path):
    audio = tmp_path / "04 - Mr Brightside.mp3"
    audio.touch()
    assert _guess_title_artist(audio) == ("Mr Brightside", None)


def test_guess_title_artist_ignores_a_hyphen_without_spaces(tmp_path):
    audio = tmp_path / "Jump-Start.wav"
    audio.touch()
    assert _guess_title_artist(audio) == ("Jump-Start", None)


# --------------------------------------------------------------------------- #
# stem identification - the one that must not be got backwards
# --------------------------------------------------------------------------- #

def test_classify_reads_the_label_not_the_order():
    # audio-separator emits the instrumental first for some models. Order is
    # not a signal; the label is.
    outputs = [
        r"C:\work\song_(Instrumental)_model_bs_roformer_ep_317_sdr_12.9755.wav",
        r"C:\work\song_(Vocals)_model_bs_roformer_ep_317_sdr_12.9755.wav",
    ]
    vocals, instrumental = separate._classify(outputs)
    assert vocals is not None and "(Vocals)" in vocals.name
    assert instrumental is not None and "(Instrumental)" in instrumental.name


def test_classify_handles_the_no_vocals_label():
    outputs = ["song_(Vocals)_x.wav", "song_(No Vocals)_x.wav"]
    vocals, instrumental = separate._classify(outputs)
    assert vocals is not None and "(Vocals)" in vocals.name
    assert instrumental is not None and "No Vocals" in instrumental.name


def test_classify_reports_nothing_rather_than_guessing():
    vocals, instrumental = separate._classify(["stem_one.wav", "stem_two.wav"])
    assert vocals is None
    assert instrumental is None


@pytest.mark.parametrize("line, expected", [
    ("  42%|####      | 21/50", 42.0),
    ("Downloading model: 7.5%", 7.5),
    ("100%|##########|", 100.0),
    ("no percentage here", None),
    ("% leading", None),
    ("999%", None),
])
def test_percent_in_is_best_effort(line, expected):
    assert separate._percent_in(line) == expected


# --------------------------------------------------------------------------- #
# provenance - never clobber a real stem
# --------------------------------------------------------------------------- #

def _song_with_source(tmp_path, name: str = "source.mp3") -> SongPaths:
    song = SongPaths(tmp_path / "some-song").ensure_dirs()
    (song.root / name).write_bytes(b"pretend this is a recording")
    return song


def test_separate_refuses_to_overwrite_a_hand_supplied_stem(tmp_path):
    song = _song_with_source(tmp_path)
    song.vocals_wav.write_bytes(b"a real studio stem")

    result = separate.separate_song(song)

    assert result.skipped
    assert result.separator_id == "none"      # nothing was separated
    assert "supplied by hand" in result.reason
    assert song.vocals_wav.read_bytes() == b"a real studio stem"


def test_separate_skips_when_its_own_output_is_current(tmp_path):
    song = _song_with_source(tmp_path)
    song.vocals_wav.write_bytes(b"our separated stem")
    separate._write_provenance(
        song, model=separate.DEFAULT_MODEL,
        source_sha1=separate.sha1_of_file(song.source()),
        device="cuda", wrote_backing=True)

    result = separate.separate_song(song)

    assert result.skipped
    assert result.separator_id == separate.SEPARATOR_ID
    assert "already separated" in result.reason


def test_separate_reruns_when_the_source_changed(tmp_path, monkeypatch):
    song = _song_with_source(tmp_path)
    song.vocals_wav.write_bytes(b"stale separated stem")
    separate._write_provenance(song, model=separate.DEFAULT_MODEL,
                               source_sha1="0" * 40, device="cpu",
                               wrote_backing=False)

    # It must get as far as looking for the interpreter - that is the proof it
    # did not take a skip branch.
    monkeypatch.setattr(separate, "find_separator_python",
                        lambda *_a, **_k: (_ for _ in ()).throw(
                            separate.SeparationError("looked for the venv")))

    with pytest.raises(separate.SeparationError, match="looked for the venv"):
        separate.separate_song(song)


def test_separate_without_a_source_says_what_to_do(tmp_path):
    song = SongPaths(tmp_path / "empty-song").ensure_dirs()
    with pytest.raises(separate.SeparationError, match="add-song"):
        separate.separate_song(song)


def test_find_separator_python_error_names_the_fix(tmp_path, monkeypatch):
    monkeypatch.delenv("VOCALCHART_SEPARATOR_PYTHON", raising=False)
    monkeypatch.setattr(separate, "_candidate_pythons", lambda: iter(()))
    monkeypatch.setitem(__import__("sys").modules, "audio_separator", None)

    with pytest.raises(separate.SeparationError) as excinfo:
        separate.find_separator_python()
    message = str(excinfo.value)
    assert ".venv-separate" in message
    assert "audio-separator" in message


# --------------------------------------------------------------------------- #
# progress protocol
# --------------------------------------------------------------------------- #

def _events(capsys) -> list[dict]:
    out = capsys.readouterr().out
    return [json.loads(line) for line in out.splitlines() if line.strip()]


def test_reporter_percent_is_overall_and_monotonic(capsys):
    rep = Reporter(["separate", "f0", "notes", "lyrics", "chart"], json_mode=True)

    rep.stage("separate")
    rep.percent(50.0)
    rep.stage("f0")
    rep.percent(10.0)          # early in a later stage still beats mid-separation
    rep.stage("lyrics")
    rep.percent(100.0)
    rep.done(chart="x")

    percents = [e["percent"] for e in _events(capsys) if e["event"] == "progress"]
    assert percents == sorted(percents), "a bar that goes backwards reads as a bug"
    assert percents[-1] == 100.0

    # Separation dominates the run, so half of it must be a large share of the
    # whole - not one fifth, which is what per-stage-equal weighting would give.
    assert 20.0 < percents[1] < 35.0


def test_reporter_stays_silent_on_stdout_in_human_mode(capsys):
    rep = Reporter(["f0"], json_mode=False, verbose=True)
    rep.stage("f0")
    rep.log("something")
    rep.done(chart="x")

    captured = capsys.readouterr()
    assert captured.out == "", "stdout is the machine channel; humans get stderr"
    assert "something" in captured.err


def test_reporter_emits_a_done_event_with_the_chart_path(capsys):
    rep = Reporter(["chart"], json_mode=True)
    rep.done(songDir="d", chart="d/chart.json", notes=167)

    done = [e for e in _events(capsys) if e["event"] == "done"]
    assert len(done) == 1
    assert done[0]["chart"] == "d/chart.json"
    assert done[0]["notes"] == 167


def test_reporter_error_event_carries_the_message(capsys):
    rep = Reporter(["separate"], json_mode=True)
    rep.error("no separation environment found", "traceback here")

    err = [e for e in _events(capsys) if e["event"] == "error"]
    assert err and err[0]["message"] == "no separation environment found"
