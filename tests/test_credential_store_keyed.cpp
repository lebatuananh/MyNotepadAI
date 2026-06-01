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

#include "ai/CredentialStore.h"

using ai::CredentialStore;

// The keychain backend may be unavailable in headless CI (Linux without
// libsecret; a locked macOS keychain). These helpers let the suite skip rather
// than hard-fail in those environments — Windows DPAPI is always available, so
// the keyed API is fully exercised there. CI still needs no SSH server.
static bool backendUsable(CredentialStore &store)
{
    if (!store.isBackendAvailable()) {
        return false;
    }
    // Probe a real store/clear round-trip; some platforms report available but
    // refuse writes in CI.
    const QString probeKey = QStringLiteral("ssh-profile:__probe__:password");
    QString err;
    if (!store.storeSecret(probeKey, QStringLiteral("x"), &err)) {
        return false;
    }
    store.clearSecret(probeKey);
    return true;
}

class TestCredentialStoreKeyed : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();

    void keyedStoreRetrieveClear();
    void distinctKeysAreIndependent();
    void apiKeyWrapperMatchesKeyedAccount();

private:
    QTemporaryDir tempDir;
};

void TestCredentialStoreKeyed::initTestCase()
{
    QVERIFY(tempDir.isValid());
    QCoreApplication::setOrganizationName("NotepadNextTest");
    QCoreApplication::setApplicationName("NotepadNextTestCred");
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, tempDir.path());
}

void TestCredentialStoreKeyed::keyedStoreRetrieveClear()
{
    CredentialStore store;
    if (!backendUsable(store)) {
        QSKIP("No usable OS keychain backend in this environment");
    }
    const QString key = QStringLiteral("ssh-profile:abc123:password");
    const QString secret = QStringLiteral("hunter2-Δ");   // non-ASCII round-trip

    QVERIFY(store.storeSecret(key, secret));
    QCOMPARE(store.retrieveSecret(key), secret);

    QVERIFY(store.clearSecret(key));
    QCOMPARE(store.retrieveSecret(key), QString());
}

void TestCredentialStoreKeyed::distinctKeysAreIndependent()
{
    CredentialStore store;
    if (!backendUsable(store)) {
        QSKIP("No usable OS keychain backend in this environment");
    }
    const QString k1 = QStringLiteral("ssh-profile:p1:password");
    const QString k2 = QStringLiteral("ssh-profile:p1:passphrase");
    QVERIFY(store.storeSecret(k1, QStringLiteral("pw")));
    QVERIFY(store.storeSecret(k2, QStringLiteral("pp")));
    QCOMPARE(store.retrieveSecret(k1), QStringLiteral("pw"));
    QCOMPARE(store.retrieveSecret(k2), QStringLiteral("pp"));
    // Clearing one leaves the other intact.
    QVERIFY(store.clearSecret(k1));
    QCOMPARE(store.retrieveSecret(k1), QString());
    QCOMPARE(store.retrieveSecret(k2), QStringLiteral("pp"));
    store.clearSecret(k2);
}

void TestCredentialStoreKeyed::apiKeyWrapperMatchesKeyedAccount()
{
    CredentialStore store;
    if (!backendUsable(store)) {
        QSKIP("No usable OS keychain backend in this environment");
    }
    // The legacy API-key wrapper must be byte-for-byte equivalent to the keyed
    // API on the fixed account name — store via the wrapper, read via the keyed
    // API on apiKeyAccount(), and vice-versa.
    const QString value = QStringLiteral("sk-test-12345");
    QVERIFY(store.storeApiKey(value));
    QCOMPARE(store.retrieveSecret(CredentialStore::apiKeyAccount()), value);

    QVERIFY(store.clearApiKey());
    QCOMPARE(store.retrieveApiKey(), QString());

    // Reverse: store via keyed API on the account name, read via the wrapper.
    QVERIFY(store.storeSecret(CredentialStore::apiKeyAccount(), value));
    QCOMPARE(store.retrieveApiKey(), value);
    store.clearApiKey();
}

QTEST_MAIN(TestCredentialStoreKeyed)

#include "test_credential_store_keyed.moc"
