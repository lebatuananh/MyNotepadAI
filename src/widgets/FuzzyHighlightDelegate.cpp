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

#include "FuzzyHighlightDelegate.h"

#include <QFontMetrics>
#include <QIcon>
#include <QPainter>
#include <QStringView>

void FuzzyHighlightDelegate::paint(QPainter *painter, const QStyleOptionViewItem &option,
                                   const QModelIndex &index) const
{
    QStyleOptionViewItem opt = option;
    initStyleOption(&opt, index);

    painter->save();

    if (opt.state & QStyle::State_Selected)
        painter->fillRect(opt.rect, opt.palette.highlight());
    else if (opt.state & QStyle::State_MouseOver)
        painter->fillRect(opt.rect, opt.palette.highlight().color().lighter(160));

    const QString text = index.data(Qt::DisplayRole).toString();
    const auto positions = index.data(MatchPositionsRole).value<QVector<int>>();
    const QIcon icon = index.data(LeadingIconRole).value<QIcon>();
    const QString trailing = index.data(TrailingTextRole).toString();

    QRect textRect = opt.rect.adjusted(4, 0, -4, 0);
    const QFont font = opt.font;
    const QFontMetrics fm(font);

    painter->setFont(font);

    const QColor normalColor = (opt.state & QStyle::State_Selected)
        ? opt.palette.highlightedText().color()
        : opt.palette.text().color();
    const QColor matchColor = QColor(79, 193, 255);

    // Leading icon (optional): square the row height, paint left, shrink rect.
    if (!icon.isNull()) {
        const int sz = qMin(textRect.height() - 2, 16);
        const QRect iconRect(textRect.left(), textRect.top() + (textRect.height() - sz) / 2, sz, sz);
        icon.paint(painter, iconRect, Qt::AlignCenter,
                   (opt.state & QStyle::State_Selected) ? QIcon::Selected : QIcon::Normal);
        textRect.setLeft(iconRect.right() + 6);
    }

    // Trailing dimmed label (optional, never fuzzy-highlighted): right-aligned,
    // reserves its width so the highlighted text never overlaps it.
    if (!trailing.isEmpty()) {
        const int tw = fm.horizontalAdvance(trailing);
        const QRect trailRect(textRect.right() - tw, textRect.top(), tw, textRect.height());
        const QColor dim = (opt.state & QStyle::State_Selected)
            ? opt.palette.highlightedText().color()
            : opt.palette.placeholderText().color();
        painter->setPen(dim);
        painter->drawText(trailRect, Qt::AlignVCenter | Qt::AlignRight, trailing);
        textRect.setRight(trailRect.left() - 8);
    }

    // Highlighted display text: walk runs of matched / unmatched characters,
    // drawing matched runs in the accent color. Clipped to the (possibly
    // icon/trailing-shrunk) text rect.
    int posIdx = 0;
    const int posCount = positions.size();
    int x = textRect.left();
    const int y = textRect.top();
    const int h = textRect.height();
    int runStart = 0;
    const int textLen = text.length();

    painter->setClipRect(textRect);

    while (runStart < textLen && x < textRect.right()) {
        bool isMatch = (posIdx < posCount && positions[posIdx] == runStart);

        int runEnd = runStart + 1;
        if (isMatch) {
            ++posIdx;
            while (runEnd < textLen && posIdx < posCount && positions[posIdx] == runEnd) {
                ++posIdx;
                ++runEnd;
            }
        } else {
            int nextMatch = (posIdx < posCount) ? positions[posIdx] : textLen;
            runEnd = nextMatch;
        }

        const QStringView run = QStringView(text).mid(runStart, runEnd - runStart);
        const int runWidth = fm.horizontalAdvance(run.toString());

        painter->setPen(isMatch ? matchColor : normalColor);
        painter->drawText(QRect(x, y, runWidth, h), Qt::AlignVCenter, run.toString());

        x += runWidth;
        runStart = runEnd;
    }

    painter->restore();
}

QSize FuzzyHighlightDelegate::sizeHint(const QStyleOptionViewItem &option,
                                       const QModelIndex &index) const
{
    QStyleOptionViewItem opt = option;
    initStyleOption(&opt, index);
    const QFontMetrics fm(opt.font);
    return QSize(opt.rect.width(), fm.height() + 4);
}
