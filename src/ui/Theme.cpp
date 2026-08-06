/*
  ==============================================================================

    Theme.cpp
    Vocal Trainer — M1

  ==============================================================================
*/

#include "Theme.h"

#include <cmath>

namespace vt
{

//==============================================================================
// Oklab
//==============================================================================

namespace
{
    inline float srgbToLinear (float c) noexcept
    {
        return c <= 0.04045f ? c / 12.92f
                             : std::pow ((c + 0.055f) / 1.055f, 2.4f);
    }

    inline float linearToSrgb (float c) noexcept
    {
        return c <= 0.0031308f ? c * 12.92f
                               : 1.055f * std::pow (c, 1.0f / 2.4f) - 0.055f;
    }

    struct Oklab { float L, a, b; };

    Oklab toOklab (juce::Colour c) noexcept
    {
        const float r = srgbToLinear ((float) c.getRed()   / 255.0f);
        const float g = srgbToLinear ((float) c.getGreen() / 255.0f);
        const float b = srgbToLinear ((float) c.getBlue()  / 255.0f);

        const float l = 0.4122214708f * r + 0.5363325363f * g + 0.0514459929f * b;
        const float m = 0.2119034982f * r + 0.6806995451f * g + 0.1073969566f * b;
        const float s = 0.0883024619f * r + 0.2817188376f * g + 0.6299787005f * b;

        const float l_ = std::cbrt (l);
        const float m_ = std::cbrt (m);
        const float s_ = std::cbrt (s);

        return { 0.2104542553f * l_ + 0.7936177850f * m_ - 0.0040720468f * s_,
                 1.9779984951f * l_ - 2.4285922050f * m_ + 0.4505937099f * s_,
                 0.0259040371f * l_ + 0.7827717662f * m_ - 0.8086757660f * s_ };
    }

    juce::Colour fromOklab (Oklab c, juce::uint8 alpha) noexcept
    {
        const float l_ = c.L + 0.3963377774f * c.a + 0.2158037573f * c.b;
        const float m_ = c.L - 0.1055613458f * c.a - 0.0638541728f * c.b;
        const float s_ = c.L - 0.0894841775f * c.a - 1.2914855480f * c.b;

        const float l = l_ * l_ * l_;
        const float m = m_ * m_ * m_;
        const float s = s_ * s_ * s_;

        const float r = linearToSrgb ( 4.0767416621f * l - 3.3077115913f * m + 0.2309699292f * s);
        const float g = linearToSrgb (-1.2684380046f * l + 2.6097574011f * m - 0.3413193965f * s);
        const float b = linearToSrgb (-0.0041960863f * l - 0.7034186147f * m + 1.7076147010f * s);

        const auto q = [] (float v) noexcept
        {
            return (juce::uint8) juce::jlimit (0, 255, juce::roundToInt (v * 255.0f));
        };

        return juce::Colour (q (r), q (g), q (b), alpha);
    }
}

juce::Colour lerpOklab (juce::Colour a, juce::Colour b, float t) noexcept
{
    t = juce::jlimit (0.0f, 1.0f, t);

    const auto ca = toOklab (a);
    const auto cb = toOklab (b);

    const Oklab mix { ca.L + (cb.L - ca.L) * t,
                      ca.a + (cb.a - ca.a) * t,
                      ca.b + (cb.b - ca.b) * t };

    const auto alpha = (juce::uint8) juce::roundToInt (juce::jlimit (0.0f, 255.0f,
                            (float) a.getAlpha() + ((float) b.getAlpha() - (float) a.getAlpha()) * t));

    return fromOklab (mix, alpha);
}

//==============================================================================
// JSON helpers — every one of these falls back rather than throwing.
//==============================================================================

namespace
{
    juce::var childObject (const juce::var& parent, const char* key)
    {
        if (parent.getDynamicObject() == nullptr)
            return {};

        return parent.getProperty (juce::Identifier (key), juce::var());
    }

    bool isNumber (const juce::var& v) noexcept
    {
        return v.isDouble() || v.isInt() || v.isInt64();
    }

