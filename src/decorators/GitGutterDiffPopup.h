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

#pragma once

#include <QByteArray>
#include <QFrame>
#include <QVector>

class QFont;
class QPlainTextEdit;

// Floating "peek" popup that displays the HEAD-side (deleted) content of a
// single git hunk when the user clicks a git gutter marker. Modelled on the
// Zed "expand diff hunk" panel: only the deleted lines are rendered, because
// the buffer already shows the current/new content — duplicating it inside
// the popup would force the eye to compare two identical blocks.
//
// One popup widget is owned per editor (lazy-created on first click) and
// shown/hidden — never recreated — to avoid the per-show paint cost. Window
// flag Qt::Popup auto-dismisses on click outside, matching the JetBrains /
// VSCode "peek" UX.
class GitGutterDiffPopup : public QFrame
{
    Q_OBJECT
public:
    explicit GitGutterDiffPopup(QWidget *parent = nullptr);

    // Render the HEAD-side lines of the clicked hunk and float the popup so
    // its bottom edge sits at `anchorGlobalPos` — i.e. the popup hovers
    // ABOVE the hunk, leaving the live buffer text visible. If the popup
    // would clip the top of the screen, it flips below the anchor. Empty
    // `oldLines` is a no-op (caller is expected to filter pure-Added hunks
    // — there is no deleted content to show).
    //
    // `editorFont` is propagated from ScintillaNext so the popup's rows line
    // up visually with the buffer below — typography mismatch would make the
    // peek read as "different file" rather than "earlier state of the same
    // file". Re-applied per call because the user may change editor font at
    // runtime.
    void showDeletedHunk(const QVector<QByteArray> &oldLines,
                         const QFont &editorFont,
                         bool isDark,
                         const QPoint &anchorGlobalPos);

private:
    QPlainTextEdit *m_view;
};
