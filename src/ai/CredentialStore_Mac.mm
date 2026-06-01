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

// macOS Security.framework / Keychain Services backend. Stores the API key
// under service="NotepadAI", account="commit-message-api-key" as a generic
// password. The system keychain handles encryption/entropy.

#include "CredentialStore.h"

#include <QString>

#import <Security/Security.h>
#import <CoreFoundation/CoreFoundation.h>

namespace ai {

namespace {

constexpr auto kService = "NotepadAI";

CFStringRef cfStringFromUtf8(const char *str)
{
    return CFStringCreateWithCString(kCFAllocatorDefault, str, kCFStringEncodingUTF8);
}

QString cfErrorMessage(OSStatus status)
{
    CFStringRef msg = SecCopyErrorMessageString(status, nullptr);
    if (!msg) return QStringLiteral("Keychain error %1").arg(int(status));
    char buf[512] = {0};
    CFStringGetCString(msg, buf, sizeof(buf), kCFStringEncodingUTF8);
    CFRelease(msg);
    return QString::fromUtf8(buf);
}

} // namespace

bool CredentialStore::platformAvailable() const { return true; }

bool CredentialStore::platformRetrieve(const QString &account, QString *outValue, QString *errorOut) const
{
    CFStringRef svc = cfStringFromUtf8(kService);
    CFStringRef acc = cfStringFromUtf8(account.toUtf8().constData());

    const void *keys[]   = { kSecClass, kSecAttrService, kSecAttrAccount, kSecReturnData, kSecMatchLimit };
    const void *values[] = { kSecClassGenericPassword, svc, acc, kCFBooleanTrue, kSecMatchLimitOne };
    CFDictionaryRef query = CFDictionaryCreate(kCFAllocatorDefault, keys, values, 5,
                                               &kCFTypeDictionaryKeyCallBacks,
                                               &kCFTypeDictionaryValueCallBacks);

    CFTypeRef result = nullptr;
    const OSStatus status = SecItemCopyMatching(query, &result);
    CFRelease(query);
    CFRelease(svc);
    CFRelease(acc);

    if (status == errSecItemNotFound) {
        if (outValue) outValue->clear();
        return true;
    }
    if (status != errSecSuccess) {
        if (errorOut) *errorOut = cfErrorMessage(status);
        if (outValue) outValue->clear();
        return false;
    }
    if (result && CFGetTypeID(result) == CFDataGetTypeID()) {
        CFDataRef data = (CFDataRef)result;
        const CFIndex len = CFDataGetLength(data);
        const UInt8 *bytes = CFDataGetBytePtr(data);
        if (outValue) *outValue = QString::fromUtf8(reinterpret_cast<const char *>(bytes), int(len));
        CFRelease(result);
        return true;
    }
    if (result) CFRelease(result);
    if (outValue) outValue->clear();
    return true;
}

bool CredentialStore::platformStore(const QString &account, const QString &value, QString *errorOut)
{
    CFStringRef svc = cfStringFromUtf8(kService);
    CFStringRef acc = cfStringFromUtf8(account.toUtf8().constData());
    const QByteArray utf8 = value.toUtf8();
    CFDataRef data = CFDataCreate(kCFAllocatorDefault,
                                  reinterpret_cast<const UInt8 *>(utf8.constData()),
                                  utf8.size());

    // Try update first; if not found, add.
    {
        const void *qkeys[]   = { kSecClass, kSecAttrService, kSecAttrAccount };
        const void *qvalues[] = { kSecClassGenericPassword, svc, acc };
        CFDictionaryRef query = CFDictionaryCreate(kCFAllocatorDefault, qkeys, qvalues, 3,
                                                   &kCFTypeDictionaryKeyCallBacks,
                                                   &kCFTypeDictionaryValueCallBacks);
        const void *ukeys[]   = { kSecValueData };
        const void *uvalues[] = { data };
        CFDictionaryRef update = CFDictionaryCreate(kCFAllocatorDefault, ukeys, uvalues, 1,
                                                    &kCFTypeDictionaryKeyCallBacks,
                                                    &kCFTypeDictionaryValueCallBacks);
        const OSStatus st = SecItemUpdate(query, update);
        CFRelease(update);
        CFRelease(query);
        if (st == errSecSuccess) {
            CFRelease(data); CFRelease(svc); CFRelease(acc);
            return true;
        }
        if (st != errSecItemNotFound) {
            if (errorOut) *errorOut = cfErrorMessage(st);
            CFRelease(data); CFRelease(svc); CFRelease(acc);
            return false;
        }
    }

    const void *akeys[]   = { kSecClass, kSecAttrService, kSecAttrAccount, kSecValueData };
    const void *avalues[] = { kSecClassGenericPassword, svc, acc, data };
    CFDictionaryRef add = CFDictionaryCreate(kCFAllocatorDefault, akeys, avalues, 4,
                                             &kCFTypeDictionaryKeyCallBacks,
                                             &kCFTypeDictionaryValueCallBacks);
    const OSStatus st = SecItemAdd(add, nullptr);
    CFRelease(add);
    CFRelease(data);
    CFRelease(svc);
    CFRelease(acc);
    if (st != errSecSuccess) {
        if (errorOut) *errorOut = cfErrorMessage(st);
        return false;
    }
    return true;
}

bool CredentialStore::platformClear(const QString &account, QString *errorOut)
{
    CFStringRef svc = cfStringFromUtf8(kService);
    CFStringRef acc = cfStringFromUtf8(account.toUtf8().constData());
    const void *keys[]   = { kSecClass, kSecAttrService, kSecAttrAccount };
    const void *values[] = { kSecClassGenericPassword, svc, acc };
    CFDictionaryRef query = CFDictionaryCreate(kCFAllocatorDefault, keys, values, 3,
                                               &kCFTypeDictionaryKeyCallBacks,
                                               &kCFTypeDictionaryValueCallBacks);
    const OSStatus st = SecItemDelete(query);
    CFRelease(query);
    CFRelease(svc);
    CFRelease(acc);
    if (st != errSecSuccess && st != errSecItemNotFound) {
        if (errorOut) *errorOut = cfErrorMessage(st);
        return false;
    }
    return true;
}

} // namespace ai
