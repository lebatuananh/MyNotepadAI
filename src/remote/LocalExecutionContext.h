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

#ifndef REMOTE_LOCALEXECUTIONCONTEXT_H
#define REMOTE_LOCALEXECUTIONCONTEXT_H

#include "ExecutionContext.h"

namespace remote {

class LocalFileSystemBackend;

// The local machine as an ExecutionContext. Always Connected. createPty()
// returns the exact PtyQt object the terminal used before this change, so the
// local terminal hot path is byte-for-byte unchanged (one virtual call at
// construction is the only added cost). createGitRunner(), exec() and
// fsBackend() are fully implemented locally now so the Phase 2-4 seams are real
// and exercised with zero regression.
class LocalExecutionContext : public ExecutionContext
{
    Q_OBJECT

public:
    explicit LocalExecutionContext(QObject *parent = nullptr);
    ~LocalExecutionContext() override;

    bool isRemote() const override { return false; }
    QString displayName() const override;
    State state() const override { return State::Connected; }

    IPtyProcess *createPty(QObject *parent) override;
    IGitProcessRunner *createGitRunner(QObject *parent) override;
    void exec(const QString &cwd,
              const QStringList &argv,
              const QByteArray &stdinPayload,
              int timeoutMs,
              ExecCallback cb) override;
    IFileSystemBackend *fsBackend() override;
    QString resolveCwd(const QString &requested) const override;

private:
    LocalFileSystemBackend *m_fsBackend = nullptr;
};

} // namespace remote

#endif // REMOTE_LOCALEXECUTIONCONTEXT_H
