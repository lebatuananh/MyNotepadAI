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

#include "LocalExecutionContext.h"

#include "IFileSystemBackend.h"

#include "iptyprocess.h"
#include "ptyqt.h"

#include "GitProcessRunner.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QProcess>
#include <QPointer>
#include <QTimer>

namespace remote {

// QFile-backed local filesystem backend. Real and exercised in Phase 1 so the
// fsBackend() seam carries zero regression; the remote SFTP backend (P2) plugs
// into the same IFileSystemBackend interface.
class LocalFileSystemBackend : public IFileSystemBackend
{
public:
    explicit LocalFileSystemBackend(QObject *parent = nullptr)
        : IFileSystemBackend(parent)
        , m_watcher(new QFileSystemWatcher(this))
    {
        connect(m_watcher, &QFileSystemWatcher::directoryChanged,
                this, &IFileSystemBackend::directoryChanged);
    }

    QByteArray readFile(const QString &path, bool *ok) override
    {
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly)) {
            if (ok) *ok = false;
            return {};
        }
        const QByteArray data = f.readAll();
        if (ok) *ok = (f.error() == QFileDevice::NoError);
        return data;
    }

    bool writeFile(const QString &path, const QByteArray &data) override
    {
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            return false;
        }
        const qint64 n = f.write(data);
        f.close();
        return n == data.size();
    }

    FileStat stat(const QString &path) override
    {
        FileStat s;
        QFileInfo info(path);
        s.exists = info.exists();
        s.isDir = info.isDir();
        s.size = info.size();
        s.lastModified = info.lastModified();
        return s;
    }

    QStringList readdir(const QString &path) override
    {
        return QDir(path).entryList(QDir::NoDotAndDotDot | QDir::AllEntries);
    }

private:
    QFileSystemWatcher *m_watcher;
};

LocalExecutionContext::LocalExecutionContext(QObject *parent)
    : ExecutionContext(parent)
{
}

LocalExecutionContext::~LocalExecutionContext() = default;

QString LocalExecutionContext::displayName() const
{
    return tr("Local");
}

IPtyProcess *LocalExecutionContext::createPty(QObject *parent)
{
    // The EXACT object the terminal constructed before this change
    // (TerminalWidget.cpp previously called PtyQt::createPtyProcess(AutoPty)
    // inline). IPtyProcess is a QObject; re-parent so the caller owns it.
    IPtyProcess *pty = PtyQt::createPtyProcess(IPtyProcess::AutoPty);
    if (pty && parent) {
        pty->setParent(parent);
    }
    return pty;
}

IGitProcessRunner *LocalExecutionContext::createGitRunner(QObject *parent)
{
    return new GitProcessRunner(parent);
}

void LocalExecutionContext::exec(const QString &cwd,
                                 const QStringList &argv,
                                 const QByteArray &stdinPayload,
                                 int timeoutMs,
                                 ExecCallback cb)
{
    if (argv.isEmpty()) {
        if (cb) cb(-1, {}, QByteArrayLiteral("empty argv"));
        return;
    }

    auto *proc = new QProcess(this);
    if (!cwd.isEmpty()) {
        proc->setWorkingDirectory(cwd);
    }
    proc->setProgram(argv.first());
    proc->setArguments(argv.mid(1));

    QPointer<QProcess> guard(proc);
    auto *timer = new QTimer(proc);
    timer->setSingleShot(true);

    connect(proc, &QProcess::finished, proc,
            [proc, cb](int exitCode, QProcess::ExitStatus status) {
                if (cb) {
                    cb(status == QProcess::CrashExit ? -1 : exitCode,
                       proc->readAllStandardOutput(),
                       proc->readAllStandardError());
                }
                proc->deleteLater();
            });
    connect(proc, &QProcess::errorOccurred, proc,
            [proc, cb](QProcess::ProcessError) {
                if (proc->state() == QProcess::NotRunning) {
                    if (cb) cb(-1, {}, proc->errorString().toUtf8());
                    proc->deleteLater();
                }
            });
    if (timeoutMs > 0) {
        connect(timer, &QTimer::timeout, proc, [guard]() {
            if (guard) guard->kill();
        });
    }

    proc->start();
    if (!stdinPayload.isEmpty()) {
        proc->write(stdinPayload);
    }
    proc->closeWriteChannel();
    if (timeoutMs > 0) {
        timer->start(timeoutMs);
    }
}

IFileSystemBackend *LocalExecutionContext::fsBackend()
{
    if (!m_fsBackend) {
        m_fsBackend = new LocalFileSystemBackend(this);
    }
    return m_fsBackend;
}

QString LocalExecutionContext::resolveCwd(const QString &requested) const
{
    if (requested.isEmpty()) {
        return {};
    }
    return QDir::cleanPath(requested);
}

} // namespace remote
