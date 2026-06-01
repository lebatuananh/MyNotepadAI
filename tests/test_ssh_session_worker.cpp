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

// Offline coverage of the SSH transport's correctness-critical behavior, driven
// entirely by FakeSshTransport + SshSessionWorker::pumpForTest() — NO network,
// no sshd, deterministic. Asserts the normative design invariants:
//   group 1: auth state machine + signals
//   group 2: host-key prompt gates auth
//   group 3: FIFO channel-open queue at cap (D5) — none dropped
//   group 4: round-robin anti-starvation read sweep (D3)
//   group 5: write-EAGAIN no-spin; write notifier off after drain (D4)
//   group 6: connection-loss cleanup (D8) — flush, channelClosed(-1), no post-loss write

#include <QtTest>
#include <QHash>
#include <QList>
#include <QSet>
#include <QSignalSpy>

#include <memory>

#include "remote/ISshTransport.h"
#include "remote/SshSessionWorker.h"

using namespace remote;

// Scriptable fake. Every knob is public so each test arranges exactly the
// scenario it needs. socketFd() returns -1 so the worker creates no real
// QSocketNotifier and pumpForTest() drives the pump deterministically.
class FakeSshTransport : public ISshTransport
{
public:
    // connect/auth scripting
    Step connectStep = Step::Ok;
    Step handshakeStep = Step::Ok;
    Step authStep = Step::Ok;
    QByteArray fakeHostKey = QByteArrayLiteral("FAKE-HOST-KEY-BLOB");

    // recorded auth
    int authPasswordCalls = 0;
    int authPublicKeyCalls = 0;
    int authAgentCalls = 0;
    QString lastAuthUser;
    QString lastAuthPassword;

    // channel-open scripting
    Step openStep = Step::Ok;             // Again to simulate EAGAIN on open
    QList<int> openedIds;                 // transport ids assigned, in order
    int nextId = 1;
    int closedCount = 0;
    QList<int> closedIds;

    // per-transportId read script (popped front; empty → Again)
    QHash<int, QList<ReadResult>> readScript;
    QHash<int, int> readCalls;

    // per-transportId write script: number of EAGAINs before it accepts in full
    QHash<int, int> writeEagainsRemaining;
    int writeCalls = 0;

    QHash<int, int> exitStatus;

    Step connectSocket(const QString &, int) override { return connectStep; }
    Step handshake() override { return handshakeStep; }
    QByteArray hostKey() const override { return fakeHostKey; }

    Step authPassword(const QString &username, const QString &password) override
    {
        ++authPasswordCalls;
        lastAuthUser = username;
        lastAuthPassword = password;
        return authStep;
    }
    Step authPublicKey(const QString &username, const QString &, const QString &) override
    {
        ++authPublicKeyCalls;
        lastAuthUser = username;
        return authStep;
    }
    Step authAgent(const QString &username) override
    {
        ++authAgentCalls;
        lastAuthUser = username;
        return authStep;
    }

    OpenResult openChannel() override
    {
        OpenResult r;
        if (openStep != Step::Ok) {
            r.step = openStep;
            return r;
        }
        r.step = Step::Ok;
        r.channelId = nextId++;
        openedIds.append(r.channelId);
        return r;
    }
    Step requestPty(int, const QByteArray &, int, int) override { return Step::Ok; }
    Step resizePty(int, int, int) override { return Step::Ok; }
    Step execOrShell(int, const QString &) override { return Step::Ok; }

    qint64 chWrite(int channelId, const QByteArray &bytes) override
    {
        ++writeCalls;
        int &eagains = writeEagainsRemaining[channelId];
        if (eagains > 0) {
            --eagains;
            return kWriteAgain;
        }
        return bytes.size(); // accept in full
    }

    ReadResult chRead(int channelId) override
    {
        ++readCalls[channelId];
        QList<ReadResult> &q = readScript[channelId];
        if (q.isEmpty()) {
            ReadResult r;
            r.again = true;
            return r;
        }
        return q.takeFirst();
    }

    int chExitStatus(int channelId) override { return exitStatus.value(channelId, 0); }
    void closeChannel(int channelId) override { ++closedCount; closedIds.append(channelId); }
    qintptr socketFd() const override { return -1; } // no real QSocketNotifier in tests
    void disconnect() override {}
};

