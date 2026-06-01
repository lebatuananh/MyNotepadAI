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

// Thin production ISshTransport over libssh2 (non-blocking). All the
// state-machine logic lives in SshSessionWorker; here each method is 1-2
// libssh2 calls that translate LIBSSH2_ERROR_EAGAIN → Step::Again and a fatal
// errno → Step::Error. Runs only on the worker thread.

#include "Libssh2Transport.h"

#include <QtGlobal>

#include <libssh2.h>

#include <cstring>

#ifdef Q_OS_WIN
#  include <winsock2.h>
#  include <ws2tcpip.h>
#else
#  include <netdb.h>
#  include <netinet/in.h>
#  include <sys/socket.h>
#  include <sys/types.h>
#  include <unistd.h>
#endif

namespace remote {

namespace {

inline ISshTransport::Step stepFromRc(int rc)
{
    if (rc == 0) return ISshTransport::Step::Ok;
    if (rc == LIBSSH2_ERROR_EAGAIN) return ISshTransport::Step::Again;
    return ISshTransport::Step::Error;
}

#ifdef Q_OS_WIN
void closeSock(qintptr s) { ::closesocket(static_cast<SOCKET>(s)); }
inline SOCKET sockHandle(qintptr s) { return static_cast<SOCKET>(s); }
#else
void closeSock(qintptr s) { ::close(static_cast<int>(s)); }
inline int sockHandle(qintptr s) { return static_cast<int>(s); }
#endif

} // namespace

Libssh2Transport::Libssh2Transport()
{
    // libssh2_init is refcounted; safe to call per transport. CRYPTO backend is
    // the vendored mbedTLS (selected at build time).
    if (libssh2_init(0) == 0) {
        m_libssh2Inited = true;
    }
}

Libssh2Transport::~Libssh2Transport()
{
    disconnect();
    if (m_libssh2Inited) {
        libssh2_exit();
    }
}

Libssh2Transport::Step Libssh2Transport::connectSocket(const QString &host, int port)
{
    if (m_sock >= 0) {
        return Step::Ok; // already connected
    }

#ifdef Q_OS_WIN
    {
        // WSAStartup is idempotent across calls; matched by WSACleanup in
        // disconnect-time teardown is not strictly necessary for a long-lived
        // app, so we leave it started.
        static bool wsaStarted = false;
        if (!wsaStarted) {
            WSADATA wsa;
            if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
                return Step::Error;
            }
            wsaStarted = true;
        }
    }
#endif

    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    const QByteArray hostUtf8 = host.toUtf8();
    const QByteArray portStr = QByteArray::number(port);
    addrinfo *res = nullptr;
    if (getaddrinfo(hostUtf8.constData(), portStr.constData(), &hints, &res) != 0 || !res) {
        return Step::Error;
    }

    qintptr sock = -1;
    for (addrinfo *ai = res; ai; ai = ai->ai_next) {
        const qintptr s = static_cast<qintptr>(::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol));
        if (s < 0) {
            continue;
        }
        if (::connect(sockHandle(s), ai->ai_addr,
                      static_cast<int>(ai->ai_addrlen)) == 0) {
            sock = s;
            break;
        }
        closeSock(s);
    }
    freeaddrinfo(res);

    if (sock < 0) {
        return Step::Error;
    }
    m_sock = sock;

    m_session = libssh2_session_init();
    if (!m_session) {
        closeSock(m_sock);
        m_sock = -1;
        return Step::Error;
    }
    // Non-blocking from here on — the worker drives via QSocketNotifier.
    libssh2_session_set_blocking(m_session, 0);
    return Step::Ok;
}

Libssh2Transport::Step Libssh2Transport::handshake()
{
    if (!m_session || m_sock < 0) {
        return Step::Error;
    }
    const int rc = libssh2_session_handshake(m_session, sockHandle(m_sock));
    const Step s = stepFromRc(rc);
    if (s == Step::Ok && m_hostKey.isEmpty()) {
        size_t len = 0;
        int type = 0;
        const char *key = libssh2_session_hostkey(m_session, &len, &type);
        if (key && len > 0) {
            m_hostKey = QByteArray(key, static_cast<int>(len));
        }
    }
    return s;
}

QByteArray Libssh2Transport::hostKey() const
{
    return m_hostKey;
}

Libssh2Transport::Step Libssh2Transport::authPassword(const QString &username,
                                                      const QString &password)
{
    if (!m_session) return Step::Error;
    const QByteArray u = username.toUtf8();
    const QByteArray p = password.toUtf8();
    const int rc = libssh2_userauth_password_ex(
        m_session, u.constData(), static_cast<unsigned int>(u.size()),
        p.constData(), static_cast<unsigned int>(p.size()), nullptr);
    return stepFromRc(rc);
}

Libssh2Transport::Step Libssh2Transport::authPublicKey(const QString &username,
                                                       const QString &keyPath,
                                                       const QString &passphrase)
{
    if (!m_session) return Step::Error;
    const QByteArray u = username.toUtf8();
    const QByteArray priv = keyPath.toUtf8();
    const QByteArray pass = passphrase.toUtf8();
    // Public-key path left null → libssh2 derives "<priv>.pub".
    const int rc = libssh2_userauth_publickey_fromfile_ex(
        m_session, u.constData(), static_cast<unsigned int>(u.size()),
        nullptr, priv.constData(), pass.isEmpty() ? nullptr : pass.constData());
    return stepFromRc(rc);
}

