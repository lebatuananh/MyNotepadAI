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

#include <QStyledItemDelegate>

// Shared list-item delegate for fuzzy-search pickers (Quick File Open, Recent
// Workspace Open). Paints matched characters in an accent color using the
// per-row match positions, and optionally renders a leading icon and a
// trailing dimmed label (e.g. a remote connection status). Pickers that only
// need highlighting simply leave the optional roles unset — behavior is then
// identical to the original Quick-File-Open delegate.
class FuzzyHighlightDelegate : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    // QVector<int> of matched character indices into the DisplayRole string.
    static constexpr int MatchPositionsRole = Qt::UserRole + 1;
    // QIcon painted at the left of the row (optional).
    static constexpr int LeadingIconRole = Qt::UserRole + 2;
    // QString painted right-aligned in a dimmed color (optional, never fuzzy).
    static constexpr int TrailingTextRole = Qt::UserRole + 3;

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override;
    QSize sizeHint(const QStyleOptionViewItem &option,
                   const QModelIndex &index) const override;
};
