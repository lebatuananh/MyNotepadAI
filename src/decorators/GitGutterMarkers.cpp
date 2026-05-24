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

#include "GitGutterMarkers.h"

GitGutterMarkers::Lines
GitGutterMarkers::linesFromHunks(const BufferDiffEngine::Hunks &hunks)
{
    Lines out;
    const qint32 n = static_cast<qint32>(hunks.size());
    if (n == 0) return out;

    // Pre-reserve based on a hunk-count heuristic — most edits are 1-2 lines
    // per hunk, so this is a tight bound and avoids realloc churn in the
    // per-keystroke path.
    out.added.reserve(n);
    out.modified.reserve(n);
    out.deletedAt.reserve(n);
    out.lineToHunkIdx.reserve(n * 2);

    for (qint32 i = 0; i < n; ++i) {
        const auto &h = hunks.at(i);

        if (h.oldCount > 0 && h.newCount > 0) {
            // Modified: marker on every buffer line covered.
            for (qint32 ln = h.newStart; ln < h.newStart + h.newCount; ++ln) {
                out.modified.append(ln);
                out.lineToHunkIdx.insert(ln, i);
            }
        } else if (h.oldCount == 0 && h.newCount > 0) {
            // Pure Added.
            for (qint32 ln = h.newStart; ln < h.newStart + h.newCount; ++ln) {
                out.added.append(ln);
                out.lineToHunkIdx.insert(ln, i);
            }
        } else if (h.oldCount > 0 && h.newCount == 0) {
            // Pure Deleted: anchor on the line BEFORE the deletion in the
            // buffer; clamp to 0 when the deletion is at the top.
            const qint32 anchor = h.newStart > 0 ? h.newStart - 1 : 0;
            out.deletedAt.append(anchor);
            out.lineToHunkIdx.insert(anchor, i);
        }
        // Both zero is a degenerate empty hunk — never emitted by xdl_diff.
    }

    return out;
}
