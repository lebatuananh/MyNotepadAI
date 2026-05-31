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

#ifndef CSVSORTFILTER_H
#define CSVSORTFILTER_H

#include <QString>
#include <QStringList>
#include <QVector>
#include <QtCore/qnamespace.h>

#include <atomic>
#include <functional>

// Pure, Qt-widget-free compute core for the CSV table preview's sort/filter view
// layer. It reads cells exclusively through a `CellReader` callback so the SAME
// comparison/filter truth runs on the UI thread (small sets) and on a worker
// (large sets), and so it unit-tests without any model/widget dependency. It
// never materializes a per-row container beyond the returned index list.
namespace CsvSortFilter {

// Reads the displayed value of a DATA cell. `dataRow` is a 0-based data-grid row
// (header-mode shift is the caller's concern), `col` a 0-based virtual column.
// Ragged / missing cells return an empty QString — which both the filter (never
// matches a non-empty needle) and the sort (empties sort last) treat correctly.
using CellReader = std::function<QString(quint64 dataRow, int col)>;

// Detect whether a column should sort numerically. Samples up to ~100 non-empty
// cells; the column is numeric IFF every sampled non-empty cell parses fully as
// a number (locale-independent, whole-string consume). A value matching
// `^-?0[0-9]` (leading-zero ZIP / phone code) forces the column to text even
// when it would otherwise parse as a number. O(sample) cell reads.
bool isNumericColumn(const CellReader &cellAt, quint64 rowCount, int col);

// Build the view order in one combined pass:
//   1. retain every data row that matches ALL active (non-empty) column filters
//      (case-insensitive substring, ANDed);
//   2. if `sortColumn >= 0`, std::stable_sort the retained indices by that
//      column's key — numeric or case-insensitive lexicographic per
//      isNumericColumn — with empty/missing/ragged cells ordered LAST in both
//      directions (and equal keys keeping their original relative order).
// `filters` is indexed by column (entries past its size / empty are inactive).
// `cancel` is polled during the filter pass; a cancelled run returns {} (the
// caller discards it via its generation counter). Returns DATA-row indices in
// displayed order. O(R·A + R log R), R ≤ kWindowSize so indices fit quint32.
QVector<quint32> computeViewOrder(const CellReader &cellAt, quint64 rowCount, int colCount,
                                  const QStringList &filters, int sortColumn,
                                  Qt::SortOrder order, const std::atomic<bool> &cancel);

} // namespace CsvSortFilter

#endif // CSVSORTFILTER_H
