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

#ifndef PATH_STATUS_INDEX_H
#define PATH_STATUS_INDEX_H

#include "GitStatusEntry.h"

#include <QHash>
#include <QSet>
#include <QString>

#include <optional>

// Pure value object: indexes a GitStatusEntries vector into two O(1) maps —
// one per-file (absolute cleanPath → Change) and one per-ancestor-folder
// (absolute cleanPath → "worst" descendant Change). Used by
// FolderAsWorkspaceFsModel to colorize tree rows in O(1) per visible cell.
//
// All paths stored and queried in QDir::cleanPath form (forward-slash, no
// trailing slash, no `.`/`..`). Lookups normalize the input themselves so
// callers can pass either native or Qt-cleaned paths.
//
// Folder rollup precomputed at rebuild time (priority order:
// Unmerged > Deleted > Modified > Added > Renamed > Copied > TypeChanged > Untracked_).
// Rebuild cost is O(N * average_depth); lookups O(1).
//
// Entries flagged with `isSubmodule==true` register the path as a folder
// rather than a file (the path on disk is a directory). They do NOT propagate
// to descendants — accurate per-file colours inside a submodule come from a
// separate `git status` invocation inside that submodule's working tree,
// whose entries are appended to the parent's before calling rebuild.
//
// Not thread-safe — rebuild + lookups on the UI thread only.
class PathStatusIndex
{
public:
    PathStatusIndex() = default;

    // Replace the index contents with entries from a fresh status snapshot.
    // `repoToplevel` is the absolute cleanPath of the repo root; ancestors
    // are walked up to (but NOT including) this path so the workspace root
    // itself is never decorated.
    void rebuild(const GitStatusEntries &entries, const QString &repoToplevel);

    void clear();

    // O(1) lookups. Input is normalized via QDir::cleanPath internally.
    // Returns nullopt when the path is not in the index (clean state).
    std::optional<GitStatusEntry::Change> fileChange(const QString &path) const;
    std::optional<GitStatusEntry::Change> folderChange(const QString &path) const;

    // Set-symmetric-difference between this index and `previous`, returning
    // every (cleanPath) whose value (file lookup OR folder lookup) differs
    // or is present in only one. Used to drive selective dataChanged
    // emission so an update touching 50 paths repaints 50 cells, not 5000.
    QSet<QString> deltaPaths(const PathStatusIndex &previous) const;

    // Snapshot of every path currently indexed (files + folders). Used when
    // the palette or master toggle changes and we need to refresh every
    // currently-decorated row at once.
    QSet<QString> allIndexedPaths() const;

    bool isEmpty() const {
        return m_files.isEmpty() && m_folders.isEmpty();
    }
    int fileCount() const { return m_files.size(); }
    int folderCount() const { return m_folders.size(); }

    // Priority comparison — exposed for testing. Returns the "worse" of the
    // two changes; ties resolve to `a`. See PathStatusIndex.cpp for the
    // priority table.
    static GitStatusEntry::Change worseChange(GitStatusEntry::Change a,
                                              GitStatusEntry::Change b);

private:
    QHash<QString, GitStatusEntry::Change> m_files;
    QHash<QString, GitStatusEntry::Change> m_folders;
};

#endif // PATH_STATUS_INDEX_H
