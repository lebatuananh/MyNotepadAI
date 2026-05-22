/*
 * This file is part of Notepad Next.
 * Copyright 2026 NotepadAI contributors
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

#include "GitDiffPainter.h"

#include "GitDiffPalette.h"
#include "ScintillaNext.h"

#include "Scintilla.h"

#include <QByteArray>
#include <cstring>

namespace {

inline sptr_t rgb(const QColor &c)
{
    // Scintilla uses 0x00BBGGRR.
    return sptr_t(c.red()) | (sptr_t(c.green()) << 8) | (sptr_t(c.blue()) << 16);
}

inline void setStyleFB(ScintillaNext *e, int id, const QColor &fg, const QColor &bg)
{
    e->send(SCI_STYLESETFORE, id, rgb(fg));
    e->send(SCI_STYLESETBACK, id, rgb(bg));
}

inline int countDigits(qint32 x)
{
    return (x < 10 ? 1 :
           (x < 100 ? 2 :
           (x < 1000 ? 3 :
           (x < 10000 ? 4 :
           (x < 100000 ? 5 :
           (x < 1000000 ? 6 :
           (x < 10000000 ? 7 :
           (x < 100000000 ? 8 :
           (x < 1000000000 ? 9 :
           10)))))))));
}

} // namespace

void GitDiffPainter::configureEditor(ScintillaNext *editor, const GitDiffPalette &pal)
{
    if (!editor) return;

    // No lexer — we apply styles ourselves.
    editor->send(SCI_SETREADONLY, 0, 0);
    editor->send(SCI_CLEARALL, 0, 0);
    editor->send(SCI_STYLECLEARALL, 0, 0);

    // Default style uses the editor's existing default background; ensure
    // foreground readable.
    const QColor defFg = pal.fgHunkHeader; // close to muted text, just placeholder
    setStyleFB(editor, StyleDefault,    QColor(Qt::black),  QColor(Qt::white));
    setStyleFB(editor, StyleFileHeader, pal.fgHunkHeader,   QColor(Qt::transparent));
    setStyleFB(editor, StyleHunkHeader, pal.fgHunkHeader,   pal.bgHunkHeader);
    setStyleFB(editor, StyleContext,    QColor(Qt::black),  QColor(Qt::white));
    setStyleFB(editor, StyleAdded,      QColor(Qt::black),  pal.bgAddLine);
    setStyleFB(editor, StyleDeleted,    QColor(Qt::black),  pal.bgDelLine);
    setStyleFB(editor, StyleNoNewline,  pal.fgHunkHeader,   QColor(Qt::transparent));

    // Margin 0 is repurposed as a right-aligned TEXT margin: GitDiffPainter::render
    // populates it per-row with the diff's own old/new file line numbers (blank for
    // headers / hunk headers / no-newline markers / blank separators). The width is
    // set in render() once the maximum displayed number is known.
    editor->send(SCI_SETMARGINTYPEN, 0, SC_MARGIN_RTEXT);
    editor->send(SCI_SETMARGINWIDTHN, 0, 0);
    // Hide selection / line markers / fold markers to look like a static view.
    editor->send(SCI_SETMARGINWIDTHN, 1, 0);
    editor->send(SCI_SETMARGINWIDTHN, 2, 0);
    (void)defFg;
}

void GitDiffPainter::render(ScintillaNext *editor, const GitDiffParser::Result &parsed)
{
    if (!editor) return;

    editor->send(SCI_SETREADONLY, 0, 0);
    editor->send(SCI_CLEARALL, 0, 0);

    const int rows = parsed.kinds.size();
    if (rows == 0) {
        editor->send(SCI_SETREADONLY, 1, 0);
        return;
    }

    // Estimate total size: sum of line lengths + 2 bytes/line (prefix + LF).
    qsizetype total = 0;
    for (int r = 0; r < rows; ++r) total += parsed.texts.at(r).size() + 2;

    QByteArray text;
    QByteArray styles;
    text.reserve(total);
    styles.reserve(total);

    for (int r = 0; r < rows; ++r) {
        const QByteArray &line = parsed.texts.at(r);
        const auto kind = parsed.kinds.at(r);

        char prefix = '\0';
        char styleId = StyleDefault;
        switch (kind) {
            case GitDiffParser::LineKind::FileHeader: styleId = StyleFileHeader; break;
            case GitDiffParser::LineKind::HunkHeader: styleId = StyleHunkHeader; break;
            case GitDiffParser::LineKind::Context:    styleId = StyleContext;   prefix = ' '; break;
            case GitDiffParser::LineKind::Added:      styleId = StyleAdded;     prefix = '+'; break;
            case GitDiffParser::LineKind::Deleted:    styleId = StyleDeleted;   prefix = '-'; break;
            case GitDiffParser::LineKind::NoNewline:  styleId = StyleNoNewline; break;
            case GitDiffParser::LineKind::Empty:      styleId = StyleDefault;   break;
        }

        const qsizetype lineStartInText = text.size();
        if (prefix != '\0') text.append(prefix);
        text.append(line);
        text.append('\n');
        const qsizetype lineEndInText = text.size();
        const qsizetype lineBytes = lineEndInText - lineStartInText;

        // Append styleId for each byte of this line.
        const qsizetype oldStylesSize = styles.size();
        styles.resize(oldStylesSize + lineBytes);
        std::memset(styles.data() + oldStylesSize, styleId, static_cast<size_t>(lineBytes));
    }

    // One bulk append + one bulk style flush.
    editor->send(SCI_APPENDTEXT, text.size(), reinterpret_cast<sptr_t>(text.constData()));
    editor->send(SCI_STARTSTYLING, 0, 0);
    editor->send(SCI_SETSTYLINGEX, styles.size(), reinterpret_cast<sptr_t>(styles.constData()));

    // Margin 0 text: per-row diff line numbers. Added/Context show newLn, Deleted
    // shows oldLn, everything else (FileHeader/HunkHeader/NoNewline/Empty) is blank.
    // First pass finds the widest number so the margin can be sized exactly once;
    // second pass posts SCI_MARGINSETTEXT per row. Per-row sends are unavoidable —
    // Scintilla has no batch margin-text API — but each goes through SciFnDirect,
    // i.e. a single virtual dispatch into Editor::WndProc with no allocations.
    editor->send(SCI_MARGINTEXTCLEARALL, 0, 0);

    qint32 maxLn = 0;
    for (int r = 0; r < rows; ++r) {
        const auto kind = parsed.kinds.at(r);
        qint32 n = -1;
        switch (kind) {
            case GitDiffParser::LineKind::Added:
            case GitDiffParser::LineKind::Context:
                n = parsed.newLn.at(r);
                break;
            case GitDiffParser::LineKind::Deleted:
                n = parsed.oldLn.at(r);
                break;
            default:
                break;
        }
        if (n > maxLn) maxLn = n;
    }

    if (maxLn > 0) {
        const sptr_t charW = editor->send(SCI_TEXTWIDTH, STYLE_LINENUMBER,
                                          reinterpret_cast<sptr_t>("8"));
        const int digits = countDigits(maxLn);
        editor->send(SCI_SETMARGINWIDTHN, 0,
                     static_cast<sptr_t>(8 + (digits + 1) * static_cast<int>(charW)));

        char buf[16];
        for (int r = 0; r < rows; ++r) {
            const auto kind = parsed.kinds.at(r);
            qint32 n = -1;
            switch (kind) {
                case GitDiffParser::LineKind::Added:
                case GitDiffParser::LineKind::Context:
                    n = parsed.newLn.at(r);
                    break;
                case GitDiffParser::LineKind::Deleted:
                    n = parsed.oldLn.at(r);
                    break;
                default:
                    break;
            }
            if (n <= 0) continue;

            // Render number into a fixed stack buffer to avoid per-row QByteArray
            // allocations on large diffs.
            int len = 0;
            char tmp[16];
            qint32 v = n;
            do {
                tmp[len++] = char('0' + (v % 10));
                v /= 10;
            } while (v > 0);
            for (int i = 0; i < len; ++i) buf[i] = tmp[len - 1 - i];
            buf[len] = '\0';

            editor->send(SCI_MARGINSETTEXT, r, reinterpret_cast<sptr_t>(buf));
            editor->send(SCI_MARGINSETSTYLE, r, STYLE_LINENUMBER);
        }
    } else {
        // No diff body lines (e.g. binary banner only) — keep the margin hidden.
        editor->send(SCI_SETMARGINWIDTHN, 0, 0);
    }

    editor->send(SCI_SETREADONLY, 1, 0);
}