// Helper to drive the connect/auth handshake to a given point.
static SshSessionWorker::ConnectParams passwordParams(const QString &pw)
{
    SshSessionWorker::ConnectParams p;
    p.host = QStringLiteral("host.example");
    p.port = 22;
    p.username = QStringLiteral("alice");
    p.authMethod = SshProfile::AuthMethod::Password;
    p.passwordProvider = [pw]() { return pw; };
    return p;
}

// Build a worker already driven to Ready, returning the worker + the (observed)
// raw fake pointer. cap controls the channel cap.
static SshSessionWorker *makeReadyWorker(FakeSshTransport **fakeOut, int cap = 10)
{
    auto fake = std::make_unique<FakeSshTransport>();
    *fakeOut = fake.get();
    auto *worker = new SshSessionWorker(std::move(fake), cap);
    worker->startConnect(passwordParams(QStringLiteral("pw")));
    worker->acceptHostKey(); // gate → drives to Ready
    return worker;
}

class TestSshSessionWorker : public QObject
{
    Q_OBJECT

private slots:
    void group1_authStateMachineAndSignals();
    void group1_authFailureEmitsAndDisconnects();
    void group2_hostKeyPromptGatesAuth();
    void group2_hostKeyRejectAborts();
    void group3_fifoQueueAtCap();
    void group4_roundRobinAntiStarvation();
    void group5_writeEagainNoSpin();
    void group6_connectionLossCleanup();
};

// --- group 1: auth state machine + signals ----------------------------------

void TestSshSessionWorker::group1_authStateMachineAndSignals()
{
    auto fake = std::make_unique<FakeSshTransport>();
    FakeSshTransport *f = fake.get();
    SshSessionWorker worker(std::move(fake));

    QSignalSpy stateSpy(&worker, &SshSessionWorker::stateChanged);
    QSignalSpy hostKeySpy(&worker, &SshSessionWorker::hostKeyReceived);
    QSignalSpy connectedSpy(&worker, &SshSessionWorker::connected);

    worker.startConnect(passwordParams(QStringLiteral("s3cret")));
    // Socket+handshake done synchronously; now gated on host key.
    QCOMPARE(worker.state(), SshSessionWorker::State::AwaitingHostKey);
    QCOMPARE(hostKeySpy.count(), 1);
    QCOMPARE(connectedSpy.count(), 0);
    QCOMPARE(f->authPasswordCalls, 0); // not authenticated until accepted

    worker.acceptHostKey();
    QCOMPARE(worker.state(), SshSessionWorker::State::Ready);
    QCOMPARE(connectedSpy.count(), 1);
    QCOMPARE(f->authPasswordCalls, 1);
    QCOMPARE(f->lastAuthUser, QStringLiteral("alice"));
    QCOMPARE(f->lastAuthPassword, QStringLiteral("s3cret")); // provider ran on worker

    // Observed the ordered state progression.
    QVERIFY(stateSpy.count() >= 4); // ConnectingSocket, Handshaking, AwaitingHostKey, Authenticating/Ready
}

void TestSshSessionWorker::group1_authFailureEmitsAndDisconnects()
{
    auto fake = std::make_unique<FakeSshTransport>();
    FakeSshTransport *f = fake.get();
    f->authStep = ISshTransport::Step::Error;
    SshSessionWorker worker(std::move(fake));

    QSignalSpy authFailedSpy(&worker, &SshSessionWorker::authFailed);
    QSignalSpy disconnectedSpy(&worker, &SshSessionWorker::disconnected);

    worker.startConnect(passwordParams(QStringLiteral("bad")));
    worker.acceptHostKey();

    QCOMPARE(f->authPasswordCalls, 1);
    QCOMPARE(authFailedSpy.count(), 1);
    QCOMPARE(disconnectedSpy.count(), 1);
    QCOMPARE(worker.state(), SshSessionWorker::State::Disconnected);
}

// --- group 2: host-key prompt gates auth ------------------------------------

