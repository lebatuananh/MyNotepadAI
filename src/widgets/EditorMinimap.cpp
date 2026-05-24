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

#include "EditorMinimap.h"

#include "GitGutterMarkers.h"
#include "MinimapMath.h"
#include "ScintillaNext.h"
#include "../git/GitDiffPalette.h"
#include "../NotepadNextApplication.h"

#include <QApplication>
#include <QEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QResizeEvent>
#include <QScrollBar>
#include <QTimer>
#include <QWheelEvent>

#include <algorithm>

namespace {
// 16 ms ≈ one frame at 60 Hz. Coalescing per-keystroke notifications down to
// this cadence is invisible to the eye and saves ~30× repaint cost.
constexpr int kRepaintCoalesceMs = 16;

inline QColor mix(const QColor &a, const QColor &b, qreal t)
{
    const qreal it = 1.0 - t;
    return QColor(
        static_cast<int>(a.red()   * it + b.red()   * t),
        static_cast<int>(a.green() * it + b.green() * t),
        static_cast<int>(a.blue()  * it + b.blue()  * t));
}
} // namespace

EditorMinimap::EditorMinimap(ScintillaNext *editor, QWidget *parent)
    : QWidget(parent ? parent : editor),
      m_editor(editor)
{
    setAttribute(Qt::WA_OpaquePaintEvent);
    setCursor(Qt::PointingHandCursor);
    refreshPalette();

    if (auto *app = qobject_cast<NotepadNextApplication *>(qApp)) {
        connect(app, &NotepadNextApplication::effectiveThemeChanged, this, [this]() {
            refreshPalette();
            update();
        });
    }

    // When parented to the editor we self-position on every editor resize.
    // Parented elsewhere (tests, isolation harnesses), the host owns layout.
    if (m_editor && parent == nullptr) {
        m_editor->installEventFilter(this);
        repositionToHost();
        raise();
        show();
    } else {
        setFixedWidth(m_width);
    }

    connectEditorSignals();
}

EditorMinimap::~EditorMinimap() = default;

void EditorMinimap::setMinimapWidth(int px)
{
    m_width = std::clamp(px, 40, 300);
    if (parentWidget() == m_editor) {
        repositionToHost();
    } else {
        setFixedWidth(m_width);
    }
    update();
}

void EditorMinimap::repositionToHost()
{
    if (!m_editor) return;
    // Park the minimap to the immediate left of the editor's vertical
    // scrollbar so neither overlaps the other. When the scrollbar isn't
    // visible (small file), it reports width 0 — we still want some gap
    // from the right edge so the minimap reads as a strip, not as part of
    // the chrome.
    int sbW = 0;
    if (auto *sb = m_editor->verticalScrollBar()) {
        if (sb->isVisible()) sbW = sb->width();
    }
    const int x = m_editor->width() - sbW - m_width;
    setGeometry(x, 0, m_width, m_editor->height());
}

bool EditorMinimap::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_editor.data()) {
        const auto t = event->type();
        if (t == QEvent::Resize || t == QEvent::Show
            || t == QEvent::Move) {
            repositionToHost();
        }
    }
    return QWidget::eventFilter(watched, event);
}

void EditorMinimap::connectEditorSignals()
{
    if (!m_editor) return;
    // SCN_MODIFIED fires on text changes; SCN_UPDATEUI fires on scroll. Use
    // a queued debounce so a single keystroke that fires multiple flavours
    // of notification only triggers one repaint frame.
    connect(m_editor.data(), &ScintillaEdit::modified,
            this, &EditorMinimap::scheduleRepaint);
    connect(m_editor.data(), &ScintillaEdit::updateUi,
            this, &EditorMinimap::scheduleRepaint);
}

void EditorMinimap::scheduleRepaint()
{
    if (m_repaintScheduled) return;
    m_repaintScheduled = true;
    QTimer::singleShot(kRepaintCoalesceMs, this, &EditorMinimap::doRepaint);
}

void EditorMinimap::doRepaint()
{
    m_repaintScheduled = false;
    update();
}

void EditorMinimap::refreshPalette()
{
    bool isDark = false;
    if (auto *app = qobject_cast<NotepadNextApplication *>(qApp)) {
        isDark = app->isEffectiveThemeDark();
    }
    // Background: a touch darker than the editor's so the minimap reads as
    // a separate strip. Fill: muted foreground for "code present" rows.
    if (isDark) {
        m_bg       = QColor(30,  30,  35);
        m_lineFill = QColor(180, 180, 195, 110);
        m_viewport = QColor(255, 255, 255, 30);
    } else {
        m_bg       = QColor(245, 245, 248);
        m_lineFill = QColor(60,  60,  70,  90);
        m_viewport = QColor(0,   0,   0,   30);
    }
    const GitDiffPalette &p = GitDiffPalette::current(isDark);
    m_addedBand    = p.fgAdded;
    m_modifiedBand = p.fgModified;
    m_deletedBand  = p.fgDeleted;
}

qint32 EditorMinimap::lineFor(int y, int widgetHeight, qint32 lineCount)
{
    return MinimapMath::lineFor(y, widgetHeight, lineCount);
}

int EditorMinimap::yFor(qint32 line, int widgetHeight, qint32 lineCount)
{
    return MinimapMath::yFor(line, widgetHeight, lineCount);
}

