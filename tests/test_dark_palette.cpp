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

#include <QtTest>
#include <QColor>

#include "DarkPalette.h"


class TestDarkPalette : public QObject
{
    Q_OBJECT

private slots:
    // The whole point of the transform: every light-mode lexer foreground
    // that lands in the editor must be readable on #1E1E1E.
    void allLightModeLexerForegrounds_meetMinimumLuminance();

    // Specific shapes the bash.lua palette ships with — these are the colors
    // that motivated the change.
    void darkGreenComment_becomesReadableGreen();
    void midGreyString_becomesReadableGrey();
    void darkBrownOperator_becomesReadableOrangeOrBrown();
    void pureBlackIdentifier_becomesNeutralLightGrey();

    // Hue preservation: a green should still look green, a red still red.
    void colorfulInputs_preserveHueWithinTolerance();

    // Already-readable inputs must pass through unchanged so we don't
    // whitewash careful palettes.
    void alreadyLightForeground_isUnchanged();

    // BGR↔RGB wrapper: Scintilla stores foregrounds 0xBBGGRR, so the
    // wrapper must round-trip correctly.
    void sciWrapper_roundTripsBgrEncoding();

    // Edge cases.
    void invalidQColor_returnsNeutralIdentifier();
    void pureWhite_isPassedThrough();

    // Lexer-by-lexer regression table: every per-style foreground we have
    // seen on a real src/languages/*.lua, transformed, must land readable on
    // #1E1E1E. Also prints the input→output table to qDebug so a reviewer
    // can eyeball the actual hex values without launching the app.
    void realLexerForegrounds_allLiftToReadable();
};


// ---------- helpers ----------

namespace {
constexpr qreal kReadabilityFloor = DarkPalette::kMinSaturatedLuminance;
const QColor kEditorBg(0x1E, 0x1E, 0x1E);

// Hue distance accounting for wraparound at 360°. Returns degrees in [0, 180].
int hueDistanceDeg(int a, int b)
{
    if (a < 0 || b < 0) return 0; // achromatic
    int d = std::abs(a - b);
    if (d > 180) d = 360 - d;
    return d;
}
} // namespace


// ---------- tests ----------

void TestDarkPalette::allLightModeLexerForegrounds_meetMinimumLuminance()
{
    // Sampled from src/languages/*.lua: representative light-mode foregrounds
    // covering the awkward dark-on-dark cases.
    const QList<QColor> lightModeForegrounds{
        QColor(QRgb(0x000000)), // identifier/default
        QColor(QRgb(0x008000)), // comment line (bash, c, python, ...)
        QColor(QRgb(0x808080)), // string (bash)
        QColor(QRgb(0x804000)), // operator (bash)
        QColor(QRgb(0x0000FF)), // instruction word / keyword
        QColor(QRgb(0xFF0000)), // number / here Q
        QColor(QRgb(0x008080)), // param (bash)
        QColor(QRgb(0x804040)), // backticks (bash)
        QColor(QRgb(0xFF8040)), // scalar (bash)
        QColor(QRgb(0x8000FF)), // some lexers' preprocessor
        QColor(QRgb(0x606020)), // muted dark olive
        QColor(QRgb(0x404040)), // dark grey
    };

    for (const QColor &fg : lightModeForegrounds) {
        const QColor out = DarkPalette::lightenForDarkBackground(fg);
        const qreal lum = DarkPalette::relativeLuminance(out);
        QVERIFY2(lum >= kReadabilityFloor,
            qPrintable(QStringLiteral("fg=%1 → out=%2 luminance %3 < floor %4")
                .arg(fg.name(), out.name())
                .arg(lum, 0, 'f', 3)
                .arg(kReadabilityFloor, 0, 'f', 3)));
    }
}

void TestDarkPalette::darkGreenComment_becomesReadableGreen()
{
    const QColor fg(QRgb(0x008000)); // bash COMMENT LINE
    const QColor out = DarkPalette::lightenForDarkBackground(fg);

    QVERIFY(DarkPalette::relativeLuminance(out) >= kReadabilityFloor);
    // Still recognizably green: H ≈ 120°, ± a few degrees of slack.
    QVERIFY2(hueDistanceDeg(out.hsvHue(), 120) <= 10,
        qPrintable(QStringLiteral("hue drifted to %1°").arg(out.hsvHue())));
}

void TestDarkPalette::midGreyString_becomesReadableGrey()
{
    const QColor fg(QRgb(0x808080)); // bash STRING — mid grey
    const QColor out = DarkPalette::lightenForDarkBackground(fg);

    // Achromatic → must clamp to the neutral identifier color so it doesn't
    // get tinted by HSL rounding.
    QCOMPARE(out, QColor(DarkPalette::kNeutralIdentifier));
}

