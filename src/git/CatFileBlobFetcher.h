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

#include "GitProcessRunner.h"

#include <QByteArray>
#include <QObject>
#include <QString>

#include <cstdint>

class CatFileBlobFetcher : public QObject
{
    Q_OBJECT
public:
    explicit CatFileBlobFetcher(QObject *parent = nullptr);
    ~CatFileBlobFetcher() override;

    void fetch(const QString &runnerScope, const QString &cacheRepoKey,
               const QString &repoToplevel, const QString &relPath);
    void cancel();

signals:
    void blobReady(const QString &repoToplevel, const QString &relPath,
                   const QByteArray &blob);
    void blobFailed(const QString &repoToplevel, const QString &relPath,
                    const QString &message);

private:
    IGitProcessRunner *m_runner = nullptr;
    quint64 m_generation = 0;
    quint64 m_inFlight   = 0;
    QString m_cacheScope;
    QString m_repoTop;
    QString m_relPath;
};
