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

#include "CatFileBlobFetcher.h"

#include "GitBaseBlobCache.h"
#include "GitRunnerFactory.h"

namespace {
constexpr int kTimeoutMs = 5000;
} // namespace

CatFileBlobFetcher::CatFileBlobFetcher(QObject *parent) : QObject(parent) {}

CatFileBlobFetcher::~CatFileBlobFetcher() { cancel(); }

void CatFileBlobFetcher::cancel()
{
    ++m_generation;
    if (m_runner) {
        m_runner->cancelAsync();
        m_runner->asQObject()->deleteLater();
        m_runner = nullptr;
    }
}

void CatFileBlobFetcher::fetch(const QString &runnerScope, const QString &cacheRepoKey,
                               const QString &repoToplevel, const QString &relPath)
{
    cancel();
    if (repoToplevel.isEmpty() || relPath.isEmpty()) {
        emit blobFailed(repoToplevel, relPath, QStringLiteral("invalid arguments"));
        return;
    }

    m_inFlight = ++m_generation;
    m_repoTop = repoToplevel;
    m_relPath = relPath;
    m_cacheScope = cacheRepoKey.isEmpty() ? repoToplevel : cacheRepoKey;

    const QString scope = runnerScope.isEmpty() ? repoToplevel : runnerScope;
    m_runner = GitRunnerFactory::createForRepo(scope, this);

    const QStringList argv = {
        QStringLiteral("-c"), QStringLiteral("core.quotepath=false"),
        QStringLiteral("-C"), repoToplevel,
        QStringLiteral("cat-file"), QStringLiteral("blob"),
        QStringLiteral("HEAD:") + relPath,
    };

    const quint64 gen = m_inFlight;
    m_runner->run(QString(), argv, QByteArray(), kTimeoutMs, false,
                  [this, gen](int exit, const QByteArray &out, const QByteArray &err) {
        if (gen != m_generation) return;
        m_runner->asQObject()->deleteLater();
        m_runner = nullptr;
        if (exit != 0) {
            emit blobFailed(m_cacheScope, m_relPath, QString::fromUtf8(err));
            return;
        }
        GitBaseBlobCache::instance().put(m_cacheScope, m_relPath, out);
        emit blobReady(m_cacheScope, m_relPath, out);
    });
}