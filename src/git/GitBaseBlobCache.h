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

#include <QByteArray>
#include <QHash>
#include <QMutex>
#include <QString>

#include <cstdint>
#include <list>

// App-wide LRU cache of HEAD blobs, keyed by (repoRoot, relPath). A single
// cache instance is shared across every open editor — the buffer-diff engine
// hits it once per keystroke (per-file), so re-fetching blobs from disk would
// dominate the gutter hot path.
//
// Size policy: ~16 MB by default, evicted least-recently-used on insert. The
// limit is intentionally small — typical sessions touch a handful of source
// files at a time, and a 16 MB ceiling caps RSS even when the workspace
// spans many repos.
//
// Keying uses xxHash64 over (repoRoot, relPath) — fast (constant nanoseconds
// for typical path lengths), zero collisions in practice for this domain. We
// store the original strings only for invalidation lookups; reads are pure
// hash hits.
//
// Thread-safety: every public method is internally locked. The buffer-diff
// engine calls `get` from the editor thread; CatFileBlobFetcher calls `put`
// from worker threads; GitWatcher calls `invalidate*` from the watcher
// thread. A single QMutex covers everything — the critical sections are tiny
// (O(1) list splice + map operation) so contention is negligible.
class GitBaseBlobCache
{
public:
    // App-singleton. The cache is process-global because workspaces share
    // editor windows in NotepadADE — keeping it per-dock would cause
    // duplicate blobs for the same path.
    static GitBaseBlobCache &instance();

    // Returns the cached HEAD blob, or an empty QByteArray on miss. A
    // non-empty result is the *current* HEAD blob — if the cache is stale
    // versus disk, the caller is expected to invalidate first (GitWatcher
    // wires this for HEAD/index changes).
    //
    // QByteArray is implicitly-shared, so the return is a cheap refcount
    // bump even for multi-megabyte blobs.
    QByteArray get(const QString &repoRoot, const QString &relPath);

    // Insert or replace. If the cache is full, evicts LRU entries until the
    // new blob fits under the byte budget.
    void put(const QString &repoRoot, const QString &relPath, QByteArray blob);

    // Drop a single entry. No-op when missing.
    void invalidate(const QString &repoRoot, const QString &relPath);

    // Drop every entry for a given repo. Used on HEAD-ref change events.
    void invalidateRepo(const QString &repoRoot);

    // Drop everything. Used on settings change or test teardown.
    void clear();

    // Current/configured byte budget.
    qsizetype byteCount() const;
    qsizetype maxBytes() const;

    // Resize the budget. Shrinking immediately evicts LRU entries down to
    // the new budget. Defaults to 16 MiB.
    void setMaxBytes(qsizetype bytes);

private:
    GitBaseBlobCache();
    ~GitBaseBlobCache();
    GitBaseBlobCache(const GitBaseBlobCache &) = delete;
    GitBaseBlobCache &operator=(const GitBaseBlobCache &) = delete;

    struct Entry {
        quint64 key;          // xxHash64 of (repoRoot, relPath) — primary index
        QString repoRoot;     // retained for invalidateRepo scans
        QString relPath;      // retained for collision detection on get
        QByteArray blob;
        qsizetype bytes;      // == blob.size(), cached so eviction doesn't have to query
    };

    using List = std::list<Entry>;
    using Map  = QHash<quint64, List::iterator>;

    static quint64 computeKey(const QString &repoRoot, const QString &relPath);
    // MUST be called with m_mutex locked. Evicts from the back of m_lru
    // until `m_bytes + headroom <= m_maxBytes`. Returns nothing — silently
    // succeeds if the cache is already empty.
    void evictLocked(qsizetype headroom);

    mutable QMutex m_mutex;
    List   m_lru;   // front = most recently used; back = LRU victim
    Map    m_map;
    qsizetype m_bytes    = 0;
    qsizetype m_maxBytes = 16 * 1024 * 1024;
};
