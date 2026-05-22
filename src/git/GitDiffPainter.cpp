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

    // Hide selection / line markers / fold markers to look like a static view.
    // Margin 0 (line numbers) is owned by the LineNumbers decorator and follows
    // the user's "Show Line Numbers" setting — don't override it here.
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

    editor->send(SCI_SETREADONLY, 1, 0);
}
