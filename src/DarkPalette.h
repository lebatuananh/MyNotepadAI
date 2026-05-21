/*
 * This file is part of Notepad Next.
 *
 * Notepad Next is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Notepad Next is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Notepad Next.  If not, see <https://www.gnu.org/licenses/>.
 */

#pragma once

#include <QColor>


// Per-language Lua files in src/languages/*.lua hard-code foreground colors for
// a white background (black identifiers, dark-green comments, mid-grey strings,
// dark-brown operators, etc). On the dark editor background (#1E1E1E) those
// colors are unreadable.
//
// Rather than fork ~80 language files (which would conflict with every upstream
// merge), we run their foregrounds through this pure transform after the
// lexer's Lua SetStyle() runs. The transform:
//
//   - Preserves hue and (mostly) saturation, so a "green" comment stays green.
//   - Lifts perceived luminance to a configurable floor, so even near-black
//     ink becomes readable.
//   - Leaves already-light colors alone.
//   - Clamps achromatic (grey/black) inputs to a neutral light grey so we
//     don't accidentally tint identifiers.
//
// Kept as a header-only inline so it can be linked into unit tests without
// dragging in Scintilla or NotepadNextApplication.
namespace DarkPalette {

// Relative-luminance floor we aim for, on Qt's 0..1 scale. 0.55 is bright
// enough to be comfortably readable on #1E1E1E while leaving room for chrome
// (line numbers etc.) to stay slightly dimmer.
inline constexpr qreal kMinLuminance = 0.55;

// Saturated-color luminance floor — we don't push fully-saturated hues all the
// way to 0.55 because that desaturates them. 0.45 keeps red/green/blue
// recognizable while still readable.
inline constexpr qreal kMinSaturatedLuminance = 0.45;

// Inputs with chroma below this are treated as grey/black and clamped to the
// neutral identifier color rather than tinted.
inline constexpr int kAchromaticSaturation = 16; // 0..255

// Default foreground for "black-ish" inputs (identifiers, default text).
// Matches the explicit defaultFore used in EditorManager::applyThemeToEditor.
inline constexpr QRgb kNeutralIdentifier = qRgb(0xD4, 0xD4, 0xD4);


// Relative luminance per WCAG 2.x (sRGB → linear → weighted sum).
inline qreal relativeLuminance(const QColor &c)
{
    auto channel = [](qreal s) {
        return (s <= 0.03928) ? (s / 12.92) : std::pow((s + 0.055) / 1.055, 2.4);
    };
    const qreal r = channel(c.redF());
    const qreal g = channel(c.greenF());
    const qreal b = channel(c.blueF());
    return 0.2126 * r + 0.7152 * g + 0.0722 * b;
}


// Take a foreground color authored for a white background and return one that
// is readable on the dark editor background. Pure; no globals.
inline QColor lightenForDarkBackground(const QColor &fg)
{
    if (!fg.isValid()) {
        return QColor(kNeutralIdentifier);
    }

    int h, s, v;
    fg.getHsv(&h, &s, &v);

    // Achromatic (black / dark grey / pure grey) → just promote to the neutral
    // identifier color. Don't try to invent a hue.
    if (s < kAchromaticSaturation) {
        // For already-light greys, pass through; they read fine.
        if (relativeLuminance(fg) >= kMinLuminance) {
            return fg;
        }
        return QColor(kNeutralIdentifier);
    }

    // Already bright enough → unchanged. Avoids whitewashing carefully-chosen
    // pastel highlights that some lexers ship.
    if (relativeLuminance(fg) >= kMinSaturatedLuminance) {
        return fg;
    }

    // Boost lightness in HSL space while preserving hue, then re-check
    // luminance. Iterate a few times because HSL lightness isn't a direct
    // proxy for perceived luminance.
    int hh, sl, ll;
    int al;
    fg.getHsl(&hh, &sl, &ll, &al);

    // Floor lightness at ~60% — gives saturated hues enough room to read
    // without washing them out. Cap so we never go pure white.
    constexpr int kMinLightness = 153; // 60% of 255
    constexpr int kMaxLightness = 220;
    ll = std::clamp(std::max(ll, kMinLightness), 0, kMaxLightness);

    // Trim saturation slightly so very saturated dark inks (pure red 0xFF0000,
    // pure blue 0x0000FF) don't vibrate against #1E1E1E.
    if (sl > 220) sl = 220;

    QColor out = QColor::fromHsl(hh, sl, ll, al);

    // Edge case: if the HSL boost still didn't clear the luminance floor
    // (can happen for deep blues), step lightness up until it does or we hit
    // the cap.
    for (int i = 0; i < 6 && relativeLuminance(out) < kMinSaturatedLuminance; ++i) {
        ll = std::min(ll + 12, kMaxLightness);
        out = QColor::fromHsl(hh, sl, ll, al);
    }

    return out;
}


// Convenience overload working directly on Scintilla's 0xBBGGRR color word.
// Scintilla stores foregrounds in BGR order; Qt expects RGB. This handles the
// swap on both sides so callers can just pipe styleFore() values through.
inline int lightenSciForeground(int sciBgr)
{
    const int b = (sciBgr >> 16) & 0xFF;
    const int g = (sciBgr >> 8) & 0xFF;
    const int r = sciBgr & 0xFF;

    const QColor out = lightenForDarkBackground(QColor(r, g, b));

    return (out.blue() << 16) | (out.green() << 8) | out.red();
}

} // namespace DarkPalette
