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

#ifndef CSVFILTERSTRIP_H
#define CSVFILTERSTRIP_H

#include <QList>

#include <functional>

class QLineEdit;

// Pure field-list reconciliation for the CSV preview's per-column filter strip.
// Extracted from CsvPreviewWidget so the off-by-one-prone splice/erase/reconcile
// arithmetic is unit-testable in isolation (mirrors the CsvSortFilter extraction).
// CsvPreviewWidget's columnsInserted / columnsRemoved / modelReset handlers
// DELEGATE here — they keep no inline copy of the arithmetic — so the SAME code
// that ships is exactly what the tests exercise. `factory` mints one fully
// wired+styled filter field; `destroy` tears one down (production passes `delete`).
namespace CsvFilterStrip {

// columnsInserted(first,last): splice (last-first+1) fresh fields in AT `first` so
// every existing field keeps riding its own column — NOT a tail append, which would
// leave a typed filter glued to the column the insert displaced. `first` is clamped
// into [0, fields.size()]; an empty/negative span is a no-op.
void spliceInserted(QList<QLineEdit *> &fields, int first, int last,
                    const std::function<QLineEdit *()> &factory);

// columnsRemoved(first,last): destroy the fields over the clamped [first,last] range
// (high→low so indices stay stable) AND erase their slots, so no widget leaks and
// the survivors stay index-aligned with their columns.
void spliceRemoved(QList<QLineEdit *> &fields, int first, int last,
                   const std::function<void(QLineEdit *)> &destroy);

// Wholesale rebuild arithmetic: grow (append factory()) or shrink (destroy from the
// tail) until the list holds exactly `targetCount` fields. Position is irrelevant
// here — the only caller (the reset/rebuild path) has already discarded filter text,
// so tail reconciliation is correct. `targetCount` < 0 is treated as 0.
void reconcileCount(QList<QLineEdit *> &fields, int targetCount,
                    const std::function<QLineEdit *()> &factory,
                    const std::function<void(QLineEdit *)> &destroy);

} // namespace CsvFilterStrip

#endif // CSVFILTERSTRIP_H
