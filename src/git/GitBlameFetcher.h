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

#include "GitBlameParser.h"

#include <QObject>
#include <QString>

#include <cstdint>

class QProcess;

// Async wrapper around `git blame --porcelain HEAD -- <relPath>`. One
// fetcher instance per editor — the inline-blame decorator owns it.
//
// Cancel-on-restart: every fetch() bumps an internal generation counter so
// rapid repeat calls (typical when the user resaves the file in quick
// succession, or switches between files) don't pile up multiple processes.
// The youngest call wins; older results are discarded silently.
class GitBlameFetcher : public QObject
{
    Q_OBJECT
public:
    explicit GitBlameFetcher(QObject *parent = nullptr);
    ~GitBlameFetcher() override;

    // Kick off a blame for `relPath` (relative to `repoToplevel`). On
    // success emits blameReady with the parsed result; on failure emits
    // blameFailed. The parser runs synchronously on the QProcess::finished
    // callback — porcelain decoding is O(bytes) and well under our gutter
    // refresh budget even for large files.
    void fetch(const QString &repoToplevel, const QString &relPath);

    // Cancel any in-flight blame. Safe to call repeatedly.
    void cancel();

signals:
    void blameReady(const QString &repoToplevel, const QString &relPath,
                    const GitBlameParser::Result &result);
    void blameFailed(const QString &repoToplevel, const QString &relPath,
                     const QString &message);

private slots:
    void onFinished();
    void onErrorOccurred();

private:
    QProcess *m_proc = nullptr;
    quint64   m_generation = 0;
    quint64   m_inFlight   = 0;
    QString   m_repoTop;
    QString   m_relPath;
};
