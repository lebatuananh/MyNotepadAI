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

#include "SshConnection.h"

#include "Libssh2Transport.h"
#include "SshChannel.h"

#include "ai/CredentialStore.h"

#include <QThread>

namespace remote {

SshConnection::SshConnection(const SshProfile &profile,
                             ai::CredentialStore *credentialStore, QObject *parent)
    : QObject(parent)
    , m_profile(profile)
    , m_credentialStore(credentialStore)
{
    init(std::make_unique<Libssh2Transport>());
}

SshConnection::SshConnection(const SshProfile &profile,
                             std::unique_ptr<ISshTransport> transport,
                             ai::CredentialStore *credentialStore, QObject *parent)
    : QObject(parent)
    , m_profile(profile)
    , m_credentialStore(credentialStore)
{
    init(std::move(transport));
}

void SshConnection::init(std::unique_ptr<ISshTransport> transport)
{
    m_thread = new QThread(this);
    m_thread->setObjectName(QStringLiteral("SshWorker:%1").arg(m_profile.id));

    // The worker owns the transport and lives on the worker thread. It has no
    // parent so moveToThread is clean; it is deleted when the thread finishes.
    m_worker = new SshSessionWorker(std::move(transport));
    m_worker->moveToThread(m_thread);
    connect(m_thread, &QThread::finished, m_worker, &QObject::deleteLater);

    // --- worker → facade relays (queued: cross-thread) -----------------------
    connect(m_worker, &SshSessionWorker::stateChanged, this,
            [this](SshSessionWorker::State s) {
                m_state = s;
                emit stateChanged(s);
            });
    connect(m_worker, &SshSessionWorker::hostKeyReceived, this,
            &SshConnection::handleHostKey);
    connect(m_worker, &SshSessionWorker::authFailed, this, &SshConnection::authFailed);
    connect(m_worker, &SshSessionWorker::connected, this, &SshConnection::connected);
    connect(m_worker, &SshSessionWorker::disconnected, this,
            [this](const QString &reason) { emit connectionLost(reason); });

    connect(m_worker, &SshSessionWorker::channelOpened, this, [this](int logicalId) {
        if (m_channels.contains(logicalId)) {
            emit channelReady(logicalId);
        }
    });
    connect(m_worker, &SshSessionWorker::channelOpenFailed, this,
            [this](int logicalId, const QString &reason) {
                if (auto *ch = m_channels.value(logicalId, nullptr)) {
                    ch->markClosed(-1);
                }
                emit channelOpenFailed(logicalId, reason);
            });
    connect(m_worker, &SshSessionWorker::dataReady, this,
            [this](int logicalId, const QByteArray &bytes) {
                if (auto *ch = m_channels.value(logicalId, nullptr)) {
                    ch->appendIncoming(bytes);
                }
            });
    connect(m_worker, &SshSessionWorker::channelClosed, this,
            [this](int logicalId, int exitStatus) {
                if (auto *ch = m_channels.value(logicalId, nullptr)) {
                    ch->markClosed(exitStatus);
                }
            });

    m_thread->start();
}

SshConnection::~SshConnection()
{
    // Ask the worker to tear down libssh2 on its own thread, then stop the loop.
    if (m_worker) {
        QMetaObject::invokeMethod(m_worker, "requestDisconnect", Qt::QueuedConnection);
    }
    if (m_thread) {
        m_thread->quit();
        m_thread->wait();
    }
    // m_worker was deleteLater'd via QThread::finished; channels are children
    // of this and clean up normally.
}

SshSessionWorker::ConnectParams SshConnection::buildConnectParams() const
{
    SshSessionWorker::ConnectParams p;
    p.host = m_profile.host;
    p.port = m_profile.port;
    p.username = m_profile.username;
    p.authMethod = m_profile.authMethod;
    p.keyPath = m_profile.keyPath;

    // Secret providers run ON THE WORKER THREAD during auth so the UI thread is
    // never blocked on a keychain syscall. Capture by value what they need.
    ai::CredentialStore *store = m_credentialStore;
    const QString id = m_profile.id;
    if (store) {
        p.passwordProvider = [store, id]() {
            return store->retrieveSecret(sshSecretKey(id, QStringLiteral("password")));
        };
        p.passphraseProvider = [store, id]() {
            return store->retrieveSecret(sshSecretKey(id, QStringLiteral("passphrase")));
        };
    }
    return p;
}

void SshConnection::connectToHost()
{
    QMetaObject::invokeMethod(m_worker, "startConnect", Qt::QueuedConnection,
                              Q_ARG(remote::SshSessionWorker::ConnectParams,
                                    buildConnectParams()));
}

void SshConnection::acceptHostKey()
{
    // Persist the now-trusted key in the app-managed known_hosts (design D9 —
    // not the system ~/.ssh/known_hosts) before letting the worker authenticate.
    if (!m_pendingHostKey.isEmpty()) {
        m_hostKeyStore.add(m_profile.host, m_profile.port, m_pendingHostKey);
        m_pendingHostKey.clear();
    }
    QMetaObject::invokeMethod(m_worker, "acceptHostKey", Qt::QueuedConnection);
}

void SshConnection::rejectHostKey()
{
    m_pendingHostKey.clear();
    QMetaObject::invokeMethod(m_worker, "rejectHostKey", Qt::QueuedConnection);
}

void SshConnection::handleHostKey(const QString &fingerprint, const QByteArray &key)
{
    // Consult the app-managed known_hosts on the UI thread (the worker stays
    // pure transport). Three cases (D9):
    //   - known + matching → auto-accept, no prompt.
    //   - known + changed  → loud warning (hostKeyChanged); do NOT auto-trust.
    //   - unknown          → prompt (hostKeyReceived) — first-connect is never
    //                        silently trusted.
    const QByteArray known = m_hostKeyStore.lookup(m_profile.host, m_profile.port);
    if (!known.isEmpty() && known == key) {
        QMetaObject::invokeMethod(m_worker, "acceptHostKey", Qt::QueuedConnection);
        return;
    }
    m_pendingHostKey = key;
    if (!known.isEmpty() && known != key) {
        emit hostKeyChanged(fingerprint, key);
    } else {
        emit hostKeyReceived(fingerprint, key);
    }
}

void SshConnection::disconnectFromHost()
{
    QMetaObject::invokeMethod(m_worker, "requestDisconnect", Qt::QueuedConnection);
}

SshChannel *SshConnection::openChannel(bool wantPty, const QByteArray &term,
                                       int cols, int rows, const QString &command)
{
    const int logicalId = m_nextLogicalId++;
    auto *ch = new SshChannel(this, logicalId, this);
    m_channels.insert(logicalId, ch);
    connect(ch, &SshChannel::closed, this, [this, logicalId](int) {
        m_channels.remove(logicalId);
    });
    QMetaObject::invokeMethod(m_worker, "requestOpenChannel", Qt::QueuedConnection,
                              Q_ARG(int, logicalId), Q_ARG(bool, wantPty),
                              Q_ARG(QByteArray, term), Q_ARG(int, cols),
                              Q_ARG(int, rows), Q_ARG(QString, command));
    return ch;
}

void SshConnection::writeToChannel(int logicalId, const QByteArray &bytes)
{
    QMetaObject::invokeMethod(m_worker, "requestWrite", Qt::QueuedConnection,
                              Q_ARG(int, logicalId), Q_ARG(QByteArray, bytes));
}

void SshConnection::resizeChannel(int logicalId, int cols, int rows)
{
    QMetaObject::invokeMethod(m_worker, "requestResize", Qt::QueuedConnection,
                              Q_ARG(int, logicalId), Q_ARG(int, cols), Q_ARG(int, rows));
}

void SshConnection::closeChannel(int logicalId)
{
    QMetaObject::invokeMethod(m_worker, "requestCloseChannel", Qt::QueuedConnection,
                              Q_ARG(int, logicalId));
}

} // namespace remote
