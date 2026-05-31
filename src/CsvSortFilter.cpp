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

#include "CsvSortFilter.h"

#include <algorithm>
#include <cmath>

namespace CsvSortFilter {

namespace {

// `^-?0[0-9]` — a leading zero immediately followed by another digit (ZIP code,
// phone number, fixed-width id). "0" alone and "0.5" are NOT matched, so plain
// zero and decimals below one stay numeric. Hand-rolled (no QRegularExpression)
// to keep the sampled hot path allocation-free.
bool hasLeadingZero(const QString &s)
{
    qsizetype i = 0;
    if (i < s.size() && s.at(i) == QLatin1Char('-'))
        ++i;
    return i + 1 < s.size() && s.at(i) == QLatin1Char('0') && s.at(i + 1).isDigit();
}

// Whole-string numeric parse. QString::toDouble uses the C locale (decimal point,
// no group separators) and reports false on trailing garbage, so this is a
// locale-independent full consume. Non-finite (inf/nan) is rejected so the sort
// comparator never sees a value that breaks strict-weak-ordering.
bool parseNumber(const QString &s, double &out)
{
    if (s.isEmpty())
        return false;
    bool ok = false;
    out = s.toDouble(&ok);
    return ok && std::isfinite(out);
}

// Deterministic string→double for the comparator (column already classified
// numeric). A non-empty cell that slipped past the bounded sample without
// parsing maps to 0.0 — finite and stable, so the ordering stays a valid
// strict-weak-ordering even on data the sample didn't cover.
double comparatorNumber(const QString &s)
{
    bool ok = false;
    const double d = s.toDouble(&ok);
    return (ok && std::isfinite(d)) ? d : 0.0;
}

} // namespace

bool isNumericColumn(const CellReader &cellAt, quint64 rowCount, int col)
{
    constexpr int kSample = 100;
    int seen = 0;
    for (quint64 r = 0; r < rowCount && seen < kSample; ++r) {
        const QString v = cellAt(r, col);
        if (v.isEmpty())
            continue;                  // empties/ragged don't decide the type
        if (hasLeadingZero(v))
            return false;              // ZIP/phone-style value → text column
        double d;
        if (!parseNumber(v, d))
            return false;              // any non-number → text column
        ++seen;
    }
    return seen > 0;                   // all-empty column → text (order moot)
}

QVector<quint32> computeViewOrder(const CellReader &cellAt, quint64 rowCount, int colCount,
                                  const QStringList &filters, int sortColumn,
                                  Qt::SortOrder order, const std::atomic<bool> &cancel)
{
    QVector<quint32> result;
    result.reserve(static_cast<qsizetype>(rowCount));

    // Compact the active (non-empty, in-range) filters once so the per-row loop
    // tests only the columns that actually filter — O(R · A), A = active count.
    struct ActiveFilter { int col; QString needle; };
    QVector<ActiveFilter> active;
    for (int c = 0; c < filters.size() && c < colCount; ++c) {
        const QString &f = filters.at(c);
        if (!f.isEmpty())
            active.append({c, f});
    }

    // Filter pass: retain rows matching ALL active filters (case-insensitive
    // substring, ANDed). Poll cancel every 64K rows so a superseded run bails.
    for (quint64 r = 0; r < rowCount; ++r) {
        if ((r & 0xFFFFu) == 0 && cancel.load(std::memory_order_relaxed))
            return {};
        bool keep = true;
        for (const ActiveFilter &af : active) {
            if (!cellAt(r, af.col).contains(af.needle, Qt::CaseInsensitive)) {
                keep = false;
                break;
            }
        }
        if (keep)
            result.append(static_cast<quint32>(r));
    }

    // Sort pass: stable, numeric-or-lexicographic, empties last in BOTH
    // directions. Keys are not precomputed for v1 (R ≤ 256K within budget).
    if (sortColumn >= 0 && sortColumn < colCount && result.size() > 1) {
        const bool numeric = isNumericColumn(cellAt, rowCount, sortColumn);
        const bool ascending = (order == Qt::AscendingOrder);
        std::stable_sort(result.begin(), result.end(), [&](quint32 a, quint32 b) -> bool {
            const QString va = cellAt(a, sortColumn);
            const QString vb = cellAt(b, sortColumn);
            const bool ea = va.isEmpty();
            const bool eb = vb.isEmpty();
            // Empty/missing/ragged ⇒ sort last regardless of direction. An empty
            // is never "before" a non-empty; two empties are equivalent (stable).
            if (ea || eb) {
                if (ea && eb) return false;
                return eb;              // a before b only when b is the empty one
            }
            int cmp;
            if (numeric) {
                const double da = comparatorNumber(va);
                const double db = comparatorNumber(vb);
                cmp = (da < db) ? -1 : (da > db ? 1 : 0);
            } else {
                cmp = va.compare(vb, Qt::CaseInsensitive);
            }
            if (cmp == 0) return false; // equal keys keep original order (stable)
            return ascending ? (cmp < 0) : (cmp > 0);
        });
    }

    return result;
}

} // namespace CsvSortFilter
