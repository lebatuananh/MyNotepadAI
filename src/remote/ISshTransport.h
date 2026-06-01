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

#ifndef REMOTE_ISSHTRANSPORT_H
#define REMOTE_ISSHTRANSPORT_H

#include <QByteArray>
#include <QString>

namespace remote {

// The injectable seam that sits DIRECTLY on top of the libssh2 surface — one or
// two libssh2 calls per method, no algorithm. Libssh2Transport is the thin
// production impl; FakeSshTransport scripts handshake/auth/channel behavior so
// the SshSessionWorker state machine, channel multiplexing, write-pending
// toggling and connection-loss cleanup are all tested OFFLINE (no real sshd).
//
// All methods run on the worker thread only (the worker is the sole caller).
// libssh2 is operated non-blocking, so every step can return Again and is
// retried on the next socket-readable/writable edge.
class ISshTransport
{
public:
    // Per-step outcome for the connect/auth/channel-setup state machine.
    enum class Step
    {
        Ok,     // completed
        Again,  // EAGAIN — retry on next socket activity
        Error,  // fatal — connection cannot proceed
    };

    // openChannel outcome: Ok carries a transport channel id.
    struct OpenResult
    {
        Step step = Step::Again;
        int  channelId = -1;
    };

    // chRead outcome. `data` may be non-empty alongside eof. `error` is a fatal
    // socket condition (SOCKET_RECV/DISCONNECT) — distinct from `again`.
    struct ReadResult
    {
        QByteArray data;
        bool again = false;
        bool eof = false;
        bool error = false;
    };

    // chWrite sentinels (returned in place of a byte count).
    static constexpr qint64 kWriteAgain = -1; // EAGAIN: send buffer full
    static constexpr qint64 kWriteError = -2; // fatal socket error

    virtual ~ISshTransport() = default;

    // --- connect / auth ------------------------------------------------------
    virtual Step connectSocket(const QString &host, int port) = 0;
    virtual Step handshake() = 0;
    // Raw host-key blob captured during handshake (for fingerprinting + the
    // known_hosts compare). Empty until handshake completes.
    virtual QByteArray hostKey() const = 0;
    virtual Step authPassword(const QString &username, const QString &password) = 0;
    virtual Step authPublicKey(const QString &username,
                               const QString &keyPath,
                               const QString &passphrase) = 0;
    virtual Step authAgent(const QString &username) = 0;

    // --- channels ------------------------------------------------------------
    virtual OpenResult openChannel() = 0;
    virtual Step requestPty(int channelId, const QByteArray &term, int cols, int rows) = 0;
    virtual Step resizePty(int channelId, int cols, int rows) = 0;
    // command empty → interactive shell; otherwise exec the command.
    virtual Step execOrShell(int channelId, const QString &command) = 0;
    virtual qint64 chWrite(int channelId, const QByteArray &bytes) = 0;
    virtual ReadResult chRead(int channelId) = 0;
    virtual int chExitStatus(int channelId) = 0;
    virtual void closeChannel(int channelId) = 0;

    // Underlying socket fd, for the QSocketNotifier pump. -1 until connected.
    virtual qintptr socketFd() const = 0;

    // Tear down session + socket. After this, no other method is called.
    virtual void disconnect() = 0;
};

} // namespace remote

#endif // REMOTE_ISSHTRANSPORT_H
