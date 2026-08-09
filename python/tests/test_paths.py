"""Cache-identity and atomic-write helpers."""

import json

from vocalchart.paths import atomic_write_text, file_fingerprint


def test_file_fingerprint_survives_a_json_round_trip(tmp_path):
    p = tmp_path / "a.txt"
    p.write_text("hi", encoding="utf-8")

    fp = file_fingerprint(p)

    # The fingerprint is stored inside cache payloads and compared by equality
    # after a JSON round trip — so it must come back EXACTLY.
    assert json.loads(json.dumps(fp)) == fp
    assert fp["bytes"] == 2


def test_file_fingerprint_of_missing_file_is_none(tmp_path):
    assert file_fingerprint(tmp_path / "missing.txt") is None


def test_file_fingerprint_changes_when_content_is_replaced(tmp_path):
    p = tmp_path / "a.txt"
    p.write_text("one", encoding="utf-8")
    before = file_fingerprint(p)

    p.write_text("two three", encoding="utf-8")
    assert file_fingerprint(p) != before


def test_atomic_write_replaces_and_leaves_no_temp(tmp_path):
    p = tmp_path / "x.json"

    atomic_write_text(p, "one")
    atomic_write_text(p, "two")

    assert p.read_text(encoding="utf-8") == "two"
    assert not (tmp_path / "x.json.tmp").exists()


def test_atomic_write_creates_parent_directories(tmp_path):
    p = tmp_path / "deep" / "er" / "x.txt"
    atomic_write_text(p, "hello")
    assert p.read_text(encoding="utf-8") == "hello"
