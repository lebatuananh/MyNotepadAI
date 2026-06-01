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
#include <QCoreApplication>
#include <QSettings>
#include <QTemporaryDir>

#include "remote/SshProfile.h"
#include "remote/SshProfileRegistry.h"

using namespace remote;

class TestSshProfileRegistry : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void init();

    void addAndList();
    void roundTripAcrossInstances();
    void updateProfile();
    void removeProfile();
    void duplicateIdRefused();
    void uriFormatParse();
    void uriLegacyLocalPathsNotSsh();

private:
    QTemporaryDir tempDir;
};

void TestSshProfileRegistry::initTestCase()
{
    QVERIFY(tempDir.isValid());
    QCoreApplication::setOrganizationName("NotepadNextTest");
    QCoreApplication::setApplicationName("NotepadNextTestSsh");
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, tempDir.path());
}

void TestSshProfileRegistry::init()
{
    QSettings s;
    s.clear();
    s.sync();
}

static SshProfile makeProfile(const QString &id, const QString &host)
{
    SshProfile p;
    p.id = id;
    p.host = host;
    p.port = 2222;
    p.username = QStringLiteral("alice");
    p.authMethod = SshProfile::AuthMethod::KeyFile;
    p.keyPath = QStringLiteral("/home/alice/.ssh/id_ed25519");
    p.lastRemotePath = QStringLiteral("/home/alice/project");
    p.lastConnectedMs = 1234567890;
    return p;
}

void TestSshProfileRegistry::addAndList()
{
    QSettings s;
    SshProfileRegistry reg(&s);
    QCOMPARE(reg.profiles().size(), 0);
    QVERIFY(reg.addProfile(makeProfile(QStringLiteral("id-a"), QStringLiteral("a.example"))));
    QVERIFY(reg.addProfile(makeProfile(QStringLiteral("id-b"), QStringLiteral("b.example"))));
    QCOMPARE(reg.profiles().size(), 2);
    QVERIFY(reg.contains(QStringLiteral("id-a")));
}

void TestSshProfileRegistry::roundTripAcrossInstances()
{
    {
        QSettings s;
        SshProfileRegistry reg(&s);
        QVERIFY(reg.addProfile(makeProfile(QStringLiteral("id-x"), QStringLiteral("x.example"))));
        s.sync();
    }
    QSettings s2;
    SshProfileRegistry reg2(&s2);
    QCOMPARE(reg2.profiles().size(), 1);
    const SshProfile p = reg2.profile(QStringLiteral("id-x"));
    QCOMPARE(p.host, QStringLiteral("x.example"));
    QCOMPARE(p.port, 2222);
    QCOMPARE(p.username, QStringLiteral("alice"));
    QCOMPARE(p.authMethod, SshProfile::AuthMethod::KeyFile);
    QCOMPARE(p.keyPath, QStringLiteral("/home/alice/.ssh/id_ed25519"));
    QCOMPARE(p.lastRemotePath, QStringLiteral("/home/alice/project"));
    QCOMPARE(p.lastConnectedMs, qint64(1234567890));
}

void TestSshProfileRegistry::updateProfile()
{
    QSettings s;
    SshProfileRegistry reg(&s);
    SshProfile p = makeProfile(QStringLiteral("id-u"), QStringLiteral("old.example"));
    QVERIFY(reg.addProfile(p));
    p.host = QStringLiteral("new.example");
    p.port = 22;
    QVERIFY(reg.updateProfile(p));
    QCOMPARE(reg.profile(QStringLiteral("id-u")).host, QStringLiteral("new.example"));
    QCOMPARE(reg.profile(QStringLiteral("id-u")).port, 22);
    QCOMPARE(reg.profiles().size(), 1);
}

void TestSshProfileRegistry::removeProfile()
{
    QSettings s;
    SshProfileRegistry reg(&s);
    QVERIFY(reg.addProfile(makeProfile(QStringLiteral("id-r"), QStringLiteral("r.example"))));
    QVERIFY(reg.removeProfile(QStringLiteral("id-r")));
    QCOMPARE(reg.profiles().size(), 0);
    QVERIFY(!reg.contains(QStringLiteral("id-r")));
    // Removal persists.
    s.sync();
    QSettings s2;
    SshProfileRegistry reg2(&s2);
    QCOMPARE(reg2.profiles().size(), 0);
}

void TestSshProfileRegistry::duplicateIdRefused()
{
    QSettings s;
    SshProfileRegistry reg(&s);
    QVERIFY(reg.addProfile(makeProfile(QStringLiteral("dup"), QStringLiteral("a"))));
    QVERIFY(!reg.addProfile(makeProfile(QStringLiteral("dup"), QStringLiteral("b"))));
    QCOMPARE(reg.profiles().size(), 1);
}

void TestSshProfileRegistry::uriFormatParse()
{
    const QString uri = formatSshUri(QStringLiteral("9f3c1a"), QStringLiteral("/home/alice/project"));
    QCOMPARE(uri, QStringLiteral("ssh://9f3c1a/home/alice/project"));
    QVERIFY(isSshUri(uri));

    const SshUri parsed = parseSshUri(uri);
    QVERIFY(parsed.valid);
    QCOMPARE(parsed.profileId, QStringLiteral("9f3c1a"));
    QCOMPARE(parsed.remotePath, QStringLiteral("/home/alice/project"));

    // No path → defaults to remote root.
    const SshUri rootOnly = parseSshUri(QStringLiteral("ssh://abc"));
    QVERIFY(rootOnly.valid);
    QCOMPARE(rootOnly.profileId, QStringLiteral("abc"));
    QCOMPARE(rootOnly.remotePath, QStringLiteral("/"));

    // format with empty path normalizes to root.
    QCOMPARE(formatSshUri(QStringLiteral("abc"), QString()), QStringLiteral("ssh://abc/"));
}

void TestSshProfileRegistry::uriLegacyLocalPathsNotSsh()
{
    // Legacy local-path entries never begin with ssh:// → zero migration.
    QVERIFY(!isSshUri(QStringLiteral("/home/alice/project")));
    QVERIFY(!isSshUri(QStringLiteral("C:/Users/alice/project")));
    QVERIFY(!isSshUri(QString()));
    const SshUri bad = parseSshUri(QStringLiteral("/home/alice"));
    QVERIFY(!bad.valid);
}

QTEST_APPLESS_MAIN(TestSshProfileRegistry)

#include "test_ssh_profile_registry.moc"
