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
// Background style <-> string
//==============================================================================

BackgroundStyle backgroundStyleFromString (const juce::String& s) noexcept
{
    const auto t = s.trim().toLowerCase();

    if (t == "staff")          return BackgroundStyle::staff;
    if (t == "staffandgrid")   return BackgroundStyle::staffAndGrid;
    if (t == "plain")          return BackgroundStyle::plain;

    return BackgroundStyle::chromatic;
}

const char* backgroundStyleToString (BackgroundStyle s) noexcept
{
    switch (s)
    {
        case BackgroundStyle::staff:        return "staff";
        case BackgroundStyle::staffAndGrid: return "staffAndGrid";
        case BackgroundStyle::plain:        return "plain";
        case BackgroundStyle::chromatic:    break;
    }

    return "chromatic";
}

//==============================================================================
// Presets
//==============================================================================
//
// A preset is a complete ThemeValues, not a patch over the defaults, so reading
// one tells you exactly what you get. They are data and nothing branches on
// which one is active.
//
// ONLY "Midnight" AND "Sheet Music" CARRY THE COLOUR-VISION GUARANTEE.
// DESIGN.md §7 picks the Okabe-Ito palette because vermilion #D55E00 against
// bluish green #009E73 stays separable under all three common dichromacies, and
// those two presets keep that pair. "Neon" deliberately does not — it is a
// preference, and the geometry (inside the bar / outside the bar) still carries
// the signal on its own, which is why hue was never allowed to be load-bearing.
//
//==============================================================================

namespace
{
    ThemeValues makeSheetMusic()
    {
        ThemeValues v;

        v.presetName = "Sheet Music";
        v.backgroundStyle = BackgroundStyle::staff;

        v.background        = juce::Colour (0xfff7f3e8);   // warm paper
        v.panel             = juce::Colour (0xffeae4d4);
        v.gridLine          = juce::Colour (0xffd6ceb8);
        v.gridLineOctave    = juce::Colour (0xffc3b99e);
        v.gridLabel         = juce::Colour (0xff7a7263);
        v.staffLine         = juce::Colour (0xff2e2a24);   // ink
        v.staffLedger       = juce::Colour (0xff8a8272);
        v.targetBarActive   = juce::Colour (0xff1f6fb2);
        v.targetBarUpcoming = juce::Colour (0xff5e8cb5);
        v.goldenNote        = juce::Colour (0xffb77a00);
        v.traceInTune       = juce::Colour (0xff00734f);
        v.traceOutOfTune    = juce::Colour (0xffb8460a);
        v.ghostContour      = juce::Colour (0xff6e6656);

        // #F0E442 is a lyric colour chosen against #101014. On paper it is
        // barely there, so the light presets re-pick it rather than inherit it.
        v.lyricSung         = juce::Colour (0xff8a6a00);
        v.lyricPending      = juce::Colour (0xff8a8272);
        v.lyricActiveWipe   = juce::Colour (0xff1a1712);

        v.text              = juce::Colour (0xff241f18);
        v.textDim           = juce::Colour (0xff6e6656);
        v.nowLine           = juce::Colour (0xffb03a2e);
        v.noteScoredGood    = juce::Colour (0xff00734f);
        v.noteScoredPoor    = juce::Colour (0xffb8460a);
        v.noteNotSung       = juce::Colour (0xffb5ad9b);
        v.editSelection     = juce::Colour (0xffb77a00);

        v.pianoWhiteKey     = juce::Colour (0xfffffdf6);
        v.pianoBlackKey     = juce::Colour (0xff2e2a24);
        v.pianoKeyBorder    = juce::Colour (0xffb9b09a);
        v.pianoLabel        = juce::Colour (0xff5a5347);

        v.transportTrack    = juce::Colour (0xffe4ddca);
        v.transportDensity  = juce::Colour (0xffb6ac93);
        v.transportPlayhead = juce::Colour (0xffb03a2e);
        v.loopRegion        = juce::Colour (0xff1f6fb2);

        // Alphas tuned for a LIGHT background. 35% of a mid blue over #101014
        // is a visible bar; the same 35% over paper is a smudge.
        v.upcomingBarAlpha    = 0.50f;
        v.haloAlpha           = 0.16f;
        v.ghostContourAlpha   = 0.35f;
        v.trailStartAlpha     = 0.70f;
        v.loopRegionAlpha     = 0.20f;

        return v;
    }

    ThemeValues makeStaffDark()
    {
        ThemeValues v;                       // the Midnight palette, unchanged

        v.presetName = "Staff Dark";
        v.backgroundStyle = BackgroundStyle::staffAndGrid;
        v.staffLine   = juce::Colour (0xff7e7e92);
        v.staffLedger = juce::Colour (0xff4a4a58);

        return v;
    }

