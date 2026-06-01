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

#include <QtTest>
#include <QDir>

#include "TerminalCwdResolver.h"
#include "remote/ExecutionContext.h"
#include "remote/LocalExecutionContext.h"

using namespace remote;

// Minimal fake remote context: isRemote() true, configurable state, POSIX
// resolveCwd. No SSH, no network — exercises the resolver's remote branch.
class FakeRemoteContext : public ExecutionContext
{
public:
    explicit FakeRemoteContext(State s) : m_state(s) {}
    bool isRemote() const override { return true; }
    QString displayName() const override { return QStringLiteral("fake"); }
    State state() const override { return m_state; }
    IPtyProcess *createPty(QObject *) override { return nullptr; }
    IGitProcessRunner *createGitRunner(QObject *) override { return nullptr; }
    void exec(const QString &, const QStringList &, const QByteArray &, int,
              ExecCallback) override {}
    IFileSystemBackend *fsBackend() override { return nullptr; }
    QString resolveCwd(const QString &requested) const override
    {
        QString p = requested.isEmpty() ? QStringLiteral("~") : requested;
        p.replace(QLatin1Char('\\'), QLatin1Char('/'));
        return QDir::cleanPath(p);
    }
    void setState(State s) { m_state = s; }

private:
    State m_state;
};

class TestExecutionContextResolver : public QObject
{
    Q_OBJECT

private slots:
    void localContext_resolveCwd_cleansLocalPath();
    void remoteContext_resolveCwd_posixNoLocalStat();

    void resolver_nullContext_matchesLegacyLocal();
    void resolver_localContext_matchesLegacyLocal();
    void resolver_remoteConnected_returnsPosixPath();
    void resolver_remoteDisconnected_returnsEmpty();
    void resolver_remoteEmptyRequested_returnsEmpty();
    void resolver_remotePathNotStattedLocally();
};

void TestExecutionContextResolver::localContext_resolveCwd_cleansLocalPath()
{
    LocalExecutionContext local;
    QCOMPARE(local.isRemote(), false);
    QCOMPARE(local.state(), ExecutionContext::State::Connected);
    QCOMPARE(local.resolveCwd(QStringLiteral("/a/b/../c")), QDir::cleanPath(QStringLiteral("/a/b/../c")));
    QCOMPARE(local.resolveCwd(QString()), QString());
}

void TestExecutionContextResolver::remoteContext_resolveCwd_posixNoLocalStat()
{
    FakeRemoteContext ctx(ExecutionContext::State::Connected);
    // Backslashes normalized to POSIX; lexical clean only (no disk access).
    QCOMPARE(ctx.resolveCwd(QStringLiteral("/srv/app/./logs")), QStringLiteral("/srv/app/logs"));
    QCOMPARE(ctx.resolveCwd(QString()), QStringLiteral("~"));
}

void TestExecutionContextResolver::resolver_nullContext_matchesLegacyLocal()
{
    // A null context must behave exactly like resolveWorkspace on a local path.
    const QString existing = QDir::tempPath();
    QCOMPARE(TerminalCwdResolver::resolveForContext(nullptr, existing),
             TerminalCwdResolver::resolveWorkspace(existing));
}

void TestExecutionContextResolver::resolver_localContext_matchesLegacyLocal()
{
    LocalExecutionContext local;
    const QString existing = QDir::tempPath();
    QCOMPARE(TerminalCwdResolver::resolveForContext(&local, existing),
             TerminalCwdResolver::resolveWorkspace(existing));
    // A non-existent local path still resolves empty (existing behavior).
    const QString bogus = QDir::tempPath() + QStringLiteral("/nope/does/not/exist");
    QCOMPARE(TerminalCwdResolver::resolveForContext(&local, bogus),
             TerminalCwdResolver::resolveWorkspace(bogus));
}

void TestExecutionContextResolver::resolver_remoteConnected_returnsPosixPath()
{
    FakeRemoteContext ctx(ExecutionContext::State::Connected);
    QCOMPARE(TerminalCwdResolver::resolveForContext(&ctx, QStringLiteral("/home/alice/x/../proj")),
             QStringLiteral("/home/alice/proj"));
}

void TestExecutionContextResolver::resolver_remoteDisconnected_returnsEmpty()
{
    FakeRemoteContext ctx(ExecutionContext::State::Disconnected);
    QCOMPARE(TerminalCwdResolver::resolveForContext(&ctx, QStringLiteral("/home/alice")), QString());
}

void TestExecutionContextResolver::resolver_remoteEmptyRequested_returnsEmpty()
{
    FakeRemoteContext ctx(ExecutionContext::State::Connected);
    QCOMPARE(TerminalCwdResolver::resolveForContext(&ctx, QString()), QString());
}

void TestExecutionContextResolver::resolver_remotePathNotStattedLocally()
{
    // A remote path that surely does not exist on the local disk must still be
    // returned verbatim (POSIX-normalized) — proving no local QFileInfo check.
    FakeRemoteContext ctx(ExecutionContext::State::Connected);
    const QString remotePath = QStringLiteral("/nonexistent-on-this-host/deep/remote/dir");
    QCOMPARE(TerminalCwdResolver::resolveForContext(&ctx, remotePath), remotePath);
}

QTEST_APPLESS_MAIN(TestExecutionContextResolver)

#include "test_execution_context_resolver.moc"