void TestSshSessionWorker::group2_hostKeyPromptGatesAuth()
{
    auto fake = std::make_unique<FakeSshTransport>();
    FakeSshTransport *f = fake.get();
    SshSessionWorker worker(std::move(fake));

    QSignalSpy hostKeySpy(&worker, &SshSessionWorker::hostKeyReceived);
    worker.startConnect(passwordParams(QStringLiteral("pw")));

    // Extra pumps before accept must NOT authenticate.
    worker.pumpForTest();
    worker.pumpForTest();
    QCOMPARE(worker.state(), SshSessionWorker::State::AwaitingHostKey);
    QCOMPARE(f->authPasswordCalls, 0);
    QCOMPARE(hostKeySpy.count(), 1); // emitted exactly once

    // The fingerprint argument is the SHA256 of the fake host key.
    const QString fp = hostKeySpy.first().at(0).toString();
    QVERIFY(fp.startsWith(QStringLiteral("SHA256:")));

    worker.acceptHostKey();
    QCOMPARE(f->authPasswordCalls, 1);
    QCOMPARE(worker.state(), SshSessionWorker::State::Ready);
}

void TestSshSessionWorker::group2_hostKeyRejectAborts()
{
    auto fake = std::make_unique<FakeSshTransport>();
    FakeSshTransport *f = fake.get();
    SshSessionWorker worker(std::move(fake));

    QSignalSpy disconnectedSpy(&worker, &SshSessionWorker::disconnected);
    worker.startConnect(passwordParams(QStringLiteral("pw")));
    worker.rejectHostKey();

    QCOMPARE(f->authPasswordCalls, 0); // never authenticated
    QCOMPARE(worker.state(), SshSessionWorker::State::Disconnected);
    QCOMPARE(disconnectedSpy.count(), 1);
}

// --- group 3: FIFO channel-open queue at cap (D5) ---------------------------

void TestSshSessionWorker::group3_fifoQueueAtCap()
{
    FakeSshTransport *f = nullptr;
    SshSessionWorker *worker = makeReadyWorker(&f, /*cap=*/2);

    QSignalSpy openedSpy(worker, &SshSessionWorker::channelOpened);

    // Five open requests with cap 2 → two open immediately, three queued.
    for (int logicalId = 1; logicalId <= 5; ++logicalId) {
        worker->requestOpenChannel(logicalId, /*wantPty=*/false, QByteArray(), 80, 24, QString());
    }
    QCOMPARE(worker->liveChannelCountForTest(), 2);
    QCOMPARE(worker->queuedChannelCountForTest(), 3);
    QCOMPARE(openedSpy.count(), 2);

    // The first two to open are logical ids 1 and 2 (FIFO).
    QCOMPARE(openedSpy.at(0).at(0).toInt(), 1);
    QCOMPARE(openedSpy.at(1).at(0).toInt(), 2);

    // Close one → exactly one queued (the head, id 3) opens next; none dropped.
    worker->requestCloseChannel(1);
    QCOMPARE(worker->liveChannelCountForTest(), 2);
    QCOMPARE(worker->queuedChannelCountForTest(), 2);
    QCOMPARE(openedSpy.count(), 3);
    QCOMPARE(openedSpy.at(2).at(0).toInt(), 3);

    // Drain the rest in order: 4 then 5.
    worker->requestCloseChannel(2);
    QCOMPARE(openedSpy.at(3).at(0).toInt(), 4);
    worker->requestCloseChannel(3);
    QCOMPARE(openedSpy.at(4).at(0).toInt(), 5);
    QCOMPARE(worker->queuedChannelCountForTest(), 0);

    delete worker;
}

// --- group 4: round-robin anti-starvation (D3) ------------------------------

void TestSshSessionWorker::group4_roundRobinAntiStarvation()
{
    FakeSshTransport *f = nullptr;
    SshSessionWorker *worker = makeReadyWorker(&f, /*cap=*/10);

    // Open three channels (transport ids 1,2,3 in open order). Empty read
    // script during setup → reads return Again harmlessly.
    for (int logicalId = 1; logicalId <= 3; ++logicalId) {
        worker->requestOpenChannel(logicalId, false, QByteArray(), 80, 24, QString());
    }
    QCOMPARE(worker->liveChannelCountForTest(), 3);

    // Channel A (transport 1) is chatty: multiple chunks this sweep, then EAGAIN
    // (empty queue → Again). B (2) and C (3) each have one chunk. The sweep must
    // service B and C in the SAME pass as A — that is the anti-starvation
    // invariant.
    auto data = [](const char *s) {
        ISshTransport::ReadResult r;
        r.data = QByteArray(s);
        return r;
    };
    f->readScript[1] = { data("a1"), data("a2"), data("a3") };
    f->readScript[2] = { data("b1") };
    f->readScript[3] = { data("c1") };
    f->readCalls.clear();

    QSignalSpy dataSpy(worker, &SshSessionWorker::dataReady);
    worker->pumpForTest(); // exactly one bounded sweep

    // Every open channel was read at least once this sweep (none skipped),
    // and each chatty/quiet channel was drained to EAGAIN.
    QVERIFY(f->readCalls.value(1) >= 1);
    QVERIFY(f->readCalls.value(2) >= 1);
    QVERIFY(f->readCalls.value(3) >= 1);

    // dataReady fired for all three logical channels in this single sweep.
    QSet<int> sawData;
    for (const QList<QVariant> &args : dataSpy) {
        sawData.insert(args.at(0).toInt());
    }
    QVERIFY(sawData.contains(1));
    QVERIFY(sawData.contains(2)); // B not starved by the chatty A
    QVERIFY(sawData.contains(3)); // C not starved by the chatty A

    delete worker;
}