    ThemeValues makeHighContrast()
    {
        ThemeValues v;

        v.presetName = "High Contrast";

        v.background        = juce::Colour (0xff000000);
        v.panel             = juce::Colour (0xff0a0a0a);
        v.gridLine          = juce::Colour (0xff3c3c3c);
        v.gridLineOctave    = juce::Colour (0xff6e6e6e);
        v.gridLabel         = juce::Colour (0xffd0d0d0);
        v.staffLine         = juce::Colour (0xffb0b0b0);
        v.staffLedger       = juce::Colour (0xff6e6e6e);
        v.targetBarActive   = juce::Colour (0xff00b7ff);
        v.targetBarUpcoming = juce::Colour (0xff0072b2);
        v.traceInTune       = juce::Colour (0xff00e58c);
        v.traceOutOfTune    = juce::Colour (0xffff6a00);
        v.noteScoredGood    = juce::Colour (0xff00e58c);
        v.noteScoredPoor    = juce::Colour (0xffff6a00);
        v.noteNotSung       = juce::Colour (0xff5a5a5a);
        v.text              = juce::Colour (0xffffffff);
        v.textDim           = juce::Colour (0xffc0c0c0);
        v.nowLine           = juce::Colour (0xffffffff);

        // The ONE preset that keeps a photographic keyboard. Everywhere else the
        // keys are toned down to sit inside the theme; here contrast IS the
        // theme, and a low-vision user reading a reference needs the maximum
        // separation the display can give.
        v.pianoWhiteKey     = juce::Colour (0xffffffff);
        v.pianoBlackKey     = juce::Colour (0xff000000);
        v.pianoKeyBorder    = juce::Colour (0xff6e6e6e);
        v.pianoLabel        = juce::Colour (0xff000000);

        v.transportTrack    = juce::Colour (0xff141414);
        v.transportDensity  = juce::Colour (0xff5a5a5a);

        // Everything thicker and larger. Low vision is the case this exists for,
        // and a 1 px grid line at 25% is exactly what it cannot resolve.
        v.gridLineThicknessPx   = 1.6f;
        v.gridLineOctaveThickPx = 2.4f;
        v.gridLabelHeightPx     = 13.0f;
        v.staffLineThicknessPx  = 2.0f;
        v.trailThicknessPx      = 3.0f;
        v.trailStartAlpha       = 0.85f;
        v.dotDiameterPx         = 9.0f;
        v.upcomingBarAlpha      = 0.50f;
        v.haloAlpha             = 0.18f;
        v.noteNameHeightPx      = 12.0f;

        return v;
    }

    ThemeValues makeNeon()
    {
        ThemeValues v;

        v.presetName = "Neon";

        v.background        = juce::Colour (0xff0b0714);
        v.panel             = juce::Colour (0xff150e24);
        v.gridLine          = juce::Colour (0xff2a1e44);
        v.gridLineOctave    = juce::Colour (0xff4a3570);
        v.gridLabel         = juce::Colour (0xff8a7acf);
        v.staffLine         = juce::Colour (0xff6a55a0);
        v.staffLedger       = juce::Colour (0xff3a2e58);
        v.targetBarActive   = juce::Colour (0xff00e5ff);
        v.targetBarUpcoming = juce::Colour (0xff7a4dff);
        v.goldenNote        = juce::Colour (0xffffc400);
        v.traceInTune       = juce::Colour (0xff39ff9e);
        v.traceOutOfTune    = juce::Colour (0xffff3d81);
        v.ghostContour      = juce::Colour (0xff9a8acf);
        v.lyricSung         = juce::Colour (0xffffe94a);
        v.lyricPending      = juce::Colour (0xff7a6ba8);
        v.text              = juce::Colour (0xffede7ff);
        v.textDim           = juce::Colour (0xff9a8acf);
        v.nowLine           = juce::Colour (0xffff3d81);
        v.noteScoredGood    = juce::Colour (0xff39ff9e);
        v.noteScoredPoor    = juce::Colour (0xffff3d81);
        v.noteNotSung       = juce::Colour (0xff3a2e58);
        v.editSelection     = juce::Colour (0xffffe94a);

        v.pianoWhiteKey     = juce::Colour (0xff241a3c);
        v.pianoBlackKey     = juce::Colour (0xff0a0614);
        v.pianoKeyBorder    = juce::Colour (0xff3a2e58);
        v.pianoLabel        = juce::Colour (0xff9a8acf);

        v.transportTrack    = juce::Colour (0xff150e24);
        v.transportDensity  = juce::Colour (0xff4a3570);
        v.transportPlayhead = juce::Colour (0xffffe94a);
        v.loopRegion        = juce::Colour (0xff00e5ff);

        v.trailStartAlpha   = 0.75f;

        return v;
    }
}

