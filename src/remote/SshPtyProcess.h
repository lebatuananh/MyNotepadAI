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

#ifndef REMOTE_SSHPTYPROCESS_H
#define REMOTE_SSHPTYPROCESS_H

#include <QPointer>

#include "iptyprocess.h"

namespace remote {

class SshConnection;
class SshChannel;

// A remote PTY presented through the existing IPtyProcess interface, so
// TerminalWidget treats it exactly like a local PTY (the read/write hot path is
// unchanged). It does NOT touch libptyqt — it opens an SSH exec channel with a
// PTY on an SshConnection and bridges the channel's readyRead/data/exit to the
// IPtyProcess contract.
//
//   startProcess(shell, cwd, env, cols, rows):
//     open channel with request_pty("xterm-256color", cols, rows) and run
//     `cd <cwd> && exec <shell>` (cwd/shell shell-quoted; SSH PTY has no
//     working-dir arg so cd is in the command). env is applied where the server
//     permits via the command prefix.
//   write   → SshConnection::write (queued)
//   resize  → request_pty_size
//   kill    → channel close
//   notifier()/readAll() → the channel's QIODevice proxy (readyRead preserved)
//   finished(exitCode) from the channel close (or -1 on connection loss)
class SshPtyProcess : public IPtyProcess
{
    Q_OBJECT

public:
    explicit SshPtyProcess(SshConnection *connection, QObject *parent = nullptr);
    ~SshPtyProcess() override;

    bool startProcess(const QString &shellPath, const QString &workingDir,
                      QStringList environment, qint16 cols, qint16 rows) override;
    bool resize(qint16 cols, qint16 rows) override;
    bool kill() override;
    PtyType type() const override { return IPtyProcess::AutoPty; }
    QString dumpDebugInfo() override;
    QIODevice *notifier() override;
    QByteArray readAll() override;
    qint64 write(const QByteArray &byteArray) override;
    bool isAvailable() override;
    void moveToThread(QThread *targetThread) override;

    // Build `cd <cwd> && exec <env> <shell>` with POSIX shell-quoting. Static +
    // pure so it can be unit-tested without a connection.
    static QString buildRemoteCommand(const QString &shellPath, const QString &workingDir,
                                      const QStringList &environment);

private:
    static QString shellQuote(const QString &s);

    QPointer<SshConnection> m_connection;
    QPointer<SshChannel> m_channel;
    bool m_finishedEmitted = false;
};

} // namespace remote

#endif // REMOTE_SSHPTYPROCESS_H
