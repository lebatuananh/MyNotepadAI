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

#include "SubmoduleStatusFetcher.h"

#include "GitProcessRunner.h"
#include "GitStatusParser.h"

#include <QProcess>
#include <QTimer>

namespace {
constexpr int kPerSubmoduleTimeoutMs = 30000;
} // namespace

SubmoduleStatusFetcher::SubmoduleStatusFetcher(QObject *parent) : QObject(parent) {}

SubmoduleStatusFetcher::~SubmoduleStatusFetcher()
{
    cancelAll();
}

void SubmoduleStatusFetcher::cancelAll()
{
    // Bump generation so any late-finishing process is ignored. Detach and
    // kill in-flight processes; their finished signal will land in
    // onTaskFinished, see the stale generation, and clean themselves up.
    ++m_generation;
    for (Task *t : m_tasks) {
        if (t->proc && t->proc->state() != QProcess::NotRunning) {
            t->proc->disconnect(this);
            t->proc->kill();
            t->proc->deleteLater();
        } else if (t->proc) {
            t->proc->deleteLater();
        }
        delete t;
    }
    m_tasks.clear();
    m_pending.clear();
    m_inflight = 0;
}

void SubmoduleStatusFetcher::fetch(const QVector<Submodule> &submodules)
{
    cancelAll();

    if (submodules.isEmpty() || GitProcessRunner::gitExecutable().isEmpty()) {
        // Single-shot empty emission on the next tick so callers see a
        // deterministic signal regardless of whether anything spawned.
        QTimer::singleShot(0, this, [this]() {
            emit entriesReady({});
        });
        return;
    }

    const int gen = m_generation;
    m_inflight = submodules.size();
    m_tasks.reserve(submodules.size());
    for (const Submodule &s : submodules) {
        startOne(s, gen);
    }
}

void SubmoduleStatusFetcher::startOne(const Submodule &sub, int generation)
{
    auto *t = new Task;
    t->relFromRoot = sub.relFromRoot;
    t->generation = generation;
    t->proc = new QProcess(this);

    const QString gitExe = GitProcessRunner::gitExecutable();
    const QStringList argv = {
        QStringLiteral("-c"), QStringLiteral("core.quotepath=false"),
        QStringLiteral("status"), QStringLiteral("--porcelain=v2"),
        QStringLiteral("-z"),
        QStringLiteral("--untracked-files=all"),
        QStringLiteral("--renames"),
    };
    t->proc->setProgram(gitExe);
    t->proc->setArguments(argv);
    t->proc->setWorkingDirectory(sub.absPath);
    t->proc->setProcessEnvironment(GitProcessRunner::baseEnv());
    t->proc->setProcessChannelMode(QProcess::SeparateChannels);

    connect(t->proc, &QProcess::readyReadStandardOutput, this, [t]() {
        t->stdoutBuf.append(t->proc->readAllStandardOutput());
    });
    connect(t->proc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, [this, t](int code, QProcess::ExitStatus status) {
        // Drain any remaining stdout the readyRead signal hasn't dispatched yet.
        t->stdoutBuf.append(t->proc->readAllStandardOutput());
        const int effectiveExit = (status == QProcess::CrashExit) ? -1 : code;
        onTaskFinished(t, effectiveExit);
    });
    connect(t->proc, &QProcess::errorOccurred, this, [this, t](QProcess::ProcessError) {
        // Errors map to "skip this submodule" — drain whatever we have and
        // finish so other submodules still count down.
        if (t->generation != m_generation) return;
        t->stdoutBuf.append(t->proc->readAllStandardOutput());
        onTaskFinished(t, -1);
    });

    // Per-task timeout: kill the process; finished() will fire with -1.
    QTimer::singleShot(kPerSubmoduleTimeoutMs, t->proc, [t]() {
        if (t->proc && t->proc->state() != QProcess::NotRunning) {
            t->proc->kill();
        }
    });

    m_tasks.append(t);
    t->proc->start();
}

void SubmoduleStatusFetcher::onTaskFinished(Task *t, int exitCode)
{
    if (t->generation != m_generation) {
        // Stale completion from a cancelled round — discard silently.
        if (t->proc) t->proc->deleteLater();
        return;
    }

    if (exitCode == 0 && !t->stdoutBuf.isEmpty()) {
        GitStatusEntries inner = GitStatusParser::parsePorcelainV2(t->stdoutBuf, nullptr);
        // Rewrite each entry's relPath to be relative to the parent workspace
        // root by prefixing the submodule's relative path. origRelPath (for
        // renames) gets the same treatment.
        const QString prefix = t->relFromRoot + QLatin1Char('/');
        m_pending.reserve(m_pending.size() + inner.size());
        for (GitStatusEntry &e : inner) {
            if (!e.relPath.isEmpty()) e.relPath = prefix + e.relPath;
            if (!e.origRelPath.isEmpty()) e.origRelPath = prefix + e.origRelPath;
            m_pending.append(std::move(e));
        }
    }
    // Note: exitCode != 0 (e.g. submodule directory is empty / not a repo)
    // is silently treated as "no entries". This matches the user-visible
    // outcome of no decorations for that subtree, which is preferable to a
    // hard error.

    if (t->proc) t->proc->deleteLater();
    t->proc = nullptr;

    --m_inflight;
    if (m_inflight <= 0) {
        const GitStatusEntries out = std::move(m_pending);
        m_pending.clear();
        // Free the task storage before emitting so subscribers calling
        // fetch() again from the slot see a clean state.
        for (Task *task : m_tasks) delete task;
        m_tasks.clear();
        emit entriesReady(out);
    }
}
