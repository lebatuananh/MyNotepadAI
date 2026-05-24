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
#include "GitProcessRunner.h"

#include <QProcess>

namespace {
// `cat-file blob HEAD:<path>` rarely takes more than a few hundred ms even
// on slow disks; cap at 5 s so we don't hang forever on a wedged repo.
constexpr int kTimeoutMs = 5000;
} // namespace

CatFileBlobFetcher::CatFileBlobFetcher(QObject *parent) : QObject(parent) {}

CatFileBlobFetcher::~CatFileBlobFetcher()
{
    cancel();
}

void CatFileBlobFetcher::cancel()
{
    ++m_generation; // invalidate any in-flight result on completion
    if (m_proc) {
        m_proc->disconnect(this);
        if (m_proc->state() != QProcess::NotRunning) m_proc->kill();
        m_proc->deleteLater();
        m_proc = nullptr;
    }
}

void CatFileBlobFetcher::fetch(const QString &repoToplevel, const QString &relPath)
{
    if (repoToplevel.isEmpty() || relPath.isEmpty()) {
        emit blobFailed(repoToplevel, relPath, QStringLiteral("invalid arguments"));
        return;
    }

    cancel();
    m_inFlight = ++m_generation;
    m_repoTop = repoToplevel;
    m_relPath = relPath;

    const QString git = GitProcessRunner::gitExecutable();
    if (git.isEmpty()) {
        emit blobFailed(repoToplevel, relPath, QStringLiteral("git not found"));
        return;
    }

    m_proc = new QProcess(this);
    m_proc->setProgram(git);
    m_proc->setArguments({
        QStringLiteral("-c"), QStringLiteral("core.quotepath=false"),
        QStringLiteral("cat-file"), QStringLiteral("blob"),
        QStringLiteral("HEAD:") + relPath,
    });
    m_proc->setWorkingDirectory(repoToplevel);
    m_proc->setProcessEnvironment(GitProcessRunner::baseEnv());

    connect(m_proc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, &CatFileBlobFetcher::onFinished);
    connect(m_proc, &QProcess::errorOccurred, this, &CatFileBlobFetcher::onErrorOccurred);

    m_proc->start();
    if (!m_proc->waitForStarted(kTimeoutMs)) {
        // Treat start failure same as any other error path.
        onErrorOccurred();
    }
}

void CatFileBlobFetcher::onFinished()
{
    if (!m_proc) return;
    const quint64 gen = m_inFlight;
    const int exitCode = m_proc->exitCode();
    const QByteArray out = m_proc->readAllStandardOutput();
    const QByteArray err = m_proc->readAllStandardError();
    m_proc->deleteLater();
    m_proc = nullptr;

    // A newer fetch may have arrived while this one was running. Drop the
    // result silently so cache writes always reflect the latest request.
    if (gen != m_generation) return;

    if (exitCode != 0) {
        emit blobFailed(m_repoTop, m_relPath, QString::fromUtf8(err));
        return;
    }

    GitBaseBlobCache::instance().put(m_repoTop, m_relPath, out);
    emit blobReady(m_repoTop, m_relPath, out);
}

void CatFileBlobFetcher::onErrorOccurred()
{
    if (!m_proc) return;
    const quint64 gen = m_inFlight;
    const QString message = m_proc->errorString();
    m_proc->deleteLater();
    m_proc = nullptr;
    if (gen != m_generation) return;
    emit blobFailed(m_repoTop, m_relPath, message);
}
