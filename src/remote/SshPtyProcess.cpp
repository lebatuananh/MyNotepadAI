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

#include "SshPtyProcess.h"

#include "SshChannel.h"
#include "SshConnection.h"

namespace remote {

SshPtyProcess::SshPtyProcess(SshConnection *connection, QObject *parent)
    : IPtyProcess()
    , m_connection(connection)
{
    if (parent) {
        setParent(parent);
    }
}

SshPtyProcess::~SshPtyProcess()
{
    if (m_channel) {
        m_channel->write(QByteArray()); // no-op if dead; keeps symmetry
    }
}

QString SshPtyProcess::shellQuote(const QString &s)
{
    // POSIX single-quote quoting: wrap in '...' and escape embedded single
    // quotes as '\''. Safe for arbitrary paths / values.
    QString out;
    out.reserve(s.size() + 2);
    out.append(QLatin1Char('\''));
    for (const QChar c : s) {
        if (c == QLatin1Char('\'')) {
            out.append(QLatin1String("'\\''"));
        } else {
            out.append(c);
        }
    }
    out.append(QLatin1Char('\''));
    return out;
}

QString SshPtyProcess::buildRemoteCommand(const QString &shellPath, const QString &workingDir,
                                          const QStringList &environment)
{
    // `cd <cwd> && exec <env...> <shell>`. The SSH PTY request has no working
    // -dir argument, so the cd is part of the command. env entries (KEY=VALUE)
    // are exported before exec where the server permits.
    QString cmd;
    if (!workingDir.isEmpty()) {
        cmd += QStringLiteral("cd ") + shellQuote(workingDir) + QStringLiteral(" && ");
    }
    cmd += QStringLiteral("exec ");
    for (const QString &e : environment) {
        const int eq = e.indexOf(QLatin1Char('='));
        if (eq <= 0) {
            continue;
        }
        const QString key = e.left(eq);
        const QString value = e.mid(eq + 1);
        cmd += key + QLatin1Char('=') + shellQuote(value) + QLatin1Char(' ');
    }
    const QString shell = shellPath.isEmpty() ? QStringLiteral("$SHELL") : shellPath;
    cmd += shellQuote(shell);
    return cmd;
}

bool SshPtyProcess::startProcess(const QString &shellPath, const QString &workingDir,
                                 QStringList environment, qint16 cols, qint16 rows)
{
    if (!m_connection) {
        m_lastError = tr("No SSH connection");
        return false;
    }
    const QString command = buildRemoteCommand(shellPath, workingDir, environment);

    m_channel = m_connection->openChannel(/*wantPty=*/true,
                                           QByteArrayLiteral("xterm-256color"),
                                           cols, rows, command);
    if (!m_channel) {
        m_lastError = tr("Could not open SSH channel");
        return false;
    }
    m_shellPath = shellPath;
    m_size = qMakePair(cols, rows);
    m_pid = 1; // a non-zero sentinel so isProcessRunning() reads true

    // Channel close → IPtyProcess::finished. Exit status -1 on abnormal/lost.
    connect(m_channel, &SshChannel::closed, this, [this](int exitStatus) {
        if (m_finishedEmitted) {
            return;
        }
        m_finishedEmitted = true;
        m_pid = 0;
        emit finished(exitStatus);
    });
    return true;
}

bool SshPtyProcess::resize(qint16 cols, qint16 rows)
{
    if (!m_channel) {
        return false;
    }
    m_size = qMakePair(cols, rows);
    m_channel->resize(cols, rows);
    return true;
}

bool SshPtyProcess::kill()
{
    if (m_connection && m_channel) {
        m_connection->closeChannel(m_channel->logicalId());
    }
    return true;
}

QString SshPtyProcess::dumpDebugInfo()
{
    return QStringLiteral("SshPtyProcess channel=%1")
        .arg(m_channel ? m_channel->logicalId() : -1);
}

QIODevice *SshPtyProcess::notifier()
{
    return m_channel ? m_channel->notifier() : nullptr;
}

QByteArray SshPtyProcess::readAll()
{
    return m_channel ? m_channel->readAll() : QByteArray();
}

qint64 SshPtyProcess::write(const QByteArray &byteArray)
{
    if (!m_channel) {
        return -1;
    }
    return m_channel->write(byteArray);
}

bool SshPtyProcess::isAvailable()
{
    return m_connection != nullptr;
}

void SshPtyProcess::moveToThread(QThread *targetThread)
{
    QObject::moveToThread(targetThread);
}

} // namespace remote
