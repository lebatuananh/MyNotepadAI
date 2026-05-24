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

#include "GitGutterDiffPopup.h"

#include "../git/GitDiffPalette.h"

#include <QFont>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QPalette>
#include <QPlainTextEdit>
#include <QScreen>
#include <QScrollBar>
#include <QTextCursor>
#include <QVBoxLayout>

namespace {
constexpr int kMaxWidthPx  = 720;
constexpr int kMaxHeightPx = 320;
constexpr int kMinWidthPx  = 240;
constexpr int kPadPx       = 6;
constexpr int kRadiusPx    = 4; // status-surface radius per ui-dna.md
} // namespace

GitGutterDiffPopup::GitGutterDiffPopup(QWidget *parent)
    : QFrame(parent, Qt::Popup),
      m_view(new QPlainTextEdit(this))
{
    setFrameShape(QFrame::NoFrame);

    auto *lay = new QVBoxLayout(this);
    lay->setContentsMargins(kPadPx, kPadPx, kPadPx, kPadPx);
    lay->setSpacing(0);
    lay->addWidget(m_view);

    m_view->setReadOnly(true);
    m_view->setFrameShape(QFrame::NoFrame);
    m_view->setLineWrapMode(QPlainTextEdit::NoWrap);
    m_view->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_view->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_view->setCursorWidth(0);
}

void GitGutterDiffPopup::showDeletedHunk(const QVector<QByteArray> &oldLines,
                                          const QFont &editorFont,
                                          bool isDark,
                                          const QPoint &anchorGlobalPos)
{
    if (oldLines.isEmpty()) return;

    const GitDiffPalette &pal = GitDiffPalette::current(isDark);

    m_view->setFont(editorFont);

    // Chrome computed per call because palette() and the GitDiffPalette
    // selector both swap on theme change. The popup background is the same
    // bgDelLine red wash GitHub / Zed use so the row reads as "removed".
    const QColor border = palette().color(QPalette::Mid);
    const QColor bg     = pal.bgDelLine.isValid() ? pal.bgDelLine : QColor(0xFF, 0xEB, 0xE9);
    setStyleSheet(QStringLiteral(
            "GitGutterDiffPopup { background: %1; border: 1px solid %2; border-radius: %3px; }"
            "QPlainTextEdit { background: transparent; border: none; }")
                  .arg(bg.name(), border.name())
                  .arg(kRadiusPx));

    m_view->clear();
    QTextCursor cur(m_view->document());
    cur.beginEditBlock();
    // No leading '-' glyph — the popup itself is the red-tinted "removed"
    // surface, so a prefix character would be redundant chrome (Zed parity).
    for (int i = 0; i < oldLines.size(); ++i) {
        if (i > 0) cur.insertBlock();
        cur.insertText(QString::fromUtf8(oldLines.at(i)));
    }
    cur.endEditBlock();

    // Size to content, clamped. Width follows the widest line so wrap doesn't
    // surprise the reader; height follows the row count.
    const QFontMetrics fm(editorFont);
    int maxChars = 0;
    for (const QByteArray &t : oldLines) {
        maxChars = qMax(maxChars, static_cast<int>(QString::fromUtf8(t).size()));
    }
    const int contentW = fm.horizontalAdvance(QLatin1Char('M')) * qMax(8, maxChars) + 2 * kPadPx;
    const int contentH = fm.height() * oldLines.size() + 2 * kPadPx;

    const int w = qBound(kMinWidthPx, contentW, kMaxWidthPx);
    const int h = qMin(contentH, kMaxHeightPx);
    resize(w, h);

    // Default: popup's bottom edge meets the anchor (top of the hunk) so the
    // peek floats above the live rows. Flip below if it would clip the screen
    // top; clamp inside the screen on both axes.
    QPoint pos(anchorGlobalPos.x(), anchorGlobalPos.y() - h);
    if (QScreen *scr = QGuiApplication::screenAt(anchorGlobalPos)) {
        const QRect avail = scr->availableGeometry();
        if (pos.y() < avail.top())        pos.setY(anchorGlobalPos.y());
        if (pos.x() + w > avail.right())  pos.setX(avail.right() - w);
        if (pos.x() < avail.left())       pos.setX(avail.left());
        if (pos.y() + h > avail.bottom()) pos.setY(avail.bottom() - h);
    }
    move(pos);
    show();
    m_view->verticalScrollBar()->setValue(0);
    m_view->horizontalScrollBar()->setValue(0);
}
