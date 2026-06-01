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

#include "SshChannel.h"

#include "SshConnection.h"

#include <utility>

namespace remote {

SshChannel::SshChannel(SshConnection *connection, int logicalId, QObject *parent)
    : QObject(parent)
    , m_connection(connection)
    , m_logicalId(logicalId)
    , m_notifier(new SshChannelNotifier(this))
{
}

SshChannel::~SshChannel() = default;

QByteArray SshChannel::readAll()
{
    // Zero-copy hand-off: move the carry out and reset it.
    QByteArray out = std::move(m_carry);
    m_carry.clear();
    return out;
}

qint64 SshChannel::write(const QByteArray &bytes)
{
    // Dead-guard (D8): once closed/lost, never post to the worker — the worker
    // thread may be stopped and the session freed.
    if (!m_open || !m_connection) {
        return -1;
    }
    m_connection->writeToChannel(m_logicalId, bytes);
    return bytes.size();
}

void SshChannel::resize(int cols, int rows)
{
    if (!m_open || !m_connection) {
        return;
    }
    m_connection->resizeChannel(m_logicalId, cols, rows);
}

void SshChannel::appendIncoming(const QByteArray &bytes)
{
    if (bytes.isEmpty()) {
        return;
    }
    m_carry.append(bytes);
    m_notifier->fireReadyRead();
}

void SshChannel::markClosed(int exitStatus)
{
    if (!m_open) {
        return;
    }
    m_open = false;       // dead-guard latches: subsequent write() returns -1
    m_exitStatus = exitStatus;
    emit closed(exitStatus);
}

} // namespace remote