// --- group 5: write-EAGAIN no-spin; notifier off after drain (D4) -----------

void TestSshSessionWorker::group5_writeEagainNoSpin()
{
    FakeSshTransport *f = nullptr;
    SshSessionWorker *worker = makeReadyWorker(&f, /*cap=*/10);

    worker->requestOpenChannel(1, false, QByteArray(), 80, 24, QString());
    QCOMPARE(worker->liveChannelCountForTest(), 1);

    // First write returns EAGAIN once (send buffer full), then accepts in full.
    f->writeEagainsRemaining[1] = 1;
    f->writeCalls = 0;

    worker->requestWrite(1, QByteArrayLiteral("keystroke"));
    // One chWrite → EAGAIN → notifier ENABLED while bytes pending.
    QCOMPARE(f->writeCalls, 1);
    QVERIFY(worker->writeNotifierEnabledForTest());

    // Socket becomes writable → one pump sweep flushes the pending bytes.
    worker->pumpForTest();
    QCOMPARE(f->writeCalls, 2);                       // exactly one more write
    QVERIFY(!worker->writeNotifierEnabledForTest());  // INVARIANT: off when drained

    // Further pumps with nothing pending do NOT spin (no extra chWrite, notifier
    // stays off).
    worker->pumpForTest();
    worker->pumpForTest();
    QCOMPARE(f->writeCalls, 2);
    QVERIFY(!worker->writeNotifierEnabledForTest());

    delete worker;
}

// --- group 6: connection-loss cleanup (D8) ----------------------------------

void TestSshSessionWorker::group6_connectionLossCleanup()
{
    FakeSshTransport *f = nullptr;
    SshSessionWorker *worker = makeReadyWorker(&f, /*cap=*/10);

    worker->requestOpenChannel(1, false, QByteArray(), 80, 24, QString());
    QCOMPARE(worker->liveChannelCountForTest(), 1);

    // The channel produces one chunk, then the socket drops (fatal read error).
    ISshTransport::ReadResult chunk;
    chunk.data = QByteArrayLiteral("hello");
    ISshTransport::ReadResult dropped;
    dropped.error = true;
    f->readScript[1] = { chunk, dropped };

    QSignalSpy dataSpy(worker, &SshSessionWorker::dataReady);
    QSignalSpy closedSpy(worker, &SshSessionWorker::channelClosed);
    QSignalSpy disconnectedSpy(worker, &SshSessionWorker::disconnected);

    worker->pumpForTest();

    // Already-read bytes were flushed to the consumer BEFORE the channel closed.
    QCOMPARE(dataSpy.count(), 1);
    QCOMPARE(dataSpy.first().at(1).toByteArray(), QByteArrayLiteral("hello"));

    // The channel was closed reporting an abnormal exit (-1, never a fake 0).
    QCOMPARE(closedSpy.count(), 1);
    QCOMPARE(closedSpy.first().at(0).toInt(), 1);
    QCOMPARE(closedSpy.first().at(1).toInt(), -1);

    QCOMPARE(disconnectedSpy.count(), 1);
    QCOMPARE(worker->state(), SshSessionWorker::State::Disconnected);

    // A write after loss is a no-op: nothing is posted to the (now stopped)
    // worker — chWrite is never called.
    const int writesBefore = f->writeCalls;
    worker->requestWrite(1, QByteArrayLiteral("late"));
    QCOMPARE(f->writeCalls, writesBefore);

    delete worker;
}

QTEST_MAIN(TestSshSessionWorker)

#include "test_ssh_session_worker.moc"

