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

#ifndef REMOTE_SSHCHANNEL_H
#define REMOTE_SSHCHANNEL_H

#include <QByteArray>
#include <QIODevice>
#include <QObject>

namespace remote {

class SshConnection;

// Minimal QIODevice that simply emits readyRead() when the proxy receives
// bytes, so consumers wired to the libptyqt notifier()-readyRead contract work
// unchanged. It is NOT a real byte stream — reads come from SshChannel's carry
// buffer via SshChannel::readAll(), not through QIODevice::read().
class SshChannelNotifier : public QIODevice
{
    Q_OBJECT
public:
    explicit SshChannelNotifier(QObject *parent = nullptr) : QIODevice(parent)
    {
        open(QIODevice::ReadOnly);
    }
    bool isSequential() const override { return true; }
    void fireReadyRead() { emit readyRead(); }

protected:
    qint64 readData(char *, qint64) override { return 0; }
    qint64 writeData(const char *, qint64) override { return -1; }
};

// UI-thread proxy for one logical SSH channel. Owns the read-carry buffer (bytes
// delivered via SshConnection's queued dataReady), a QIODevice notifier that
// emits readyRead so the existing terminal contract is unchanged, and the
// pending-write path delegated to the worker. The dead-guard is the key safety
// property (D8): after the channel is closed, write() is a NO-OP returning an
// error and NEVER posts to the (now stopped) worker thread.
class SshChannel : public QObject
{
    Q_OBJECT

public:
    SshChannel(SshConnection *connection, int logicalId, QObject *parent = nullptr);
    ~SshChannel() override;

    int logicalId() const { return m_logicalId; }
    bool isOpen() const { return m_open; }

    // Drain everything received so far (zero-copy move-out of the carry buffer).
    QByteArray readAll();

    // The readyRead notifier (lifetime owned by this channel).
    SshChannelNotifier *notifier() { return m_notifier; }

    // Queue bytes to the worker. Returns the byte count on success, or -1 if the
    // channel is closed/lost (dead-guard — nothing is posted to the worker).
    qint64 write(const QByteArray &bytes);

    void resize(int cols, int rows);

    // Exit status reported by channelClosed (valid once !isOpen()). -1 = abnormal.
    int exitStatus() const { return m_exitStatus; }

    // --- called by SshConnection on the UI thread (relayed from the worker) --
    void appendIncoming(const QByteArray &bytes); // append to carry + fire readyRead
    void markClosed(int exitStatus);              // dead-guard latches here

signals:
    void closed(int exitStatus);

private:
    SshConnection *m_connection;
    int m_logicalId;
    bool m_open = true;
    int m_exitStatus = 0;
    QByteArray m_carry;            // already-read bytes awaiting readAll()
    SshChannelNotifier *m_notifier;
};

} // namespace remote

#endif // REMOTE_SSHCHANNEL_H
