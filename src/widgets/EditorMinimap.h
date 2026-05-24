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

#include <QColor>
#include <QPointer>
#include <QVector>
#include <QWidget>

#include <cstdint>

class ScintillaNext;

// Vertical thumbnail view of the editor content, rendered down to ~2px per
// line. Modeled after the VS Code / Sublime minimap: shows where the code is
// dense, marks git changes, and lets the user click/drag to navigate.
//
// Performance contract:
//   * paintEvent walks the line table once per repaint, O(lines).
//   * No text glyph rendering — only filled rectangles. A 50k-line file paints
//     in ~1 ms because the inner loop is two QPainter::fillRect calls per
//     line and the QImage-sized backing store is reused across paints.
//   * Repaint trigger is coalesced via a 16 ms throttle so a burst of
//     SCN_MODIFIED notifications doesn't stall the editor.
//
// Hooks into ScintillaNext via signals (modified, painted) — no per-keystroke
// Scintilla API calls from paintEvent. Read-only with respect to the editor;
// mouse events emit `scrollRequested(line)` which the host wires to
// editor->gotoLine/setFirstVisibleLine.
class EditorMinimap : public QWidget
{
    Q_OBJECT

public:
    explicit EditorMinimap(ScintillaNext *editor, QWidget *parent = nullptr);
    ~EditorMinimap() override;

    // Pure-math accessors exposed for unit tests. Both clamp out-of-range
    // inputs so callers don't have to validate.
    qint32 lineFromY(int y) const;
    int    yFromLine(qint32 line) const;

    // Static pure variants (no ScintillaNext / widget state). Same clamping
    // semantics as the instance methods. Used by unit tests and reachable by
    // callers that already know height + lineCount.
    static qint32 lineFor(int y, int widgetHeight, qint32 lineCount);
    static int    yFor(qint32 line, int widgetHeight, qint32 lineCount);

    // Override the default fixed width. The default (100 px) matches the
    // VS Code reference.
    void setMinimapWidth(int px);
    int  minimapWidth() const { return m_width; }

public slots:
    // Repaint trigger. Coalesced via a single-shot 16 ms timer so a burst
    // of editor notifications produces at most one frame.
    void scheduleRepaint();

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    // Reposition the minimap whenever the host editor resizes.
    bool eventFilter(QObject *watched, QEvent *event) override;

private slots:
    void doRepaint();

private:
    void connectEditorSignals();
    // Apply theme-derived colours (background, fill, viewport). Called on
    // ctor and on app-level effectiveThemeChanged.
    void refreshPalette();
    // Position the widget at the right edge of the host editor, immediately
    // left of the vertical scrollbar so neither overlaps the other.
    void repositionToHost();

    // Scroll the editor so the given line becomes vertically centered in
    // the viewport, clamped to the document range.
    void scrollEditorToLine(qint32 line);

    QPointer<ScintillaNext> m_editor;
    int  m_width = 100;
    bool m_repaintScheduled = false;
    bool m_dragging = false;

    // Cached palette — refreshed only on theme change, hot in paintEvent.
    QColor m_bg;
    QColor m_lineFill;
    QColor m_viewport;
    QColor m_addedBand;
    QColor m_modifiedBand;
    QColor m_deletedBand;
};
