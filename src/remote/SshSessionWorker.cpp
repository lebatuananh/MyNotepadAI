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

#include "SshSessionWorker.h"

#include "SshHostKeyStore.h"

#include <QMetaType>
#include <QSocketNotifier>

#include <utility>

namespace remote {

SshSessionWorker::SshSessionWorker(std::unique_ptr<ISshTransport> transport,
                                   int channelCap, QObject *parent)
    : QObject(parent)
    , m_transport(std::move(transport))
    , m_channelCap(channelCap > 0 ? channelCap : 10)
{
    qRegisterMetaType<remote::SshSessionWorker::State>("remote::SshSessionWorker::State");
    qRegisterMetaType<remote::SshSessionWorker::ConnectParams>(
        "remote::SshSessionWorker::ConnectParams");
}

SshSessionWorker::~SshSessionWorker()
{
    teardownNotifiers();
    if (m_transport) {
        m_transport->disconnect();
    }
}

void SshSessionWorker::setState(State s)
{
    if (m_state == s) {
        return;
    }
    m_state = s;
    emit stateChanged(s);
}

int SshSessionWorker::liveChannelCount() const
{
    int n = 0;
    for (auto it = m_channels.constBegin(); it != m_channels.constEnd(); ++it) {
        const ChPhase p = it.value().phase;
        if (p != ChPhase::Queued && p != ChPhase::Closed) {
            ++n;
        }
    }
    return n;
}

SshSessionWorker::Channel *SshSessionWorker::channelByTransportId(int transportId)
{
    for (auto it = m_channels.begin(); it != m_channels.end(); ++it) {
        if (it.value().transportId == transportId) {
            return &it.value();
        }
    }
    return nullptr;
}

// --- connection entry points -------------------------------------------------

void SshSessionWorker::startConnect(const ConnectParams &params)
{
    m_params = params;
    m_lost = false;
    m_hostKeyEmitted = false;
    m_hostKeyAccepted = false;
    m_hostKeyRejected = false;
    setState(State::ConnectingSocket);
    pump();
}

void SshSessionWorker::acceptHostKey()
{
    m_hostKeyAccepted = true;
    pump();
}

void SshSessionWorker::rejectHostKey()
{
    m_hostKeyRejected = true;
    // The user refused the fingerprint — abort without authenticating.
    enterConnectionLost(tr("Host key rejected"));
}

void SshSessionWorker::requestDisconnect()
{
    if (m_state == State::Disconnected || m_state == State::Failed) {
        teardownNotifiers();
        if (m_transport) m_transport->disconnect();
        return;
    }
    teardownNotifiers();
    if (m_transport) {
        m_transport->disconnect();
    }
    setState(State::Disconnected);
}

// --- channel request slots ---------------------------------------------------

void SshSessionWorker::requestOpenChannel(int logicalId, bool wantPty,
                                          const QByteArray &term, int cols, int rows,
                                          const QString &command)
{
    if (m_lost || m_state == State::Disconnected || m_state == State::Failed) {
        emit channelOpenFailed(logicalId, tr("Not connected"));
        return;
    }
    Channel ch;
    ch.logicalId = logicalId;
    ch.phase = ChPhase::Queued;   // promoted to Opening by tryStartQueued under the cap
    ch.wantPty = wantPty;
    ch.term = term.isEmpty() ? QByteArrayLiteral("xterm-256color") : term;
    ch.cols = cols > 0 ? cols : 80;
    ch.rows = rows > 0 ? rows : 24;
    ch.command = command;
    m_channels.insert(logicalId, ch);
    m_openQueue.append(logicalId);   // FIFO (D5): never dropped
    pump();
}

void SshSessionWorker::requestResize(int logicalId, int cols, int rows)
{
    auto it = m_channels.find(logicalId);
    if (it == m_channels.end()) {
        return;
    }
    it->cols = cols > 0 ? cols : it->cols;
    it->rows = rows > 0 ? rows : it->rows;
    if (it->phase == ChPhase::Open && !m_lost) {
        m_transport->resizePty(it->transportId, it->cols, it->rows);
    }
}

void SshSessionWorker::requestWrite(int logicalId, const QByteArray &bytes)
{
    auto it = m_channels.find(logicalId);
    if (it == m_channels.end() || it->phase == ChPhase::Closed || m_lost) {
        return; // dead-guard: nothing posted to a gone channel/session
    }
    it->pending.append(bytes);   // move-append, no per-byte allocation
    flushPendingWrites();
}

void SshSessionWorker::requestCloseChannel(int logicalId)
{
    auto it = m_channels.find(logicalId);
    if (it == m_channels.end()) {
        return;
    }
    // finishChannel frees the underlying transport channel (when live), so do
    // NOT close it here too — that would double-free.
    finishChannel(logicalId, -1);
}

// --- the pump: a single bounded sweep (D3) -----------------------------------

void SshSessionWorker::pump()
{
    if (m_lost) {
        return;
    }

    if (m_state == State::ConnectingSocket || m_state == State::Handshaking
        || m_state == State::AwaitingHostKey || m_state == State::Authenticating) {
        advanceConnect();
    }

    if (m_state != State::Ready) {
        return;
    }

    // Ready: progress any channels still being set up, drain reads round-robin,
    // then flush writes (which toggles the write notifier per D4).
    tryStartQueued();
    advanceChannelSetup();
    readSweep();
    flushPendingWrites();
}

void SshSessionWorker::advanceConnect()
{
    // Socket connect.
    if (m_state == State::ConnectingSocket) {
        const ISshTransport::Step s = m_transport->connectSocket(m_params.host, m_params.port);
        if (s == ISshTransport::Step::Again) return;
        if (s == ISshTransport::Step::Error) {
            enterConnectionLost(tr("Could not reach %1:%2").arg(m_params.host).arg(m_params.port));
            return;
        }
        setupNotifiers();
        setState(State::Handshaking);
    }

    // SSH handshake (key exchange).
    if (m_state == State::Handshaking) {
        const ISshTransport::Step s = m_transport->handshake();
        if (s == ISshTransport::Step::Again) return;
        if (s == ISshTransport::Step::Error) {
            enterConnectionLost(tr("SSH handshake failed"));
            return;
        }
        setState(State::AwaitingHostKey);
    }

    // Host-key verification gate. Emit the fingerprint once; auth is blocked
    // until acceptHostKey() (rejectHostKey() aborts via enterConnectionLost).
    if (m_state == State::AwaitingHostKey) {
        if (m_hostKeyRejected) {
            return; // already aborting
        }
        if (!m_hostKeyEmitted) {
            m_hostKeyEmitted = true;
            const QByteArray key = m_transport->hostKey();
            emit hostKeyReceived(SshHostKeyStore::sha256Fingerprint(key), key);
        }
        if (!m_hostKeyAccepted) {
            return; // wait for the UI / test to accept
        }
        setState(State::Authenticating);
    }

    // Authentication.
    if (m_state == State::Authenticating) {
        ISshTransport::Step s = ISshTransport::Step::Error;
        switch (m_params.authMethod) {
        case SshProfile::AuthMethod::Password: {
            const QString pw = m_params.passwordProvider ? m_params.passwordProvider() : QString();
            s = m_transport->authPassword(m_params.username, pw);
            break;
        }
        case SshProfile::AuthMethod::KeyFile: {
            const QString pass = m_params.passphraseProvider ? m_params.passphraseProvider() : QString();
            s = m_transport->authPublicKey(m_params.username, m_params.keyPath, pass);
            break;
        }
        case SshProfile::AuthMethod::Agent:
            s = m_transport->authAgent(m_params.username);
            break;
        }
        if (s == ISshTransport::Step::Again) return;
        if (s == ISshTransport::Step::Error) {
            const QString reason = tr("Authentication failed");
            emit authFailed(reason);
            enterConnectionLost(reason);
            return;
        }
        setState(State::Ready);
        emit connected();
    }
}

// --- channel-open FIFO queue (D5) --------------------------------------------

void SshSessionWorker::tryStartQueued()
{
    // Promote queued channels to Opening while under the cap. FIFO: always take
    // the head; never drop a request.
    while (!m_openQueue.isEmpty() && liveChannelCount() < m_channelCap) {
        const int logicalId = m_openQueue.takeFirst();
        auto it = m_channels.find(logicalId);
        if (it == m_channels.end()) {
            continue; // closed before it got a slot
        }
        it->phase = ChPhase::Opening; // now counts against the cap
    }
}

void SshSessionWorker::advanceChannelSetup()
{
    // Drive every not-yet-Open channel through open → (pty) → exec/shell.
    // Iterate over a snapshot of ids so finishChannel-driven removals are safe.
    const QList<int> ids = m_channels.keys();
    for (int logicalId : ids) {
        auto it = m_channels.find(logicalId);
        if (it == m_channels.end()) continue;
        Channel &ch = it.value();

        if (ch.phase == ChPhase::Opening) {
            const ISshTransport::OpenResult r = m_transport->openChannel();
            if (r.step == ISshTransport::Step::Again) {
                continue;
            }
            if (r.step == ISshTransport::Step::Error) {
                emit channelOpenFailed(logicalId, tr("Could not open channel"));
                finishChannel(logicalId, -1);
                continue;
            }
            ch.transportId = r.channelId;
            ch.phase = ch.wantPty ? ChPhase::NeedPty : ChPhase::NeedExec;
        }

        if (ch.phase == ChPhase::NeedPty) {
            const ISshTransport::Step s =
                m_transport->requestPty(ch.transportId, ch.term, ch.cols, ch.rows);
            if (s == ISshTransport::Step::Again) continue;
            if (s == ISshTransport::Step::Error) {
                emit channelOpenFailed(logicalId, tr("Could not allocate a PTY"));
                finishChannel(logicalId, -1);
                continue;
            }
            ch.phase = ChPhase::NeedExec;
        }

        if (ch.phase == ChPhase::NeedExec) {
            const ISshTransport::Step s = m_transport->execOrShell(ch.transportId, ch.command);
            if (s == ISshTransport::Step::Again) continue;
            if (s == ISshTransport::Step::Error) {
                emit channelOpenFailed(logicalId, tr("Could not start the remote shell"));
                finishChannel(logicalId, -1);
                continue;
            }
            ch.phase = ChPhase::Open;
            if (!m_rotation.contains(logicalId)) {
                m_rotation.append(logicalId);
            }
            emit channelOpened(logicalId);
        }
    }
}

// --- round-robin read sweep (D3) ---------------------------------------------

void SshSessionWorker::readSweep()
{
    // INVARIANT (anti-starvation): visit every Open channel exactly once per
    // sweep and drain it to EAGAIN. The outer loop advances regardless of how
    // much one channel produces, so a chatty channel cannot starve the others.
    // Time O(C + B); no per-byte allocation (chRead returns a QByteArray that we
    // MOVE into the dataReady payload); no scan of closed channels.
    const QList<int> order = m_rotation; // stable snapshot; safe across removals
    for (int logicalId : order) {
        auto it = m_channels.find(logicalId);
        if (it == m_channels.end() || it->phase != ChPhase::Open) {
            continue;
        }
        const int transportId = it->transportId;
        for (;;) {
            ISshTransport::ReadResult r = m_transport->chRead(transportId);
            if (r.error) {
                enterConnectionLost(tr("SSH connection lost"));
                return;
            }
            if (!r.data.isEmpty()) {
                emit dataReady(logicalId, std::move(r.data)); // queued to UI thread
            }
            if (r.eof) {
                finishChannel(logicalId, m_transport->chExitStatus(transportId));
                break;
            }
            if (r.again) {
                break; // channel drained; move to the next
            }
        }
    }
}

// --- write path + write-notifier invariant (D4) ------------------------------

void SshSessionWorker::flushPendingWrites()
{
    if (m_lost) {
        return;
    }
    bool anyPending = false;
    for (auto it = m_channels.begin(); it != m_channels.end(); ++it) {
        Channel &ch = it.value();
        if (ch.phase != ChPhase::Open || ch.pending.isEmpty()) {
            continue;
        }
        while (!ch.pending.isEmpty()) {
            const qint64 n = m_transport->chWrite(ch.transportId, ch.pending);
            if (n == ISshTransport::kWriteAgain) {
                anyPending = true;
                break; // send buffer full; wait for socket-writable
            }
            if (n == ISshTransport::kWriteError) {
                enterConnectionLost(tr("SSH connection lost"));
                return;
            }
            if (n <= 0) {
                break; // nothing consumed; avoid a spin
            }
            ch.pending.remove(0, static_cast<int>(n));
        }
    }
    // INVARIANT: the write notifier is enabled ONLY while bytes are pending and
    // is disabled the instant they drain — otherwise it fires continuously
    // (socket is almost always writable) and burns CPU.
    setWriteNotifierEnabled(anyPending);
}

// --- connection-loss cleanup (D8) --------------------------------------------

void SshSessionWorker::enterConnectionLost(const QString &reason)
{
    if (m_lost) {
        return;
    }
    m_lost = true; // mark dead FIRST — no further libssh2 calls on a dead session

    teardownNotifiers();

    // Close each channel reporting an abnormal exit (-1, never a fabricated
    // success). Already-read bytes were emitted synchronously during the sweep,
    // so no tail output is lost. Iterate a snapshot of ids.
    const QList<int> ids = m_channels.keys();
    for (int logicalId : ids) {
        auto it = m_channels.find(logicalId);
        if (it == m_channels.end()) continue;
        it->phase = ChPhase::Closed;
        emit channelClosed(logicalId, -1);
    }
    m_channels.clear();
    m_rotation.clear();
    m_openQueue.clear();

    if (m_transport) {
        m_transport->disconnect();
    }
    setState(State::Disconnected);
    emit disconnected(reason);
}

void SshSessionWorker::finishChannel(int logicalId, int exitStatus)
{
    auto it = m_channels.find(logicalId);
    if (it == m_channels.end()) {
        return;
    }
    // Free the underlying transport channel if it was opened and the session is
    // still alive. (On connection loss the session is dead — enterConnectionLost
    // handles teardown without calling libssh2.)
    if (it->transportId >= 0 && !m_lost) {
        m_transport->closeChannel(it->transportId);
    }
    m_channels.erase(it);
    m_rotation.removeAll(logicalId);
    m_openQueue.removeAll(logicalId);
    emit channelClosed(logicalId, exitStatus);

    // A freed slot may let a queued open proceed (D5). Only meaningful while
    // still connected.
    if (!m_lost && m_state == State::Ready) {
        tryStartQueued();
        advanceChannelSetup();
    }
}

// --- notifiers (production only; tests drive pumpForTest) --------------------

void SshSessionWorker::setupNotifiers()
{
    const qintptr fd = m_transport->socketFd();
    if (fd < 0) {
        return; // fd not valid yet (or a fake transport in tests)
    }
    if (!m_readNotifier) {
        m_readNotifier = new QSocketNotifier(fd, QSocketNotifier::Read, this);
        connect(m_readNotifier, &QSocketNotifier::activated, this,
                &SshSessionWorker::onSocketActivity);
    }
    if (!m_writeNotifier) {
        m_writeNotifier = new QSocketNotifier(fd, QSocketNotifier::Write, this);
        m_writeNotifier->setEnabled(false); // D4: off until bytes pending
        connect(m_writeNotifier, &QSocketNotifier::activated, this,
                &SshSessionWorker::onSocketActivity);
    }
}

void SshSessionWorker::teardownNotifiers()
{
    if (m_readNotifier) {
        m_readNotifier->setEnabled(false);
        m_readNotifier->deleteLater();
        m_readNotifier = nullptr;
    }
    if (m_writeNotifier) {
        m_writeNotifier->setEnabled(false);
        m_writeNotifier->deleteLater();
        m_writeNotifier = nullptr;
    }
    m_writeNotifierWanted = false;
}

void SshSessionWorker::setWriteNotifierEnabled(bool on)
{
    m_writeNotifierWanted = on; // observable in tests
    if (m_writeNotifier) {
        m_writeNotifier->setEnabled(on);
    }
}

void SshSessionWorker::onSocketActivity()
{
    // A single readable/writable edge → one bounded pump sweep. Not recursive.
    pump();
}

} // namespace remote
