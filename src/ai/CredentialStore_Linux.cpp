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

// Linux libsecret backend, loaded via QLibrary at runtime. When libsecret-1
// is unavailable (headless servers, minimal containers), platformAvailable()
// returns false and callers fall back to the env-var override path.

#include "CredentialStore.h"

#include <QByteArray>
#include <QLibrary>
#include <QMutex>
#include <QMutexLocker>

namespace ai {

namespace {

constexpr auto kService = "NotepadAI";

// Minimal GLib types we need without pulling in GLib headers.
using GErrorPtr = void *;
struct SecretSchemaAttribute {
    const char *name;
    int         type;   // SECRET_SCHEMA_ATTRIBUTE_STRING == 0
};
struct SecretSchema {
    const char *name;
    int         flags;  // SECRET_SCHEMA_NONE == 0
    SecretSchemaAttribute attributes[32];
    // tail-padded for ABI compatibility
    int reserved;
    void *reserved1;
    void *reserved2;
    void *reserved3;
    void *reserved4;
    void *reserved5;
    void *reserved6;
    void *reserved7;
};

using SecretPasswordStoreSync_fn = int (*)(const SecretSchema *, const char *,
                                           const char *, const char *,
                                           void * /* cancellable */,
                                           GErrorPtr *,
                                           const char *, const char *,
                                           const char *, const char *,
                                           void * /* sentinel */);
using SecretPasswordLookupSync_fn = char *(*)(const SecretSchema *,
                                              void *, GErrorPtr *,
                                              const char *, const char *,
                                              const char *, const char *,
                                              void *);
using SecretPasswordClearSync_fn = int (*)(const SecretSchema *,
                                           void *, GErrorPtr *,
                                           const char *, const char *,
                                           const char *, const char *,
                                           void *);
using SecretPasswordFree_fn      = void (*)(char *);
using GErrorFree_fn              = void (*)(GErrorPtr);
using GErrorMessage_fn           = const char *(*)(GErrorPtr);

struct LibSecret {
    QLibrary lib;
    QLibrary glib;
    SecretPasswordStoreSync_fn  store_sync  = nullptr;
    SecretPasswordLookupSync_fn lookup_sync = nullptr;
    SecretPasswordClearSync_fn  clear_sync  = nullptr;
    SecretPasswordFree_fn       free_pw     = nullptr;
    GErrorFree_fn               g_error_free = nullptr;
    SecretSchema                schema {};
    bool ok = false;

    bool load() {
        lib.setFileName(QStringLiteral("secret-1"));
        lib.setLoadHints(QLibrary::ResolveAllSymbolsHint);
        if (!lib.load()) return false;
        glib.setFileName(QStringLiteral("glib-2.0"));
        glib.load();   // optional — only used for g_error_free

        store_sync  = reinterpret_cast<SecretPasswordStoreSync_fn>(lib.resolve("secret_password_store_sync"));
        lookup_sync = reinterpret_cast<SecretPasswordLookupSync_fn>(lib.resolve("secret_password_lookup_sync"));
        clear_sync  = reinterpret_cast<SecretPasswordClearSync_fn>(lib.resolve("secret_password_clear_sync"));
        free_pw     = reinterpret_cast<SecretPasswordFree_fn>(lib.resolve("secret_password_free"));
        if (glib.isLoaded()) {
            g_error_free = reinterpret_cast<GErrorFree_fn>(glib.resolve("g_error_free"));
        }
        if (!store_sync || !lookup_sync || !clear_sync || !free_pw) return false;

        schema.name = "com.notepadai.CommitMessageApiKey";
        schema.flags = 0;
        schema.attributes[0] = { "service", 0 };
        schema.attributes[1] = { "account", 0 };
        schema.attributes[2] = { nullptr, 0 };
        ok = true;
        return true;
    }
};

LibSecret &libSecret()
{
    static LibSecret L;
    static QMutex m;
    QMutexLocker lock(&m);
    if (!L.ok && !L.lib.isLoaded()) L.load();
    return L;
}

} // namespace

bool CredentialStore::platformAvailable() const
{
    return libSecret().ok;
}

bool CredentialStore::platformRetrieve(const QString &account, QString *outValue, QString *errorOut) const
{
    auto &S = libSecret();
    if (!S.ok) {
        if (errorOut) *errorOut = QStringLiteral("libsecret unavailable");
        if (outValue) outValue->clear();
        return false;
    }
    const QByteArray accountUtf8 = account.toUtf8();
    GErrorPtr err = nullptr;
    char *pw = S.lookup_sync(&S.schema, nullptr, &err,
                             "service", kService,
                             "account", accountUtf8.constData(),
                             nullptr);
    if (err) {
        if (errorOut) *errorOut = QStringLiteral("libsecret lookup failed");
        if (S.g_error_free) S.g_error_free(err);
        if (outValue) outValue->clear();
        return false;
    }
    if (!pw) {
        if (outValue) outValue->clear();
        return true;   // not found is not an error
    }
    if (outValue) *outValue = QString::fromUtf8(pw);
    S.free_pw(pw);
    return true;
}

bool CredentialStore::platformStore(const QString &account, const QString &value, QString *errorOut)
{
    auto &S = libSecret();
    if (!S.ok) {
        if (errorOut) *errorOut = QStringLiteral("libsecret unavailable");
        return false;
    }
    const QByteArray accountUtf8 = account.toUtf8();
    GErrorPtr err = nullptr;
    const QByteArray utf8 = value.toUtf8();
    const int r = S.store_sync(&S.schema, "default",
                               "NotepadAI secret",
                               utf8.constData(),
                               nullptr, &err,
                               "service", kService,
                               "account", accountUtf8.constData(),
                               nullptr);
    if (err) {
        if (errorOut) *errorOut = QStringLiteral("libsecret store failed");
        if (S.g_error_free) S.g_error_free(err);
        return false;
    }
    return r != 0;
}

bool CredentialStore::platformClear(const QString &account, QString *errorOut)
{
    auto &S = libSecret();
    if (!S.ok) {
        if (errorOut) *errorOut = QStringLiteral("libsecret unavailable");
        return false;
    }
    const QByteArray accountUtf8 = account.toUtf8();
    GErrorPtr err = nullptr;
    S.clear_sync(&S.schema, nullptr, &err,
                 "service", kService,
                 "account", accountUtf8.constData(),
                 nullptr);
    if (err) {
        if (errorOut) *errorOut = QStringLiteral("libsecret clear failed");
        if (S.g_error_free) S.g_error_free(err);
        return false;
    }
    return true;
}

} // namespace ai
