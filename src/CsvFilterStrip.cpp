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

#include "CsvFilterStrip.h"

#include <QLineEdit>

#include <algorithm>

namespace CsvFilterStrip {

void spliceInserted(QList<QLineEdit *> &fields, int first, int last,
                    const std::function<QLineEdit *()> &factory)
{
    if (first < 0 || last < first || !factory) return;
    const int count = last - first + 1;
    // Clamp into the current list so a splice past the end still lands as an append
    // (defensive; first should be <= size for a valid model insert).
    int at = std::min(first, static_cast<int>(fields.size()));
    for (int i = 0; i < count; ++i)
        fields.insert(at++, factory());
}

void spliceRemoved(QList<QLineEdit *> &fields, int first, int last,
                   const std::function<void(QLineEdit *)> &destroy)
{
    if (first < 0 || last < first || fields.isEmpty() || !destroy) return;
    const int lo = std::max(0, first);
    const int hi = std::min(last, static_cast<int>(fields.size()) - 1);
    if (lo > hi) return;
    for (int c = hi; c >= lo; --c) {
        QLineEdit *f = fields.takeAt(c);
        destroy(f);
    }
}

void reconcileCount(QList<QLineEdit *> &fields, int targetCount,
                    const std::function<QLineEdit *()> &factory,
                    const std::function<void(QLineEdit *)> &destroy)
{
    const int target = std::max(0, targetCount);
    while (fields.size() > target && destroy)
        destroy(fields.takeLast());
    while (fields.size() < target && factory)
        fields.append(factory());
}

} // namespace CsvFilterStrip