    float readFloat (const juce::var& obj, const char* key, float fallback) noexcept
    {
        if (obj.getDynamicObject() == nullptr)
            return fallback;

        const auto v = obj.getProperty (juce::Identifier (key), juce::var());

        if (! isNumber (v))
            return fallback;

        const auto d = (double) v;

        if (! std::isfinite (d))
            return fallback;

        return (float) d;
    }

    int readInt (const juce::var& obj, const char* key, int fallback) noexcept
    {
        if (obj.getDynamicObject() == nullptr)
            return fallback;

        const auto v = obj.getProperty (juce::Identifier (key), juce::var());

        if (! isNumber (v))
            return fallback;

        const auto d = (double) v;

        if (! std::isfinite (d))
            return fallback;

        return juce::roundToInt (d);
    }

    bool readBool (const juce::var& obj, const char* key, bool fallback) noexcept
    {
        if (obj.getDynamicObject() == nullptr)
            return fallback;

        const auto v = obj.getProperty (juce::Identifier (key), juce::var());

        // Accept a real bool, and also 0/1 — a hand-edited theme.json is as
        // likely to contain "showScoredNotes": 0 as "false".
        if (v.isBool())
            return (bool) v;

        if (isNumber (v))
            return ((double) v) != 0.0;

        return fallback;
    }

    /** Accepts "#RRGGBB", "#AARRGGBB", "RRGGBB", "AARRGGBB". Anything else -> fallback. */
    juce::Colour readColour (const juce::var& obj, const char* key, juce::Colour fallback)
    {
        if (obj.getDynamicObject() == nullptr)
            return fallback;

        const auto v = obj.getProperty (juce::Identifier (key), juce::var());

        if (! v.isString())
            return fallback;

        auto s = v.toString().trim();

        if (s.startsWithChar ('#'))
            s = s.substring (1);

        if (! s.containsOnly ("0123456789abcdefABCDEF"))
            return fallback;

        if (s.length() == 6)
            return juce::Colour ((juce::uint32) (0xff000000u | (juce::uint32) s.getHexValue32()));

        if (s.length() == 8)
            return juce::Colour ((juce::uint32) s.getHexValue64());

        return fallback;
    }

    juce::String colourToString (juce::Colour c)
    {
        return "#" + juce::String::toHexString ((int) (c.getARGB() & 0x00ffffffu)).paddedLeft ('0', 6).toUpperCase();
    }

    // Note the explicit overloads: passing a float straight to juce::var is
    // ambiguous (float -> double / int / bool are all standard conversions).
    void setProp (juce::DynamicObject::Ptr o, const char* key, const juce::String& value)
    {
        o->setProperty (juce::Identifier (key), juce::var (value));
    }

    void setProp (juce::DynamicObject::Ptr o, const char* key, int value)
    {
        o->setProperty (juce::Identifier (key), juce::var (value));
    }

    void setProp (juce::DynamicObject::Ptr o, const char* key, float value)
    {
        o->setProperty (juce::Identifier (key), juce::var ((double) value));
    }

    void setProp (juce::DynamicObject::Ptr o, const char* key, bool value)
    {
        o->setProperty (juce::Identifier (key), juce::var (value));
    }

