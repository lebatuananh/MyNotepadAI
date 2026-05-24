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

#include "GitBaseBlobCache.h"

#include <QMutexLocker>

#include "xxhash.h"

GitBaseBlobCache::GitBaseBlobCache() = default;
GitBaseBlobCache::~GitBaseBlobCache() = default;

GitBaseBlobCache &GitBaseBlobCache::instance()
{
    // Magic-static: thread-safe init in C++11+, zero overhead after first
    // call. The destructor runs at static teardown — fine, by then no thread
    // is still hammering the cache.
    static GitBaseBlobCache s;
    return s;
}

quint64 GitBaseBlobCache::computeKey(const QString &repoRoot, const QString &relPath)
{
    // Serialise (repoRoot, relPath) into a transient byte buffer for xxHash.
    // A NUL separator makes the join unambiguous — repoRoot can't end in a
    // path-separator that would collide with the start of relPath. The
    // buffer lives on the stack for small paths; larger paths heap-allocate
    // once per call (still well under microsecond territory).
    QByteArray buf;
    buf.reserve(repoRoot.size() + relPath.size() + 1);
    buf += repoRoot.toUtf8();
    buf += '\0';
    buf += relPath.toUtf8();
    return XXH64(buf.constData(), static_cast<size_t>(buf.size()), /*seed=*/0);
}

QByteArray GitBaseBlobCache::get(const QString &repoRoot, const QString &relPath)
{
    const quint64 key = computeKey(repoRoot, relPath);
    QMutexLocker locker(&m_mutex);
    const auto it = m_map.constFind(key);
    if (it == m_map.constEnd()) return {};

    // Confirm the underlying strings match — guards against the (vanishingly
    // unlikely) xxHash collision before we hand back somebody else's blob.
    const auto &listIt = it.value();
    if (listIt->repoRoot != repoRoot || listIt->relPath != relPath) {
        return {};
    }
    // Promote to most-recently-used: O(1) splice within the same list.
    m_lru.splice(m_lru.begin(), m_lru, listIt);
    return listIt->blob;
}

void GitBaseBlobCache::put(const QString &repoRoot, const QString &relPath, QByteArray blob)
{
    const quint64 key = computeKey(repoRoot, relPath);
    const qsizetype incoming = blob.size();

    QMutexLocker locker(&m_mutex);

    // Replace existing entry in place: keeps hash position, just refresh
    // the blob and promote it. Saves us a remove+insert churn.
    if (const auto it = m_map.find(key); it != m_map.end()) {
        auto listIt = it.value();
        if (listIt->repoRoot == repoRoot && listIt->relPath == relPath) {
            m_bytes -= listIt->bytes;
            listIt->blob  = std::move(blob);
            listIt->bytes = incoming;
            m_bytes += incoming;
            m_lru.splice(m_lru.begin(), m_lru, listIt);
            evictLocked(0);
            return;
        }
        // Hash collision with a different string pair — let the new entry
        // displace the colliding one; we don't try to chain.
        m_bytes -= listIt->bytes;
        m_lru.erase(listIt);
        m_map.erase(it);
    }

    // Make room before insertion so we never blip above the configured cap.
    evictLocked(incoming);

    m_lru.push_front(Entry{key, repoRoot, relPath, std::move(blob), incoming});
    m_map.insert(key, m_lru.begin());
    m_bytes += incoming;
}

void GitBaseBlobCache::invalidate(const QString &repoRoot, const QString &relPath)
{
    const quint64 key = computeKey(repoRoot, relPath);
    QMutexLocker locker(&m_mutex);
    const auto it = m_map.find(key);
    if (it == m_map.end()) return;
    auto listIt = it.value();
    if (listIt->repoRoot != repoRoot || listIt->relPath != relPath) return;
    m_bytes -= listIt->bytes;
    m_lru.erase(listIt);
    m_map.erase(it);
}

void GitBaseBlobCache::invalidateRepo(const QString &repoRoot)
{
    QMutexLocker locker(&m_mutex);
    // O(n) sweep. Triggered on watcher events (HEAD ref change, branch
    // switch) — they fire seconds-apart at most, so the scan cost is
    // irrelevant relative to the IO that triggered it.
    for (auto it = m_lru.begin(); it != m_lru.end(); ) {
        if (it->repoRoot == repoRoot) {
            m_bytes -= it->bytes;
            m_map.remove(it->key);
            it = m_lru.erase(it);
        } else {
            ++it;
        }
    }
}

void GitBaseBlobCache::clear()
{
    QMutexLocker locker(&m_mutex);
    m_map.clear();
    m_lru.clear();
    m_bytes = 0;
}

qsizetype GitBaseBlobCache::byteCount() const
{
    QMutexLocker locker(&m_mutex);
    return m_bytes;
}

qsizetype GitBaseBlobCache::maxBytes() const
{
    QMutexLocker locker(&m_mutex);
    return m_maxBytes;
}

void GitBaseBlobCache::setMaxBytes(qsizetype bytes)
{
    QMutexLocker locker(&m_mutex);
    m_maxBytes = bytes < 0 ? 0 : bytes;
    evictLocked(0);
}

void GitBaseBlobCache::evictLocked(qsizetype headroom)
{
    while (!m_lru.empty() && m_bytes + headroom > m_maxBytes) {
        auto &victim = m_lru.back();
        m_bytes -= victim.bytes;
        m_map.remove(victim.key);
        m_lru.pop_back();
    }
}