Libssh2Transport::Step Libssh2Transport::authAgent(const QString &username)
{
    if (!m_session) return Step::Error;
    const QByteArray u = username.toUtf8();

    if (!m_agent) {
        m_agent = libssh2_agent_init(m_session);
        if (!m_agent) {
            return Step::Error;
        }
        if (libssh2_agent_connect(m_agent) != 0) {
            return Step::Error;
        }
        if (libssh2_agent_list_identities(m_agent) != 0) {
            return Step::Error;
        }
        m_agentPrevId = nullptr;
    }

    // Walk identities, trying each until one authenticates. Non-blocking auth
    // can return EAGAIN; surface that so the worker retries on the next edge.
    for (;;) {
        struct libssh2_agent_publickey *identity = nullptr;
        const int rc = libssh2_agent_get_identity(
            m_agent, &identity, static_cast<struct libssh2_agent_publickey *>(m_agentPrevId));
        if (rc == 1) {
            return Step::Error; // exhausted all identities without success
        }
        if (rc < 0) {
            return Step::Error;
        }
        const int authRc = libssh2_agent_userauth(m_agent, u.constData(), identity);
        if (authRc == 0) {
            return Step::Ok;
        }
        if (authRc == LIBSSH2_ERROR_EAGAIN) {
            return Step::Again; // retry this same identity next edge
        }
        m_agentPrevId = identity; // this key failed; advance to the next
    }
}

Libssh2Transport::OpenResult Libssh2Transport::openChannel()
{
    OpenResult out;
    if (!m_session) {
        out.step = Step::Error;
        return out;
    }
    LIBSSH2_CHANNEL *ch = libssh2_channel_open_session(m_session);
    if (ch) {
        const int id = m_nextChannelId++;
        m_channels.insert(id, ch);
        out.step = Step::Ok;
        out.channelId = id;
        return out;
    }
    const int err = libssh2_session_last_errno(m_session);
    out.step = (err == LIBSSH2_ERROR_EAGAIN) ? Step::Again : Step::Error;
    return out;
}

Libssh2Transport::Step Libssh2Transport::requestPty(int channelId, const QByteArray &term,
                                                    int cols, int rows)
{
    LIBSSH2_CHANNEL *ch = channel(channelId);
    if (!ch) return Step::Error;
    const QByteArray t = term.isEmpty() ? QByteArrayLiteral("xterm-256color") : term;
    const int rc = libssh2_channel_request_pty_ex(
        ch, t.constData(), static_cast<unsigned int>(t.size()),
        nullptr, 0, cols, rows, 0, 0);
    return stepFromRc(rc);
}

Libssh2Transport::Step Libssh2Transport::resizePty(int channelId, int cols, int rows)
{
    LIBSSH2_CHANNEL *ch = channel(channelId);
    if (!ch) return Step::Error;
    const int rc = libssh2_channel_request_pty_size_ex(ch, cols, rows, 0, 0);
    return stepFromRc(rc);
}

Libssh2Transport::Step Libssh2Transport::execOrShell(int channelId, const QString &command)
{
    LIBSSH2_CHANNEL *ch = channel(channelId);
    if (!ch) return Step::Error;
    int rc;
    if (command.isEmpty()) {
        rc = libssh2_channel_shell(ch);
    } else {
        const QByteArray c = command.toUtf8();
        rc = libssh2_channel_exec(ch, c.constData());
    }
    return stepFromRc(rc);
}

qint64 Libssh2Transport::chWrite(int channelId, const QByteArray &bytes)
{
    LIBSSH2_CHANNEL *ch = channel(channelId);
    if (!ch) return kWriteError;
    const ssize_t n = libssh2_channel_write(ch, bytes.constData(),
                                            static_cast<size_t>(bytes.size()));
    if (n == LIBSSH2_ERROR_EAGAIN) return kWriteAgain;
    if (n < 0) return kWriteError;
    return static_cast<qint64>(n);
}

Libssh2Transport::ReadResult Libssh2Transport::chRead(int channelId)
{
    ReadResult out;
    LIBSSH2_CHANNEL *ch = channel(channelId);
    if (!ch) {
        out.error = true;
        return out;
    }
    char buf[32 * 1024];
    const ssize_t n = libssh2_channel_read(ch, buf, sizeof(buf));
    if (n == LIBSSH2_ERROR_EAGAIN) {
        out.again = true;
        return out;
    }
    if (n < 0) {
        out.error = true;
        return out;
    }
    if (n > 0) {
        out.data = QByteArray(buf, static_cast<int>(n));
    }
    // libssh2_channel_eof is cheap; report EOF so the worker can finish + read
    // the exit status.
    if (libssh2_channel_eof(ch)) {
        out.eof = true;
    }
    return out;
}

int Libssh2Transport::chExitStatus(int channelId)
{
    LIBSSH2_CHANNEL *ch = channel(channelId);
    if (!ch) return -1;
    return libssh2_channel_get_exit_status(ch);
}

void Libssh2Transport::closeChannel(int channelId)
{
    LIBSSH2_CHANNEL *ch = channel(channelId);
    if (!ch) return;
    libssh2_channel_close(ch);
    libssh2_channel_free(ch);
    m_channels.remove(channelId);
}

void Libssh2Transport::disconnect()
{
    for (auto it = m_channels.begin(); it != m_channels.end(); ++it) {
        if (it.value()) {
            libssh2_channel_free(it.value());
        }
    }
    m_channels.clear();

    if (m_agent) {
        libssh2_agent_disconnect(m_agent);
        libssh2_agent_free(m_agent);
        m_agent = nullptr;
    }
    if (m_session) {
        libssh2_session_disconnect(m_session, "NotepadAI closing");
        libssh2_session_free(m_session);
        m_session = nullptr;
    }
    if (m_sock >= 0) {
        closeSock(m_sock);
        m_sock = -1;
    }
}

} // namespace remote
