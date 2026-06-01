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

#ifndef AI_CREDENTIAL_STORE_H
#define AI_CREDENTIAL_STORE_H

#include <QObject>
#include <QString>

namespace ai {

// Resolve / store / clear the commit-message-generation API key via the
// platform OS keychain — Windows DPAPI, macOS Keychain Services, Linux
// libsecret (runtime-loaded). Env vars override the keychain on retrieve:
//
//   NOTEPADAI_COMMIT_API_KEY      — literal key value, takes precedence
//   NOTEPADAI_COMMIT_API_KEY_FILE — path to a file whose first line is the key
//
// The store NEVER writes the key value to settings.ini. The settings flag
// Ai/CommitMessageApiKeyConfigured tracks "user has stored a key" for the
// synchronous UI enable/disable check; the value itself lives only in the OS
// keychain.
//
// retrieve()/store()/clear() are synchronous to keep the surface minimal.
// They are called only at user-initiated moments (Preferences save, generation
// trigger) so blocking briefly on a keychain syscall is acceptable.
class CredentialStore : public QObject
{
    Q_OBJECT

public:
    explicit CredentialStore(QObject *parent = nullptr);
    ~CredentialStore() override;

    // Synchronous check used by UI updateActionsEnabled: looks at env vars
    // (sync) and then the persisted "configured" flag. Does NOT touch the
    // keychain itself — that would be O(slow-syscall) per UI refresh.
    bool isApiKeyAvailable() const;

    // Synchronous retrieval. Env override wins. Otherwise reads from OS
    // keychain. Returns empty string on miss / error (errorOut populated when
    // non-null and an error occurred — not when the value is simply absent).
    QString retrieveApiKey(QString *errorOut = nullptr) const;

    // Synchronous store to OS keychain. On success, sets the configured flag.
    // Returns true on success. errorOut populated on failure if non-null.
    bool storeApiKey(const QString &value, QString *errorOut = nullptr);

    // Synchronous clear from OS keychain. On success, clears the configured
    // flag. Returns true on success or when no key was present.
    bool clearApiKey(QString *errorOut = nullptr);

    // True if the platform backend is operational (e.g., libsecret loaded on
    // Linux). When false, UI should show a hint that env override is required.
    bool isBackendAvailable() const;

    // --- Keyed secret API ----------------------------------------------------
    //
    // General-purpose OS-keychain storage keyed by an arbitrary account name.
    // Used for SSH secrets under the schema `ssh-profile:<profileId>:<kind>`
    // where `kind` is `password` or `passphrase`. NEVER writes the value to
    // settings in plaintext. Synchronous; the SSH connect path calls
    // retrieveSecret() on the worker thread so the UI thread is not blocked.
    //
    // The legacy storeApiKey/retrieveApiKey/clearApiKey are thin wrappers over
    // this keyed API using the fixed account name kApiKeyAccount, so their
    // behavior is byte-for-byte unchanged.
    QString retrieveSecret(const QString &key, QString *errorOut = nullptr) const;
    bool storeSecret(const QString &key, const QString &value, QString *errorOut = nullptr);
    bool clearSecret(const QString &key, QString *errorOut = nullptr);

    // Fixed account name used by the legacy API-key wrappers. Public so a test
    // can assert the wrapper/keyed equivalence.
    static QString apiKeyAccount();

signals:
    void apiKeyConfiguredChanged(bool configured);

private:
    // Platform-impl seam: implemented in CredentialStore_{Windows,Mac,Linux}.cpp.
    // Each returns whether the operation succeeded and writes an optional error
    // string. `account` selects which keychain entry to act on. retrieve
    // returns empty value on "not found".
    bool platformRetrieve(const QString &account, QString *outValue, QString *errorOut) const;
    bool platformStore(const QString &account, const QString &value, QString *errorOut);
    bool platformClear(const QString &account, QString *errorOut);
    bool platformAvailable() const;

    // Env-override probe (sync, fast). Implemented in CredentialStore_Common.cpp.
    static QString envOverrideKey();
};

} // namespace ai

#endif // AI_CREDENTIAL_STORE_H
