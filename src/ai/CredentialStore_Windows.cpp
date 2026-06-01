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

// Windows DPAPI-backed credential store. The ciphertext blob is stored in
// QSettings under Ai/Secrets/<account> (base64); the key never lives in
// plaintext on disk. CryptProtectData with CRYPTPROTECT_LOCAL_MACHINE binds
// the blob to the machine so it survives user logout but does not round-trip
// to a different machine. The legacy commit-message API key keeps its original
// settings key (Ai/CommitMessageApiKeyBlob) so existing installs resolve
// unchanged; all other accounts (e.g. SSH secrets) use the per-account group.

#include "CredentialStore.h"
#include "ApplicationSettings.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QString>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wincrypt.h>

namespace ai {

namespace {

constexpr auto kLegacyApiKeyAccount = "commit-message-api-key";
constexpr auto kLegacyApiKeySettingKey = "Ai/CommitMessageApiKeyBlob";

// Map an account name to its settings blob key. The legacy commit-message API
// key keeps its historical key for byte-for-byte back-compat; everything else
// lands under a per-account group.
QString blobSettingKey(const QString &account)
{
    if (account == QLatin1String(kLegacyApiKeyAccount)) {
        return QLatin1String(kLegacyApiKeySettingKey);
    }
    return QStringLiteral("Ai/Secrets/") + account;
}

ApplicationSettings *findSettings()
{
    if (auto *app = QCoreApplication::instance()) {
        for (QObject *child : app->children()) {
            if (auto *s = qobject_cast<ApplicationSettings *>(child)) return s;
        }
    }
    return nullptr;
}

QByteArray loadBlob(const QString &account)
{
    const QString key = blobSettingKey(account);
    if (auto *s = findSettings()) {
        return QByteArray::fromBase64(s->value(key).toByteArray());
    }
    ApplicationSettings transient;
    return QByteArray::fromBase64(transient.value(key).toByteArray());
}

void saveBlob(const QString &account, const QByteArray &blob)
{
    const QString key = blobSettingKey(account);
    if (auto *s = findSettings()) {
        s->setValue(key, blob.toBase64());
        return;
    }
    ApplicationSettings transient;
    transient.setValue(key, blob.toBase64());
}

void removeBlob(const QString &account)
{
    const QString key = blobSettingKey(account);
    if (auto *s = findSettings()) { s->remove(key); return; }
    ApplicationSettings transient;
    transient.remove(key);
}

QString lastErrorMessage()
{
    const DWORD code = ::GetLastError();
    LPWSTR buf = nullptr;
    const DWORD n = ::FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, 0, reinterpret_cast<LPWSTR>(&buf), 0, nullptr);
    QString msg;
    if (n > 0 && buf) msg = QString::fromWCharArray(buf, int(n)).trimmed();
    if (buf) ::LocalFree(buf);
    if (msg.isEmpty()) msg = QStringLiteral("Windows error %1").arg(code);
    return msg;
}

} // namespace

bool CredentialStore::platformAvailable() const { return true; }

bool CredentialStore::platformRetrieve(const QString &account, QString *outValue, QString *errorOut) const
{
    const QByteArray blob = loadBlob(account);
    if (blob.isEmpty()) {
        if (outValue) outValue->clear();
        return true;   // not configured is not an error
    }

    DATA_BLOB in{};
    in.cbData = static_cast<DWORD>(blob.size());
    in.pbData = reinterpret_cast<BYTE *>(const_cast<char *>(blob.constData()));
    DATA_BLOB out{};

    const BOOL ok = ::CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr,
                                         CRYPTPROTECT_UI_FORBIDDEN, &out);
    if (!ok) {
        if (errorOut) *errorOut = lastErrorMessage();
        if (outValue) outValue->clear();
        return false;
    }
    if (outValue) {
        *outValue = QString::fromUtf8(reinterpret_cast<const char *>(out.pbData),
                                      static_cast<int>(out.cbData));
    }
    ::SecureZeroMemory(out.pbData, out.cbData);
    ::LocalFree(out.pbData);
    return true;
}

bool CredentialStore::platformStore(const QString &account, const QString &value, QString *errorOut)
{
    const QByteArray utf8 = value.toUtf8();
    DATA_BLOB in{};
    in.cbData = static_cast<DWORD>(utf8.size());
    in.pbData = reinterpret_cast<BYTE *>(const_cast<char *>(utf8.constData()));
    DATA_BLOB out{};

    const BOOL ok = ::CryptProtectData(&in, L"NotepadAI secret",
                                       nullptr, nullptr, nullptr,
                                       CRYPTPROTECT_LOCAL_MACHINE | CRYPTPROTECT_UI_FORBIDDEN,
                                       &out);
    if (!ok) {
        if (errorOut) *errorOut = lastErrorMessage();
        return false;
    }
    const QByteArray blob(reinterpret_cast<const char *>(out.pbData),
                          static_cast<int>(out.cbData));
    ::LocalFree(out.pbData);
    saveBlob(account, blob);
    return true;
}

bool CredentialStore::platformClear(const QString &account, QString *errorOut)
{
    Q_UNUSED(errorOut);
    removeBlob(account);
    return true;
}

} // namespace ai
