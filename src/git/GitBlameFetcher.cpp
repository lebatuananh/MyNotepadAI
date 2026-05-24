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

#include "GitBlameFetcher.h"

#include "GitProcessRunner.h"

#include <QProcess>

namespace {
// Blame can take seconds on a long history file; 15 s is the same budget
// the diff view uses.
constexpr int kTimeoutMs = 15000;
} // namespace

GitBlameFetcher::GitBlameFetcher(QObject *parent) : QObject(parent) {}

GitBlameFetcher::~GitBlameFetcher() { cancel(); }

void GitBlameFetcher::cancel()
{
    ++m_generation;
    if (m_proc) {
        m_proc->disconnect(this);
        if (m_proc->state() != QProcess::NotRunning) m_proc->kill();
        m_proc->deleteLater();
        m_proc = nullptr;
    }
}

void GitBlameFetcher::fetch(const QString &repoToplevel, const QString &relPath)
{
    if (repoToplevel.isEmpty() || relPath.isEmpty()) {
        emit blameFailed(repoToplevel, relPath, QStringLiteral("invalid arguments"));
        return;
    }

    cancel();
    m_inFlight = ++m_generation;
    m_repoTop = repoToplevel;
    m_relPath = relPath;

    const QString git = GitProcessRunner::gitExecutable();
    if (git.isEmpty()) {
        emit blameFailed(repoToplevel, relPath, QStringLiteral("git not found"));
        return;
    }

    m_proc = new QProcess(this);
    m_proc->setProgram(git);
    m_proc->setArguments({
        QStringLiteral("-c"), QStringLiteral("core.quotepath=false"),
        QStringLiteral("blame"),
        QStringLiteral("--porcelain"),
        QStringLiteral("HEAD"),
        QStringLiteral("--"),
        relPath,
    });
    m_proc->setWorkingDirectory(repoToplevel);
    m_proc->setProcessEnvironment(GitProcessRunner::baseEnv());

    connect(m_proc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, &GitBlameFetcher::onFinished);
    connect(m_proc, &QProcess::errorOccurred, this, &GitBlameFetcher::onErrorOccurred);

    m_proc->start();
    if (!m_proc->waitForStarted(kTimeoutMs)) onErrorOccurred();
}

void GitBlameFetcher::onFinished()
{
    if (!m_proc) return;
    const quint64 gen = m_inFlight;
    const int exitCode = m_proc->exitCode();
    const QByteArray out = m_proc->readAllStandardOutput();
    const QByteArray err = m_proc->readAllStandardError();
    m_proc->deleteLater();
    m_proc = nullptr;

    if (gen != m_generation) return;

    if (exitCode != 0) {
        emit blameFailed(m_repoTop, m_relPath, QString::fromUtf8(err));
        return;
    }

    // Parse synchronously — porcelain decoding is fast and the main UI
    // thread is the only consumer, so dispatching across threads would just
    // add latency.
    const auto result = GitBlameParser::parse(out);
    emit blameReady(m_repoTop, m_relPath, result);
}

void GitBlameFetcher::onErrorOccurred()
{
    if (!m_proc) return;
    const quint64 gen = m_inFlight;
    const QString message = m_proc->errorString();
    m_proc->deleteLater();
    m_proc = nullptr;
    if (gen != m_generation) return;
    emit blameFailed(m_repoTop, m_relPath, message);
}