void TestDarkPalette::darkBrownOperator_becomesReadableOrangeOrBrown()
{
    const QColor fg(QRgb(0x804000)); // bash OPERATOR — dark brown
    const QColor out = DarkPalette::lightenForDarkBackground(fg);

    QVERIFY(DarkPalette::relativeLuminance(out) >= kReadabilityFloor);
    // Brown sits at H ≈ 30° (orange family). Allow generous slack.
    QVERIFY2(hueDistanceDeg(out.hsvHue(), 30) <= 15,
        qPrintable(QStringLiteral("brown drifted to hue %1°").arg(out.hsvHue())));
}

void TestDarkPalette::pureBlackIdentifier_becomesNeutralLightGrey()
{
    const QColor out = DarkPalette::lightenForDarkBackground(QColor(0, 0, 0));
    QCOMPARE(out, QColor(DarkPalette::kNeutralIdentifier));
}

void TestDarkPalette::colorfulInputs_preserveHueWithinTolerance()
{
    struct Sample {
        QColor fg;
        int expectedHue;
    };
    const QList<Sample> samples{
        {QColor(QRgb(0x0000FF)), 240}, // pure blue
        {QColor(QRgb(0xFF0000)),   0}, // pure red
        {QColor(QRgb(0x008080)), 180}, // teal
        {QColor(QRgb(0x800080)), 300}, // purple
        {QColor(QRgb(0xFF8040)),  20}, // already-bright orange
    };

    for (const Sample &s : samples) {
        const QColor out = DarkPalette::lightenForDarkBackground(s.fg);
        QVERIFY2(hueDistanceDeg(out.hsvHue(), s.expectedHue) <= 10,
            qPrintable(QStringLiteral("fg=%1 expected ~%2° got %3°")
                .arg(s.fg.name())
                .arg(s.expectedHue)
                .arg(out.hsvHue())));
    }
}

void TestDarkPalette::alreadyLightForeground_isUnchanged()
{
    // Light pastel that already reads on #1E1E1E — must pass through.
    const QList<QColor> alreadyBright{
        QColor(0xD4, 0xD4, 0xD4),
        QColor(0xCC, 0xCC, 0xCC),
        QColor(0xFF, 0xC0, 0x80),
        QColor(0xA0, 0xE0, 0xA0),
    };

    for (const QColor &fg : alreadyBright) {
        const QColor out = DarkPalette::lightenForDarkBackground(fg);
        QCOMPARE(out, fg);
    }
}

void TestDarkPalette::sciWrapper_roundTripsBgrEncoding()
{
    // 0x008000 in RGB is dark green. In Scintilla's 0xBBGGRR encoding that's
    // 0x008000 as well (because R=0). Use an asymmetric color to verify the
    // BGR↔RGB swap is actually happening: 0xFF0000 in BGR == blue.
    //
    // Pure blue (RGB 0x0000FF) is stored in Scintilla as BGR 0xFF0000.
    const int sciIn = 0xFF0000; // BGR for pure blue
    const int sciOut = DarkPalette::lightenSciForeground(sciIn);

    // Decode back to RGB.
    const int b = (sciOut >> 16) & 0xFF;
    const int g = (sciOut >> 8) & 0xFF;
    const int r = sciOut & 0xFF;
    const QColor outRgb(r, g, b);

    QVERIFY(DarkPalette::relativeLuminance(outRgb) >= kReadabilityFloor);
    QVERIFY2(hueDistanceDeg(outRgb.hsvHue(), 240) <= 10,
        qPrintable(QStringLiteral("blue drifted to %1°").arg(outRgb.hsvHue())));
}

void TestDarkPalette::invalidQColor_returnsNeutralIdentifier()
{
    const QColor out = DarkPalette::lightenForDarkBackground(QColor());
    QCOMPARE(out, QColor(DarkPalette::kNeutralIdentifier));
}

void TestDarkPalette::pureWhite_isPassedThrough()
{
    const QColor fg(0xFF, 0xFF, 0xFF);
    const QColor out = DarkPalette::lightenForDarkBackground(fg);
    QCOMPARE(out, fg);
}


