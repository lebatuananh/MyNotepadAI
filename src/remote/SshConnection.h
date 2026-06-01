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

#ifndef REMOTE_SSHCONNECTION_H
#define REMOTE_SSHCONNECTION_H

#include <QByteArray>
#include <QHash>
#include <QObject>
#include <QPointer>
#include <QString>

#include <functional>
#include <memory>

#include "SshProfile.h"
#include "SshHostKeyStore.h"
#include "SshSessionWorker.h"

class QThread;

namespace ai { class CredentialStore; }

namespace remote {

class ISshTransport;
class SshChannel;

// UI-thread facade over one SSH connection. Spawns and owns the worker QThread
// (the worker runs Libssh2Transport / FakeSshTransport), owns the per-channel
// SshChannel proxies, and relays state. ALL UI↔worker communication is via
// queued connections — no locks. One SshConnection == one TCP connection ==
// one LIBSSH2_SESSION (multiplexing many channels).
class SshConnection : public QObject
{
    Q_OBJECT

public:
    using State = SshSessionWorker::State;

    // Production: builds a Libssh2Transport internally.
    SshConnection(const SshProfile &profile,
                  ai::CredentialStore *credentialStore,
                  QObject *parent = nullptr);
    // Test / advanced: inject a transport (e.g. FakeSshTransport). Takes
    // ownership and moves it onto the worker thread.
    SshConnection(const SshProfile &profile,
                  std::unique_ptr<ISshTransport> transport,
                  ai::CredentialStore *credentialStore,
                  QObject *parent = nullptr);
    ~SshConnection() override;

    const SshProfile &profile() const { return m_profile; }
    State state() const { return m_state; }
    bool isConnected() const { return m_state == State::Ready; }

    // Begin the staged connect. Host-key prompts surface via hostKeyReceived;
    // the caller MUST respond with acceptHostKey()/rejectHostKey(). A key that
    // matches the app-managed known_hosts is auto-accepted (no prompt); a key
    // that DIFFERS from the stored one surfaces hostKeyChanged (MITM guard) and
    // is NOT auto-trusted.
    void connectToHost();
    void acceptHostKey();
    void rejectHostKey();
    void disconnectFromHost();

    // Open a new logical channel. Returns an owned proxy immediately; the actual
    // SSH channel opens asynchronously (channel becomes usable on channelReady).
    // wantPty + term/cols/rows + command describe a remote PTY shell.
    SshChannel *openChannel(bool wantPty, const QByteArray &term, int cols, int rows,
                            const QString &command);

    // Posted to the worker (queued). Called by SshChannel.
    void writeToChannel(int logicalId, const QByteArray &bytes);
    void resizeChannel(int logicalId, int cols, int rows);
    void closeChannel(int logicalId);

signals:
    void stateChanged(remote::SshConnection::State state);
    // Unknown host: prompt the user (first-connect TOFU is NOT silent).
    void hostKeyReceived(const QString &fingerprint, const QByteArray &key);
    // Known host whose key CHANGED since last accepted — loud warning, no
    // silent proceed (MITM guard, design D9).
    void hostKeyChanged(const QString &fingerprint, const QByteArray &key);
    void authFailed(const QString &reason);
    void connected();
    void connectionLost(const QString &reason);
    void channelReady(int logicalId);
    void channelOpenFailed(int logicalId, const QString &reason);

private:
    void init(std::unique_ptr<ISshTransport> transport);
    void handleHostKey(const QString &fingerprint, const QByteArray &key);
    SshSessionWorker::ConnectParams buildConnectParams() const;

    SshProfile m_profile;
    ai::CredentialStore *m_credentialStore;
    SshHostKeyStore m_hostKeyStore;
    QByteArray m_pendingHostKey;          // key awaiting user accept (then persisted)
    QThread *m_thread = nullptr;
    SshSessionWorker *m_worker = nullptr; // lives on m_thread; deleted via thread teardown
    State m_state = State::Idle;
    QHash<int, SshChannel *> m_channels;  // logicalId → proxy (UI thread owns)
    int m_nextLogicalId = 1;
};

} // namespace remote

#endif // REMOTE_SSHCONNECTION_H
