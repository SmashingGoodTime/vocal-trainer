"""Lyric tokenisation and note assignment — pure-function coverage.

These exist because the subtlest lyric bugs found in review all lived in code
no test touched: the dead tolerance parameter (every word attached to SOME
note, however far away), the unreachable melisma branch, and hyphenated
compounds losing their tail during normalisation.
"""

from dataclasses import dataclass

from vocalchart.lyrics import (Word, _tokens, assign_syllables_to_notes,
                               syllabify)


@dataclass
class _N:
    """Duck-typed note: assign_syllables_to_notes touches only these fields."""
    startSeconds: float
    durationSeconds: float
    syllable: object = None
    syllableRole: str = "none"


# --------------------------------------------------------------------------- #
# tokenisation
# --------------------------------------------------------------------------- #

def test_tokens_keeps_hyphenated_tail():
    assert _tokens("twenty-one") == ["twenty", "one"]


def test_tokens_normalises_curly_apostrophe():
    assert _tokens("Don’t stop") == ["don't", "stop"]


def test_syllabify_hyphenated_compound_keeps_both_parts():
    parts = syllabify("twenty-one")
    assert "one" in parts, "the tail of a hyphenated compound must not vanish"


# --------------------------------------------------------------------------- #
# word -> note assignment
# --------------------------------------------------------------------------- #

def test_word_overlapping_a_note_is_assigned():
    notes = [_N(0.0, 0.5)]
    assign_syllables_to_notes(notes, [Word("hello", 0.05, 0.4)])
    assert notes[0].syllable is not None


def test_far_word_is_left_unassigned():
    # A hallucinated or drifted word far from every note must NOT be slapped
    # onto the nearest one — that is how a word lands in the wrong phrase.
    notes = [_N(0.0, 0.5), _N(1.0, 0.5)]
    words = [Word("hello", 0.05, 0.4), Word("ghost", 30.0, 30.3)]

    assign_syllables_to_notes(notes, words)

    joined = " ".join(n.syllable for n in notes if n.syllable)
    assert "ghost" not in joined


def test_word_within_tolerance_of_a_note_still_attaches():
    notes = [_N(1.0, 0.5)]
    # 50 ms before the note starts — inside the 80 ms tolerance.
    assign_syllables_to_notes(notes, [Word("hey", 0.55, 0.95)])
    assert notes[0].syllable is not None


def test_held_one_syllable_word_marks_melisma():
    # One syllable sung across two notes: the trailing note carries no text and
    # role "melisma", so the renderer draws a tie instead of repeating the word.
    # This branch used to be unreachable — the pipeline never emitted a melisma.
    notes = [_N(0.0, 0.4), _N(0.45, 0.4), _N(2.0, 0.4)]
    words = [Word("grace", 0.0, 0.9), Word("how", 2.0, 2.35)]

    assign_syllables_to_notes(notes, words)

    assert notes[0].syllable == "grace"
    assert notes[0].syllableRole == "wordStart"
    assert notes[1].syllable is None
    assert notes[1].syllableRole == "melisma"
    assert notes[2].syllable == "how"


def test_multi_syllable_word_spreads_in_order():
    notes = [_N(0.0, 0.3), _N(0.35, 0.3)]
    assign_syllables_to_notes(notes, [Word("hello", 0.0, 0.6)])

    assert notes[0].syllableRole == "wordStart"
    assert notes[1].syllableRole == "continuation"
    assert (notes[0].syllable + notes[1].syllable).lower() == "hello"