qint32 EditorMinimap::lineFromY(int y) const
{
    if (!m_editor) return 0;
    return MinimapMath::lineFor(y, height(), static_cast<qint32>(m_editor->lineCount()));
}

int EditorMinimap::yFromLine(qint32 line) const
{
    if (!m_editor) return 0;
    return MinimapMath::yFor(line, height(), static_cast<qint32>(m_editor->lineCount()));
}

void EditorMinimap::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.fillRect(rect(), m_bg);

    if (!m_editor) return;
    const qint32 lineCount = static_cast<qint32>(m_editor->lineCount());
    if (lineCount <= 0) return;

    const int W = width();
    const int H = height();

    // Per-line band height (in floats, accumulated for the integer-line
    // rasterisation step below). We avoid querying Scintilla for line
    // lengths on the hot path because that's an O(N) cost on the wrong
    // side of the editor → minimap channel; instead, mark every line as
    // "present" with the uniform fill colour, and let git markers + the
    // viewport overlay do the visual work.
    const double pxPerLine = static_cast<double>(H) / static_cast<double>(lineCount);

    // Pass 1: uniform-tint fill for every line. We span the strip's inner
    // width minus a 2 px right gutter so git markers (drawn in pass 2) can
    // sit at the right edge and still read as "associated with the line".
    const int contentLeft  = 2;
    const int contentRight = W - 4; // leaves 2px for marker band
    const int fillWidth    = contentRight - contentLeft;
    if (fillWidth > 0) {
        painter.fillRect(contentLeft, 0, fillWidth, H, m_lineFill);
    }

    // Pass 2: git marker bands. We don't have access to the gutter's marker
    // line list here, so we walk Scintilla's marker bitmap once per
    // marker kind and paint the matching y-band. markerNext is O(log
    // lineCount) per query, but called only on lines that already host the
    // marker bit — effectively O(markers).
    auto paintMarker = [&](int markerId, const QColor &c) {
        if (!c.isValid()) return;
        const int bandX = W - 4;
        const int bandW = 3;
        qint32 ln = 0;
        while (true) {
            const qint32 hit = static_cast<qint32>(m_editor->markerNext(ln, 1 << markerId));
            if (hit < 0) break;
            const int y0 = static_cast<int>(hit * pxPerLine);
            const int y1 = std::max(y0 + 1,
                                    static_cast<int>((hit + 1) * pxPerLine));
            painter.fillRect(bandX, y0, bandW, y1 - y0, c);
            ln = hit + 1;
            if (ln >= lineCount) break;
        }
    };
    paintMarker(GitGutterMarkerIds::Added,    m_addedBand);
    paintMarker(GitGutterMarkerIds::Modified, m_modifiedBand);
    paintMarker(GitGutterMarkerIds::Deleted,  m_deletedBand);

    // Pass 3: viewport overlay. firstVisibleLine + linesOnScreen come from
    // Scintilla cheaply (precomputed state, no scan).
    const qint32 firstVis = static_cast<qint32>(m_editor->firstVisibleLine());
    const qint32 onScreen = static_cast<qint32>(m_editor->linesOnScreen());
    if (onScreen > 0) {
        const int vy0 = static_cast<int>(firstVis * pxPerLine);
        const int vy1 = std::max(vy0 + 1,
                                 static_cast<int>((firstVis + onScreen) * pxPerLine));
        painter.fillRect(0, vy0, W, vy1 - vy0, m_viewport);
    }
}

void EditorMinimap::resizeEvent(QResizeEvent *)
{
    update();
}

void EditorMinimap::scrollEditorToLine(qint32 line)
{
    if (!m_editor) return;
    const qint32 lineCount = static_cast<qint32>(m_editor->lineCount());
    if (lineCount <= 0) return;
    const qint32 onScreen = static_cast<qint32>(m_editor->linesOnScreen());
    // Centre the target line in the viewport when possible. Edge-clamp so
    // we never scroll past the document end.
    const qint32 top = std::clamp<qint32>(line - onScreen / 2, 0,
                                          std::max<qint32>(0, lineCount - 1));
    m_editor->setFirstVisibleLine(top);
}

void EditorMinimap::mousePressEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton) return;
    m_dragging = true;
    scrollEditorToLine(lineFromY(event->position().toPoint().y()));
}

void EditorMinimap::mouseMoveEvent(QMouseEvent *event)
{
    if (!m_dragging) return;
    scrollEditorToLine(lineFromY(event->position().toPoint().y()));
}

void EditorMinimap::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) m_dragging = false;
}

void EditorMinimap::wheelEvent(QWheelEvent *event)
{
    if (!m_editor) return;
    // Forward wheel deltas to the editor — same scrolling cadence as
    // hovering the editor itself.
    const int dy = event->angleDelta().y();
    if (dy == 0) return;
    const qint32 onScreen = static_cast<qint32>(m_editor->linesOnScreen());
    // ~3 lines per wheel notch, matching Qt's default.
    const qint32 step = std::max<qint32>(1, onScreen / 8);
    const qint32 first = static_cast<qint32>(m_editor->firstVisibleLine());
    const qint32 delta = (dy > 0) ? -step : step;
    m_editor->setFirstVisibleLine(std::max<qint32>(0, first + delta));
    event->accept();
}