void TestDarkPalette::realLexerForegrounds_allLiftToReadable()
{
    // (language, style, fg authored for white bg). Sampled directly from
    // src/languages/{bash,cpp,python,javascript}.lua. Each entry is what
    // SetStyle() actually writes into Scintilla before applyThemeToEditor
    // takes a second pass. The transform must turn each one into something
    // readable on #1E1E1E.
    struct Entry {
        const char *lang;
        const char *style;
        QRgb lightFg; // 0xRRGGBB
    };

    const QList<Entry> entries{
        // bash.lua
        {"bash", "DEFAULT",          0x000000},
        {"bash", "INSTRUCTION WORD", 0x0000FF},
        {"bash", "NUMBER",           0xFF0000},
        {"bash", "STRING",           0x808080},
        {"bash", "CHARACTER",        0x808080},
        {"bash", "OPERATOR",         0x804000},
        {"bash", "IDENTIFIER",       0x000000},
        {"bash", "SCALAR",           0xFF8040},
        {"bash", "COMMENT LINE",     0x008000},
        {"bash", "PARAM",            0x008080},
        {"bash", "BACKTICKS",        0x804040},

        // cpp.lua — biggest language matrix in the codebase
        {"cpp", "PREPROCESSOR",      0x804000},
        {"cpp", "DEFAULT",           0x000000},
        {"cpp", "INSTRUCTION WORD",  0x0000FF},
        {"cpp", "TYPE WORD",         0x8000FF},
        {"cpp", "NUMBER",            0xFF8000},
        {"cpp", "STRING",            0x808080},
        {"cpp", "CHARACTER",         0x808080},
        {"cpp", "OPERATOR",          0x000080},
        {"cpp", "VERBATIM",          0x000000},
        {"cpp", "COMMENT",           0x008000},
        {"cpp", "COMMENT LINE",      0x008000},
        {"cpp", "COMMENT DOC",       0x008080},
        {"cpp", "TASK MARKER",       0x008000},

        // python.lua
        {"python", "DEFAULT",     0x000000},
        {"python", "COMMENTLINE", 0x008000},
        {"python", "NUMBER",      0xFF0000},
        {"python", "STRING",      0x808080},
        {"python", "KEYWORDS",    0x0000FF},
        {"python", "BUILTINS",    0x880088},
        {"python", "TRIPLE",      0xFF8000},
        {"python", "DEFNAME",     0xFF00FF},
        {"python", "OPERATOR",    0x000080},
        {"python", "IDENTIFIER",  0x000000},
    };

    qDebug().noquote() << "  language  style                  light fg → dark fg  (lum)";
    qDebug().noquote() << "  --------  ---------------------  -----------------------";

    int failures = 0;
    for (const Entry &e : entries) {
        const QColor in(e.lightFg);
        const QColor out = DarkPalette::lightenForDarkBackground(in);
        const qreal lum = DarkPalette::relativeLuminance(out);

        qDebug().noquote() << QStringLiteral("  %1  %2  %3 → %4  (%5)")
            .arg(QString::fromLatin1(e.lang).leftJustified(8))
            .arg(QString::fromLatin1(e.style).leftJustified(21))
            .arg(in.name())
            .arg(out.name())
            .arg(lum, 0, 'f', 3);

        // Every output must clear the saturated-color floor at minimum.
        if (lum < DarkPalette::kMinSaturatedLuminance) {
            ++failures;
            QFAIL(qPrintable(QStringLiteral("%1/%2: %3 → %4 luminance %5 < floor %6")
                .arg(e.lang, e.style, in.name(), out.name())
                .arg(lum, 0, 'f', 3)
                .arg(DarkPalette::kMinSaturatedLuminance, 0, 'f', 3)));
        }

        // Hue preservation (only meaningful for chromatic inputs).
        const int inSat = in.hsvSaturation();
        if (inSat >= DarkPalette::kAchromaticSaturation) {
            const int inHue = in.hsvHue();
            const int outHue = out.hsvHue();
            const int dist = hueDistanceDeg(inHue, outHue);
            QVERIFY2(dist <= 15,
                qPrintable(QStringLiteral("%1/%2: hue drifted %3° (in=%4° out=%5°)")
                    .arg(e.lang, e.style)
                    .arg(dist).arg(inHue).arg(outHue)));
        } else {
            // Achromatic input must land on the neutral identifier color so
            // grey strings / black identifiers don't pick up a hue.
            QVERIFY2(out == QColor(DarkPalette::kNeutralIdentifier),
                qPrintable(QStringLiteral("%1/%2: achromatic input %3 → %4, "
                    "expected neutral identifier")
                    .arg(e.lang, e.style, in.name(), out.name())));
        }
    }

    QCOMPARE(failures, 0);
}


QTEST_APPLESS_MAIN(TestDarkPalette)

#include "test_dark_palette.moc"