juce::StringArray Theme::getPresetNames()
{
    return { "Midnight", "Sheet Music", "Staff Dark", "High Contrast", "Neon" };
}

ThemeValues Theme::getPreset (const juce::String& name)
{
    if (name == "Sheet Music")   return makeSheetMusic();
    if (name == "Staff Dark")    return makeStaffDark();
    if (name == "High Contrast") return makeHighContrast();
    if (name == "Neon")          return makeNeon();

    return {};     // "Midnight" — the compiled-in DESIGN.md values
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

    juce::String readString (const juce::var& obj, const char* key, const juce::String& fallback)
    {
        if (obj.getDynamicObject() == nullptr)
            return fallback;

        const auto v = obj.getProperty (juce::Identifier (key), juce::var());

        return v.isString() ? v.toString().trim() : fallback;
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
    return writeValuesToFile (ThemeValues {});
}

bool Theme::writeValuesToFile (const ThemeValues& d)
{
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
    setProp (colours, "staffLine",         colourToString (d.staffLine));
    setProp (colours, "staffLedger",       colourToString (d.staffLedger));
    setProp (colours, "pianoWhiteKey",     colourToString (d.pianoWhiteKey));
    setProp (colours, "pianoBlackKey",     colourToString (d.pianoBlackKey));
    setProp (colours, "pianoKeyBorder",    colourToString (d.pianoKeyBorder));
    setProp (colours, "pianoLabel",        colourToString (d.pianoLabel));
    setProp (colours, "transportTrack",    colourToString (d.transportTrack));
    setProp (colours, "transportDensity",  colourToString (d.transportDensity));
    setProp (colours, "transportPlayhead", colourToString (d.transportPlayhead));
    setProp (colours, "loopRegion",        colourToString (d.loopRegion));

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
    setProp (highway, "backgroundStyle",         juce::String (backgroundStyleToString (d.backgroundStyle)));
    setProp (highway, "staffLineThicknessPx",    d.staffLineThicknessPx);
    setProp (highway, "staffLedgerThicknessPx",  d.staffLedgerThicknessPx);
    setProp (highway, "staffGridAlpha",          d.staffGridAlpha);
    setProp (highway, "showNoteNames",           d.showNoteNames);
    setProp (highway, "noteNameShowOctave",      d.noteNameShowOctave);
    setProp (highway, "noteNameHeightPx",        d.noteNameHeightPx);
    setProp (highway, "noteNameMinBarWidthPx",   d.noteNameMinBarWidthPx);
    setProp (highway, "noteNameAlpha",           d.noteNameAlpha);

    juce::DynamicObject::Ptr piano (new juce::DynamicObject());
    setProp (piano, "showPianoKeyboard",  d.showPianoKeyboard);
    setProp (piano, "widthPx",            d.pianoWidthPx);
    setProp (piano, "blackKeyWidth",      d.pianoBlackKeyWidth);

    juce::DynamicObject::Ptr transport (new juce::DynamicObject());
    setProp (transport, "showTransportBar", d.showTransportBar);
    setProp (transport, "heightPx",         d.transportBarHeight);
    setProp (transport, "loopRegionAlpha",  d.loopRegionAlpha);

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
                                             "Colours are #RRGGBB or #AARRGGBB. 'preset' names the built-in "
                                             "theme this started from and is only used to tick the Theme menu; "
                                             "picking a preset REPLACES this file."));
    setProp (root, "preset",    d.presetName);
    setProp (root, "colours",   juce::var (colours.get()));
    setProp (root, "lane",      juce::var (lane.get()));
    setProp (root, "highway",   juce::var (highway.get()));
    setProp (root, "piano",     juce::var (piano.get()));
    setProp (root, "transport", juce::var (transport.get()));
    setProp (root, "trace",     juce::var (trace.get()));
    setProp (root, "tolerance", juce::var (tol.get()));
    setProp (root, "smoothing", juce::var (smoothing.get()));

    const auto file = getThemeFile();

    if (! file.getParentDirectory().createDirectory().wasOk())
        return false;

    return file.replaceWithText (juce::JSON::toString (juce::var (root.get()), false, 6));
}

void Theme::adoptValues (const ThemeValues& newValues)
{
    values = newValues;
    sendChangeMessage();
}

