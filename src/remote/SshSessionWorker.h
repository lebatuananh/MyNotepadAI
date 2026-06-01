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

#ifndef REMOTE_SSHSESSIONWORKER_H
#define REMOTE_SSHSESSIONWORKER_H

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QObject>
#include <QString>

#include <functional>
#include <memory>

#include "ISshTransport.h"
#include "SshProfile.h"

class QSocketNotifier;

namespace remote {

// Lives on a dedicated worker QThread. Owns the LIBSSH2_SESSION via an injected
// ISshTransport and is the ONLY place libssh2 is ever touched. Drives:
//   - the connect/auth state machine (D2),
//   - the round-robin read pump (D3),
//   - the write path with the write-notifier invariant (D4),
//   - the FIFO channel-open queue with a cap (D5),
//   - connection-loss detection + ordered cleanup (D8).
//
// The UI-thread facade (SshConnection) talks to it ONLY via queued
// signals/slots — no locks. Tests construct it directly on the test thread,
// inject a FakeSshTransport, and call pumpForTest() to drive the pump without a
// real QSocketNotifier.
class SshSessionWorker : public QObject
{
    Q_OBJECT

public:
    enum class State
    {
        Idle,
        ConnectingSocket,
        Handshaking,
        AwaitingHostKey, // emitted hostKeyReceived; auth gated on accept/reject
        Authenticating,
        Ready,
        Disconnected,
        Failed,
    };
    Q_ENUM(State)

    struct ConnectParams
    {
        QString host;
        int port = 22;
        QString username;
        SshProfile::AuthMethod authMethod = SshProfile::AuthMethod::Agent;
        QString keyPath;
        // Secret providers invoked ON THE WORKER THREAD during auth so the UI
        // thread is never blocked on a keychain syscall (spec: "Secret fetched
        // off the UI thread"). Empty/unset → empty secret.
        std::function<QString()> passwordProvider;
        std::function<QString()> passphraseProvider;
    };

    // Takes ownership of the transport. Default cap = 10 concurrent channels.
    explicit SshSessionWorker(std::unique_ptr<ISshTransport> transport,
                              int channelCap = 10,
                              QObject *parent = nullptr);
    ~SshSessionWorker() override;

    State state() const { return m_state; }

    // --- test hooks ----------------------------------------------------------
    // Run exactly one bounded pump sweep (advance connect/auth, then if Ready
    // service channel setup + reads + writes). Returns nothing; observe via
    // signals + the accessors below.
    void pumpForTest() { pump(); }
    bool writeNotifierEnabledForTest() const { return m_writeNotifierWanted; }
    int liveChannelCountForTest() const { return liveChannelCount(); }
    int queuedChannelCountForTest() const { return m_openQueue.size(); }

public slots:
    // All posted from the UI thread (queued) in production; called directly in
    // tests. startConnect captures params; in production it also wires the
    // socket notifiers once the fd is valid.
    void startConnect(const remote::SshSessionWorker::ConnectParams &params);
    void acceptHostKey();
    void rejectHostKey();

    // logicalId is minted by the UI-thread facade and is stable for the
    // channel's whole life. wantPty + term/cols/rows + command describe the
    // channel setup (PTY shell for the remote terminal).
    void requestOpenChannel(int logicalId, bool wantPty, const QByteArray &term,
                            int cols, int rows, const QString &command);
    void requestResize(int logicalId, int cols, int rows);
    void requestWrite(int logicalId, const QByteArray &bytes);
    void requestCloseChannel(int logicalId);
    void requestDisconnect();

signals:
    void stateChanged(remote::SshSessionWorker::State state);
    void hostKeyReceived(const QString &fingerprint, const QByteArray &key);
    void authFailed(const QString &reason);
    void connected();
    void channelOpened(int logicalId);
    void channelOpenFailed(int logicalId, const QString &reason);
    void dataReady(int logicalId, const QByteArray &bytes);
    void channelClosed(int logicalId, int exitStatus);
    // Connection lost / disconnected with a human reason.
    void disconnected(const QString &reason);

private slots:
    void onSocketActivity();

private:
    enum class ChPhase
    {
        Queued,    // waiting for a slot (counts against queue, not the cap)
        Opening,   // openChannel in flight (counts against the cap)
        NeedPty,   // channel open; requestPty pending
        NeedExec,  // pty done (or skipped); execOrShell pending
        Open,      // fully set up; in the read/write rotation
        Closed,    // terminal; pending removal
    };

    struct Channel
    {
        int logicalId = -1;
        int transportId = -1;
        ChPhase phase = ChPhase::Queued;
        bool wantPty = false;
        QByteArray term;
        int cols = 80;
        int rows = 24;
        QString command;     // empty → interactive shell
        QByteArray pending;  // unsent write bytes (move-appended; no per-byte alloc)
    };

    void setState(State s);
    void pump();                 // single bounded sweep (D3 + D4 + setup)
    void advanceConnect();       // socket→handshake→hostkey→auth→ready
    void advanceChannelSetup();  // open/pty/exec for not-yet-Open channels
    void readSweep();            // round-robin reads (D3)
    void flushPendingWrites();   // write path + notifier invariant (D4)
    void tryStartQueued();       // FIFO dequeue while under cap (D5)
    void enterConnectionLost(const QString &reason); // D8
    void finishChannel(int logicalId, int exitStatus);

    int liveChannelCount() const;        // Opening..Open (counts against cap)
    Channel *channelByTransportId(int transportId);

    void setupNotifiers();       // production only (real fd)
    void teardownNotifiers();
    void setWriteNotifierEnabled(bool on);

    std::unique_ptr<ISshTransport> m_transport;
    int m_channelCap;
    State m_state = State::Idle;

    ConnectParams m_params;
    bool m_hostKeyEmitted = false;
    bool m_hostKeyAccepted = false;
    bool m_hostKeyRejected = false;

    // Live + queued channels. m_channels holds every non-removed channel keyed
    // by logicalId; m_openQueue is the FIFO of Queued logicalIds (D5).
    QHash<int, Channel> m_channels;
    QList<int> m_openQueue;
    // Stable rotation order for the round-robin read sweep (insertion order of
    // Open channels). Kept as a list so every sweep visits each exactly once.
    QList<int> m_rotation;

    QSocketNotifier *m_readNotifier = nullptr;
    QSocketNotifier *m_writeNotifier = nullptr;
    bool m_writeNotifierWanted = false;
    bool m_lost = false;
};

} // namespace remote

Q_DECLARE_METATYPE(remote::SshSessionWorker::ConnectParams)

#endif // REMOTE_SSHSESSIONWORKER_H