    void setProp (juce::DynamicObject::Ptr o, const char* key, const juce::var& value)
    {
        o->setProperty (juce::Identifier (key), value);
    }
}

//==============================================================================
// Theme
//==============================================================================

Theme::Theme()
{
    const auto file = getThemeFile();

    if (! file.existsAsFile())
        writeDefaultsToFile();

    reloadNow();

    seenModTime = file.getLastModificationTime();
    seenSize    = file.getSize();

    startTimerHz (kPollHz);
}

Theme::~Theme()
{
    stopTimer();
}

juce::File Theme::getThemeFile()
{
    // On Windows this is %APPDATA% (Roaming).
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
              .getChildFile ("VocalTrainer")
              .getChildFile ("theme.json");
}

bool Theme::writeDefaultsToFile() const
{
    const ThemeValues d;

    juce::DynamicObject::Ptr colours (new juce::DynamicObject());
    setProp (colours, "background",        colourToString (d.background));
    setProp (colours, "panel",             colourToString (d.panel));
    setProp (colours, "gridLine",          colourToString (d.gridLine));
    setProp (colours, "gridLineOctave",    colourToString (d.gridLineOctave));
    setProp (colours, "gridLabel",         colourToString (d.gridLabel));
    setProp (colours, "targetBarActive",   colourToString (d.targetBarActive));
    setProp (colours, "targetBarUpcoming", colourToString (d.targetBarUpcoming));
    setProp (colours, "goldenNote",        colourToString (d.goldenNote));
    setProp (colours, "traceInTune",       colourToString (d.traceInTune));
    setProp (colours, "traceOutOfTune",    colourToString (d.traceOutOfTune));
    setProp (colours, "ghostContour",      colourToString (d.ghostContour));
    setProp (colours, "lyricSung",         colourToString (d.lyricSung));
    setProp (colours, "lyricPending",      colourToString (d.lyricPending));
    setProp (colours, "lyricActiveWipe",   colourToString (d.lyricActiveWipe));
    setProp (colours, "text",              colourToString (d.text));
    setProp (colours, "textDim",           colourToString (d.textDim));
    setProp (colours, "nowLine",           colourToString (d.nowLine));
    setProp (colours, "noteScoredGood",    colourToString (d.noteScoredGood));
    setProp (colours, "noteScoredPoor",    colourToString (d.noteScoredPoor));
    setProp (colours, "noteNotSung",       colourToString (d.noteNotSung));
    setProp (colours, "editSelection",     colourToString (d.editSelection));

    juce::DynamicObject::Ptr lane (new juce::DynamicObject());
    setProp (lane, "headerHeight",          d.headerHeight);
    setProp (lane, "controlsHeight",        d.controlsHeight);
    setProp (lane, "controlsRow2Height",    d.controlsRow2Height);
    setProp (lane, "footerHeight",          d.footerHeight);
    setProp (lane, "semitonesVisible",      d.semitonesVisible);
    setProp (lane, "historySeconds",        d.historySeconds);
    setProp (lane, "nowLineFraction",       d.nowLineFraction);
    setProp (lane, "gridLineThicknessPx",   d.gridLineThicknessPx);
    setProp (lane, "gridLineOctaveThickPx", d.gridLineOctaveThickPx);
    setProp (lane, "gridLabelHeightPx",     d.gridLabelHeightPx);
    setProp (lane, "noteBarCornerRadiusPx", d.noteBarCornerRadiusPx);
    setProp (lane, "targetBarBorderPx",     d.targetBarBorderPx);
    setProp (lane, "upcomingBarAlpha",      d.upcomingBarAlpha);
    setProp (lane, "haloAlpha",             d.haloAlpha);

    juce::DynamicObject::Ptr highway (new juce::DynamicObject());
    setProp (highway, "historySeconds",         d.highwayHistorySeconds);
    setProp (highway, "lookaheadSeconds",       d.highwayLookaheadSeconds);
    setProp (highway, "minSemitonesVisible",    d.minSemitonesVisible);
    setProp (highway, "zoomPaddingSemitones",   d.zoomPaddingSemitones);
    setProp (highway, "zoomHorizonInnerSeconds", d.zoomHorizonInnerSeconds);
    setProp (highway, "zoomHorizonOuterSeconds", d.zoomHorizonOuterSeconds);
    setProp (highway, "zoomSmoothingHz",        d.zoomSmoothingHz);
    setProp (highway, "ghostContourAlpha",      d.ghostContourAlpha);
    setProp (highway, "ghostContourThicknessPx", d.ghostContourThicknessPx);
    setProp (highway, "scoredBarAlpha",          d.scoredBarAlpha);
    setProp (highway, "showScoredNotes",         d.showScoredNotes);

    juce::DynamicObject::Ptr trace (new juce::DynamicObject());
    setProp (trace, "trailMs",              d.trailMs);
    setProp (trace, "ringMs",               d.ringMs);
    setProp (trace, "trailThicknessPx",     d.trailThicknessPx);
    setProp (trace, "trailStartAlpha",      d.trailStartAlpha);
    setProp (trace, "trailAlphaBands",      d.trailAlphaBands);
    setProp (trace, "traceBreakSemitones",  d.traceBreakSemitones);
    setProp (trace, "dotDiameterPx",        d.dotDiameterPx);
    setProp (trace, "dotRingPx",            d.dotRingPx);

    juce::DynamicObject::Ptr tol (new juce::DynamicObject());
    setProp (tol, "tFullCents",             d.tFullCents);
    setProp (tol, "tZeroCents",             d.tZeroCents);
    setProp (tol, "guideVocalDb",       d.guideVocalDb);
    setProp (tol, "clarityThreshold",       d.clarityThreshold);
    setProp (tol, "colourMedianMs",         d.colourMedianMs);

    juce::DynamicObject::Ptr smoothing (new juce::DynamicObject());
    setProp (smoothing, "oneEuroMinCutoffHz",   d.oneEuroMinCutoffHz);
    setProp (smoothing, "oneEuroBeta",          d.oneEuroBeta);
    setProp (smoothing, "oneEuroDerivCutoffHz", d.oneEuroDerivCutoffHz);
    setProp (smoothing, "freeSingRecentreSemis", d.freeSingRecentreSemis);

    juce::DynamicObject::Ptr root (new juce::DynamicObject());
    setProp (root, "_comment", juce::String ("Vocal Trainer theme. Saved changes are picked up within 250 ms; "
                                             "no rebuild needed. Delete this file to restore defaults. "
                                             "Colours are #RRGGBB or #AARRGGBB."));
    setProp (root, "colours",   juce::var (colours.get()));
    setProp (root, "lane",      juce::var (lane.get()));
    setProp (root, "highway",   juce::var (highway.get()));
    setProp (root, "trace",     juce::var (trace.get()));
    setProp (root, "tolerance", juce::var (tol.get()));
    setProp (root, "smoothing", juce::var (smoothing.get()));

    const auto file = getThemeFile();

    if (! file.getParentDirectory().createDirectory().wasOk())
        return false;

    return file.replaceWithText (juce::JSON::toString (juce::var (root.get()), false, 6));
}

bool Theme::reloadNow()
{
    const auto file = getThemeFile();

    if (! file.existsAsFile())
    {
        // No file at all: keep the compiled-in defaults. Do not fight the user by
        // rewriting it here — they may have deliberately deleted it while running.
        parseError = false;
        return false;
    }

    return loadFrom (file);
}

bool Theme::loadFrom (const juce::File& file)
{
    const auto root = juce::JSON::parse (file);

    if (root.getDynamicObject() == nullptr)
    {
        // Malformed or half-written. Keep the values we already have.
        parseError = true;
        return false;
    }

    const ThemeValues d;                 // defaults, used for every missing key
    ThemeValues v;

    const auto colours   = childObject (root, "colours");
    const auto lane      = childObject (root, "lane");
    const auto highway   = childObject (root, "highway");
    const auto trace     = childObject (root, "trace");
    const auto tol       = childObject (root, "tolerance");
    const auto smoothing = childObject (root, "smoothing");

    v.background        = readColour (colours, "background",        d.background);
    v.panel             = readColour (colours, "panel",             d.panel);
    v.gridLine          = readColour (colours, "gridLine",          d.gridLine);
    v.gridLineOctave    = readColour (colours, "gridLineOctave",    d.gridLineOctave);
    v.gridLabel         = readColour (colours, "gridLabel",         d.gridLabel);
    v.targetBarActive   = readColour (colours, "targetBarActive",   d.targetBarActive);
    v.targetBarUpcoming = readColour (colours, "targetBarUpcoming", d.targetBarUpcoming);
    v.goldenNote        = readColour (colours, "goldenNote",        d.goldenNote);
    v.traceInTune       = readColour (colours, "traceInTune",       d.traceInTune);
    v.traceOutOfTune    = readColour (colours, "traceOutOfTune",    d.traceOutOfTune);
    v.ghostContour      = readColour (colours, "ghostContour",      d.ghostContour);
    v.lyricSung         = readColour (colours, "lyricSung",         d.lyricSung);
    v.lyricPending      = readColour (colours, "lyricPending",      d.lyricPending);
    v.lyricActiveWipe   = readColour (colours, "lyricActiveWipe",   d.lyricActiveWipe);
    v.text              = readColour (colours, "text",              d.text);
    v.textDim           = readColour (colours, "textDim",           d.textDim);
    v.nowLine           = readColour (colours, "nowLine",           d.nowLine);
    v.noteScoredGood    = readColour (colours, "noteScoredGood",    d.noteScoredGood);
    v.noteScoredPoor    = readColour (colours, "noteScoredPoor",    d.noteScoredPoor);
    v.noteNotSung       = readColour (colours, "noteNotSung",       d.noteNotSung);
    v.editSelection     = readColour (colours, "editSelection",     d.editSelection);

    v.headerHeight          = juce::jlimit (0,     200,    readInt   (lane, "headerHeight",          d.headerHeight));
    v.controlsHeight        = juce::jlimit (0,     200,    readInt   (lane, "controlsHeight",        d.controlsHeight));
    v.controlsRow2Height    = juce::jlimit (0,     200,    readInt   (lane, "controlsRow2Height",    d.controlsRow2Height));
    v.footerHeight          = juce::jlimit (0,     200,    readInt   (lane, "footerHeight",          d.footerHeight));
    v.semitonesVisible      = juce::jlimit (2.0f,  60.0f,  readFloat (lane, "semitonesVisible",      d.semitonesVisible));
    v.historySeconds        = juce::jlimit (0.05f, 10.0f,  readFloat (lane, "historySeconds",        d.historySeconds));
    v.nowLineFraction       = juce::jlimit (0.05f, 1.0f,   readFloat (lane, "nowLineFraction",       d.nowLineFraction));
    v.gridLineThicknessPx   = juce::jlimit (0.25f, 8.0f,   readFloat (lane, "gridLineThicknessPx",   d.gridLineThicknessPx));
    v.gridLineOctaveThickPx = juce::jlimit (0.25f, 8.0f,   readFloat (lane, "gridLineOctaveThickPx", d.gridLineOctaveThickPx));
    v.gridLabelHeightPx     = juce::jlimit (6.0f,  40.0f,  readFloat (lane, "gridLabelHeightPx",     d.gridLabelHeightPx));
    v.noteBarCornerRadiusPx = juce::jlimit (0.0f,  40.0f,  readFloat (lane, "noteBarCornerRadiusPx", d.noteBarCornerRadiusPx));
    v.targetBarBorderPx     = juce::jlimit (0.0f,  8.0f,   readFloat (lane, "targetBarBorderPx",     d.targetBarBorderPx));
    v.upcomingBarAlpha      = juce::jlimit (0.0f,  1.0f,   readFloat (lane, "upcomingBarAlpha",      d.upcomingBarAlpha));
    v.haloAlpha             = juce::jlimit (0.0f,  1.0f,   readFloat (lane, "haloAlpha",             d.haloAlpha));

    v.highwayHistorySeconds    = juce::jlimit (0.25f, 10.0f, readFloat (highway, "historySeconds",          d.highwayHistorySeconds));
    v.highwayLookaheadSeconds  = juce::jlimit (0.5f,  20.0f, readFloat (highway, "lookaheadSeconds",        d.highwayLookaheadSeconds));
    v.minSemitonesVisible      = juce::jlimit (4.0f,  60.0f, readFloat (highway, "minSemitonesVisible",     d.minSemitonesVisible));
    v.zoomPaddingSemitones     = juce::jlimit (0.0f,  24.0f, readFloat (highway, "zoomPaddingSemitones",    d.zoomPaddingSemitones));
    v.zoomHorizonInnerSeconds  = juce::jlimit (1.0f,  60.0f, readFloat (highway, "zoomHorizonInnerSeconds", d.zoomHorizonInnerSeconds));
    v.zoomHorizonOuterSeconds  = juce::jlimit (v.zoomHorizonInnerSeconds, 120.0f,
                                                             readFloat (highway, "zoomHorizonOuterSeconds", d.zoomHorizonOuterSeconds));
    v.zoomSmoothingHz          = juce::jlimit (0.05f, 30.0f, readFloat (highway, "zoomSmoothingHz",         d.zoomSmoothingHz));
    v.ghostContourAlpha        = juce::jlimit (0.0f,  1.0f,  readFloat (highway, "ghostContourAlpha",       d.ghostContourAlpha));
    v.ghostContourThicknessPx  = juce::jlimit (0.25f, 8.0f,  readFloat (highway, "ghostContourThicknessPx", d.ghostContourThicknessPx));
    v.scoredBarAlpha           = juce::jlimit (0.0f,  1.0f,  readFloat (highway, "scoredBarAlpha",          d.scoredBarAlpha));
    v.showScoredNotes          =                             readBool  (highway, "showScoredNotes",         d.showScoredNotes);

    v.trailMs               = juce::jlimit (50,    5000,   readInt   (trace, "trailMs",              d.trailMs));
    v.ringMs                = juce::jlimit (v.trailMs, 20000, readInt (trace, "ringMs",              d.ringMs));
    v.trailThicknessPx      = juce::jlimit (0.5f,  16.0f,  readFloat (trace, "trailThicknessPx",     d.trailThicknessPx));
    v.trailStartAlpha       = juce::jlimit (0.0f,  1.0f,   readFloat (trace, "trailStartAlpha",      d.trailStartAlpha));
    v.trailAlphaBands       = juce::jlimit (1,     32,     readInt   (trace, "trailAlphaBands",      d.trailAlphaBands));
    v.traceBreakSemitones   = juce::jlimit (0.05f, 12.0f,  readFloat (trace, "traceBreakSemitones",  d.traceBreakSemitones));
    v.dotDiameterPx         = juce::jlimit (1.0f,  60.0f,  readFloat (trace, "dotDiameterPx",        d.dotDiameterPx));
    v.dotRingPx             = juce::jlimit (0.0f,  16.0f,  readFloat (trace, "dotRingPx",            d.dotRingPx));

    v.tFullCents            = juce::jlimit (1.0f,  200.0f, readFloat (tol, "tFullCents",             d.tFullCents));
    v.tZeroCents            = juce::jlimit (v.tFullCents + 1.0f, 600.0f,
                                                           readFloat (tol, "tZeroCents",             d.tZeroCents));
    v.clarityThreshold      = juce::jlimit (0.0f,  1.0f,   readFloat (tol, "clarityThreshold",       d.clarityThreshold));
    v.colourMedianMs        = juce::jlimit (0,     2000,   readInt   (tol, "colourMedianMs",         d.colourMedianMs));

    v.oneEuroMinCutoffHz    = juce::jlimit (0.01f, 50.0f,  readFloat (smoothing, "oneEuroMinCutoffHz",    d.oneEuroMinCutoffHz));
    v.oneEuroBeta           = juce::jlimit (0.0f,  20.0f,  readFloat (smoothing, "oneEuroBeta",           d.oneEuroBeta));
    v.oneEuroDerivCutoffHz  = juce::jlimit (0.01f, 50.0f,  readFloat (smoothing, "oneEuroDerivCutoffHz",  d.oneEuroDerivCutoffHz));
    v.freeSingRecentreSemis = juce::jlimit (0.5f,  24.0f,  readFloat (smoothing, "freeSingRecentreSemis", d.freeSingRecentreSemis));

    values = v;
    parseError = false;
    return true;
}

void Theme::timerCallback()
{
    const auto file = getThemeFile();

    if (! file.existsAsFile())
    {
        changePending = false;
        return;
    }

    const auto modTime = file.getLastModificationTime();
    const auto size    = file.getSize();

    if (modTime != seenModTime || size != seenSize)
    {
        // Stamps moved. Wait one more tick (250 ms) before reading so that an
        // editor which truncates-then-writes cannot be caught mid-save.
        seenModTime = modTime;
        seenSize    = size;
        changePending = true;
        return;
    }

    if (! changePending)
        return;

    changePending = false;

    if (loadFrom (file))
        sendChangeMessage();
}

} // namespace vt
