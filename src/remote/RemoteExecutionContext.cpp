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

#include "RemoteExecutionContext.h"

#include "SshConnection.h"
#include "SshPtyProcess.h"

#include <QDir>

namespace remote {

namespace {

// Map the worker/connection state machine onto the ExecutionContext::State enum.
ExecutionContext::State mapState(SshConnection::State s)
{
    switch (s) {
    case SshSessionWorker::State::Idle:
    case SshSessionWorker::State::Disconnected:
        return ExecutionContext::State::Disconnected;
    case SshSessionWorker::State::ConnectingSocket:
    case SshSessionWorker::State::Handshaking:
    case SshSessionWorker::State::AwaitingHostKey:
    case SshSessionWorker::State::Authenticating:
        return ExecutionContext::State::Connecting;
    case SshSessionWorker::State::Ready:
        return ExecutionContext::State::Connected;
    case SshSessionWorker::State::Failed:
        return ExecutionContext::State::Failed;
    }
    return ExecutionContext::State::Disconnected;
}

} // namespace

RemoteExecutionContext::RemoteExecutionContext(const SshProfile &profile,
                                               SshConnection *connection, QObject *parent)
    : ExecutionContext(parent)
    , m_profile(profile)
    , m_connection(connection)
{
    if (m_connection) {
        m_state = mapState(m_connection->state());

        connect(m_connection, &SshConnection::stateChanged, this,
                [this](SshConnection::State s) {
                    const State mapped = mapState(s);
                    if (mapped != m_state) {
                        m_state = mapped;
                        emit stateChanged(m_state);
                    }
                });
        connect(m_connection, &SshConnection::connectionLost, this,
                [this](const QString &reason) {
                    if (m_state != State::Disconnected) {
                        m_state = State::Disconnected;
                        emit stateChanged(m_state);
                    }
                    emit connectionLost(reason);
                });
        connect(m_connection, &SshConnection::connected, this, [this]() {
            // reconnected() vs first connect: emit reconnected only if we had
            // previously been Connected then dropped. Phase 1 keeps this simple
            // — the auto-reconnect flow is Phase 2; we just surface the signal.
            emit reconnected();
        });
    }
}

RemoteExecutionContext::~RemoteExecutionContext() = default;

QString RemoteExecutionContext::displayName() const
{
    QString who = m_profile.username.isEmpty()
                      ? m_profile.host
                      : (m_profile.username + QLatin1Char('@') + m_profile.host);
    return who;
}

IPtyProcess *RemoteExecutionContext::createPty(QObject *parent)
{
    if (!m_connection) {
        return nullptr;
    }
    return new SshPtyProcess(m_connection, parent);
}

IGitProcessRunner *RemoteExecutionContext::createGitRunner(QObject *parent)
{
    Q_UNUSED(parent);
    // TODO P3: git over SSH. Declared seam; not implemented in Phase 1.
    return nullptr;
}

void RemoteExecutionContext::exec(const QString &cwd, const QStringList &argv,
                                  const QByteArray &stdinPayload, int timeoutMs,
                                  ExecCallback cb)
{
    Q_UNUSED(cwd);
    Q_UNUSED(argv);
    Q_UNUSED(stdinPayload);
    Q_UNUSED(timeoutMs);
    // TODO P4: one-shot remote exec over an SSH channel. Declared seam; not
    // implemented in Phase 1. Fail closed rather than pretending success.
    if (cb) {
        cb(-1, {}, QByteArrayLiteral("remote exec not implemented (Phase 4)"));
    }
}

IFileSystemBackend *RemoteExecutionContext::fsBackend()
{
    // TODO P2: SFTP-backed remote filesystem. Declared seam; not implemented in
    // Phase 1 (the remote file tree is Phase 2).
    return nullptr;
}

QString RemoteExecutionContext::resolveCwd(const QString &requested) const
{
    // Remote path: POSIX-normalize WITHOUT any local QFileInfo check (the path
    // lives on another machine). See design D11.
    QString path = requested;
    if (path.isEmpty()) {
        // Default to the profile's last remote path, else remote home ("~").
        path = m_profile.lastRemotePath.isEmpty() ? QStringLiteral("~")
                                                   : m_profile.lastRemotePath;
    }
    // Normalize separators to POSIX and collapse redundant slashes, but keep a
    // leading "~" or "/" intact. QDir::cleanPath is purely lexical (no disk
    // access), which is exactly what we want for a remote path.
    path.replace(QLatin1Char('\\'), QLatin1Char('/'));
    path = QDir::cleanPath(path);
    return path;
}

} // namespace remote
