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

#ifndef GIT_GUTTER_MARKERS_H
#define GIT_GUTTER_MARKERS_H

#include "../git/BufferDiffEngine.h"

#include <QHash>
#include <QVector>

#include <cstdint>

// Scintilla marker IDs for the editor gutter. Public so the scrollbar
// overview painter (HighlightedScrollBar) can walk the same markers when
// rendering its per-line ticks. Range 0–19 is otherwise unused by the app;
// 21–30 cover hidelines, bookmark, and the fold markers, so 16/17/18 stay
// safely out of the way.
namespace GitGutterMarkerIds {
constexpr int Added    = 16;
constexpr int Modified = 17;
constexpr int Deleted  = 18;
} // namespace GitGutterMarkerIds

// Pure conversion: BufferDiffEngine::Hunks (raw line-range diff) → three flat
// arrays of 0-indexed editor line numbers, one per gutter marker kind.
//
// Hunk classification:
//   * oldCount == 0 (Added)    — marker on every buffer line covered.
//   * newCount == 0 (Deleted)  — marker on the line BEFORE the deletion in
//                                the buffer (newStart-1), clamped to 0 when
//                                the deletion sits at top of file.
//   * both > 0   (Modified)    — marker on every buffer line covered.
//
// Lines are 0-indexed (Scintilla convention). BufferDiffEngine emits 0-indexed
// already (xdl_diff convention) — no offset needed.
class GitGutterMarkers
{
public:
    struct Lines {
        QVector<qint32> added;
        QVector<qint32> modified;
        QVector<qint32> deletedAt;

        // Side-table for click-to-diff: every 0-indexed editor line that has
        // a marker maps to an index into the input Hunks vector. The caller
        // (GitGutterDecorator) keeps that vector alive alongside `Lines` and
        // uses the index to look up oldStart/oldCount for slicing the base
        // blob when rendering the inline annotation.
        QHash<qint32, qint32> lineToHunkIdx;
    };

    // Pure function — allocates only the returned vectors.
    static Lines linesFromHunks(const BufferDiffEngine::Hunks &hunks);
};

#endif // GIT_GUTTER_MARKERS_H