bool Theme::applyPreset (const juce::String& name)
{
    const auto preset = getPreset (name);

    if (! writeValuesToFile (preset))
        return false;

    // Adopt the file's stamps NOW so the 4 Hz poll does not see its own write as
    // a user edit and reload-and-broadcast a second time a quarter of a second
    // later. The values would be identical, but the second broadcast rebuilds
    // every buffered layer in the UI for nothing.
    const auto file = getThemeFile();
    seenModTime   = file.getLastModificationTime();
    seenSize      = file.getSize();
    changePending = false;
    parseError    = false;

    adoptValues (preset);
    return true;
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

    //--------------------------------------------------------------------------
    // THE FALLBACK BASE IS THE NAMED PRESET, NOT THE COMPILED-IN DEFAULTS.
    //
    // Every key is optional, so a theme.json containing nothing but
    //     { "preset": "Sheet Music" }
    // is a complete, valid theme — which is the whole point of naming presets in
    // the file rather than only in a menu. Falling back to Midnight instead
    // would hand back a dark grid with a couple of paper-coloured strays in it.
    //--------------------------------------------------------------------------
    const auto presetName = readString (root, "preset", ThemeValues {}.presetName);
    const auto d = getPreset (presetName);

    ThemeValues v = d;
    v.presetName = presetName;

    const auto colours   = childObject (root, "colours");
    const auto lane      = childObject (root, "lane");
    const auto highway   = childObject (root, "highway");
    const auto piano     = childObject (root, "piano");
    const auto transport = childObject (root, "transport");
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
    v.staffLine         = readColour (colours, "staffLine",         d.staffLine);
    v.staffLedger       = readColour (colours, "staffLedger",       d.staffLedger);
    v.pianoWhiteKey     = readColour (colours, "pianoWhiteKey",     d.pianoWhiteKey);
    v.pianoBlackKey     = readColour (colours, "pianoBlackKey",     d.pianoBlackKey);
    v.pianoKeyBorder    = readColour (colours, "pianoKeyBorder",    d.pianoKeyBorder);
    v.pianoLabel        = readColour (colours, "pianoLabel",        d.pianoLabel);
    v.transportTrack    = readColour (colours, "transportTrack",    d.transportTrack);
    v.transportDensity  = readColour (colours, "transportDensity",  d.transportDensity);
    v.transportPlayhead = readColour (colours, "transportPlayhead", d.transportPlayhead);
    v.loopRegion        = readColour (colours, "loopRegion",        d.loopRegion);

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

    v.backgroundStyle          = backgroundStyleFromString (
                                     readString (highway, "backgroundStyle",
                                                 backgroundStyleToString (d.backgroundStyle)));
    v.staffLineThicknessPx     = juce::jlimit (0.25f, 8.0f,  readFloat (highway, "staffLineThicknessPx",    d.staffLineThicknessPx));
    v.staffLedgerThicknessPx   = juce::jlimit (0.25f, 8.0f,  readFloat (highway, "staffLedgerThicknessPx",  d.staffLedgerThicknessPx));
    v.staffGridAlpha           = juce::jlimit (0.0f,  1.0f,  readFloat (highway, "staffGridAlpha",          d.staffGridAlpha));
    v.showNoteNames            =                             readBool  (highway, "showNoteNames",           d.showNoteNames);
    v.noteNameShowOctave       =                             readBool  (highway, "noteNameShowOctave",      d.noteNameShowOctave);
    v.noteNameHeightPx         = juce::jlimit (5.0f,  40.0f, readFloat (highway, "noteNameHeightPx",        d.noteNameHeightPx));
    v.noteNameMinBarWidthPx    = juce::jlimit (0.0f, 400.0f, readFloat (highway, "noteNameMinBarWidthPx",   d.noteNameMinBarWidthPx));
    v.noteNameAlpha            = juce::jlimit (0.0f,  1.0f,  readFloat (highway, "noteNameAlpha",           d.noteNameAlpha));

    v.showPianoKeyboard        =                             readBool  (piano, "showPianoKeyboard",  d.showPianoKeyboard);
    v.pianoWidthPx             = juce::jlimit (24,    400,   readInt   (piano, "widthPx",            d.pianoWidthPx));
    v.pianoBlackKeyWidth       = juce::jlimit (0.15f, 1.0f,  readFloat (piano, "blackKeyWidth",      d.pianoBlackKeyWidth));

    v.showTransportBar         =                             readBool  (transport, "showTransportBar", d.showTransportBar);
    v.transportBarHeight       = juce::jlimit (12,    200,   readInt   (transport, "heightPx",         d.transportBarHeight));
    v.loopRegionAlpha          = juce::jlimit (0.0f,  1.0f,  readFloat (transport, "loopRegionAlpha",  d.loopRegionAlpha));

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
