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
#include <QObject>
#include <QString>

#include <cstdint>

class QProcess;

// Async fetch of `HEAD:<relPath>` raw bytes, populating GitBaseBlobCache.
//
// Standalone path (no GitController coupling) so editors that live outside
// any workspace — or that need their blob ahead of the workspace finishing
// its own queue — can still get a base blob for the buffer-diff engine.
// Calling code that already owns a GitController can use
// GitController::requestCatFileBlob instead; the two paths populate the same
// cache.
//
// Cancellation: each request bumps an internal generation counter. A pending
// process whose generation no longer matches is killed and its result
// dropped. This matters because we re-fetch on every save and the user may
// trigger N saves in quick succession.
class CatFileBlobFetcher : public QObject
{
    Q_OBJECT
public:
    explicit CatFileBlobFetcher(QObject *parent = nullptr);
    ~CatFileBlobFetcher() override;

    // Kick off a fetch. `repoToplevel` must be the absolute path to the
    // working tree root (`git rev-parse --show-toplevel`); `relPath` is
    // relative to that root. On success the blob is written into
    // GitBaseBlobCache AND emitted via blobReady. On failure the cache is
    // not touched and blobFailed is emitted.
    void fetch(const QString &repoToplevel, const QString &relPath);

    // Cancel any in-flight fetch. Safe to call repeatedly.
    void cancel();

signals:
    void blobReady(const QString &repoToplevel, const QString &relPath,
                   const QByteArray &blob);
    void blobFailed(const QString &repoToplevel, const QString &relPath,
                    const QString &message);

private slots:
    void onFinished();
    void onErrorOccurred();

private:
    QProcess *m_proc = nullptr;
    quint64   m_generation = 0; // bumped on each fetch + cancel
    quint64   m_inFlight   = 0; // generation belonging to m_proc
    QString   m_repoTop;
    QString   m_relPath;
};
