/*
    ==========================================================================

    ChartWriter.h — saves an edited Chart back to chart.json.

    THE ONE RULE: NEVER LOSE A FIELD THIS BUILD DOES NOT READ.

    Chart::parse() is deliberately tolerant and ignores what it does not
    understand. A naive writer that serialised only the fields the C++ side
    parses would therefore DELETE everything else on the first save — the
    reference-audio SHA-1, the generator's version and settings, per-note
    provenance, and any block a future pipeline version adds. The user would
    correct one mis-transcribed word and silently lose the metadata that lets
    the offline pipeline know the cache is still valid.

    So the writer re-reads the original chart.json at save time and merges: the
    blocks it owns (notes, phrases, and generator.lyricSource) are rewritten,
    and every other key at every level is passed through byte-for-byte. The
    contour in particular is passed through untouched — it is the largest thing
    in the file and nothing here has any business regenerating it.

    A .bak of the previous file is written first. Hand-editing a chart is
    exactly the situation where a single mis-click wants an undo, and a copy on
    disk is a more dependable undo than anything held in memory.

    THREADING: message thread. It does file I/O and is driven by a button.

    ==========================================================================
*/

#pragma once

#include <juce_core/juce_core.h>

#include "Chart.h"

namespace vt
{

class ChartWriter
{
public:
    /** Builds the JSON that would be written.

        `originalJson` is the previously-parsed file to merge unknown fields
        from; pass a void var for a chart that never came from a file (a
        generated exercise), in which case only the known schema is emitted. */
    static juce::var toJson (const Chart&, const juce::var& originalJson);

    /** Saves to `target`, or to the chart's own source file when `target` is
        invalid. Writes `<target>.bak` first when the target already exists.

        Returns false with `error` filled. Failure leaves the existing file
        alone — the temporary-then-rename dance is what juce::File::replaceWith
        does internally, so a full disk cannot truncate a good chart. */
    static bool save (const Chart&, const juce::File& target, juce::String& error);

private:
    static juce::var readOriginal (const juce::File&);
    static juce::String noteTypeToString (NoteType);
    static juce::String syllableRoleToString (SyllableRole);

    /** Copies every property of `from` into `into` that `into` does not already
        have — i.e. the pass-through half of the merge. */
    static void mergeUnknownProperties (juce::DynamicObject& into, const juce::var& from);
};

} // namespace vt
