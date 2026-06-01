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

#include "SshHostKeyStore.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>

namespace remote {

QString SshHostKeyStore::hostKeyId(const QString &host, int port)
{
    return host + QLatin1Char(':') + QString::number(port);
}

QString SshHostKeyStore::storePath() const
{
    if (!m_filePath.isEmpty()) {
        return m_filePath;
    }
    QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (dir.isEmpty()) {
        dir = QDir::homePath();
    }
    QDir().mkpath(dir);
    return dir + QStringLiteral("/ssh_known_hosts.json");
}

QString SshHostKeyStore::sha256Fingerprint(const QByteArray &key)
{
    const QByteArray digest = QCryptographicHash::hash(key, QCryptographicHash::Sha256);
    // OpenSSH-style: base64 of the raw digest, with trailing '=' padding
    // stripped, prefixed with "SHA256:".
    QByteArray b64 = digest.toBase64();
    while (b64.endsWith('=')) {
        b64.chop(1);
    }
    return QStringLiteral("SHA256:") + QString::fromLatin1(b64);
}

QByteArray SshHostKeyStore::lookup(const QString &host, int port) const
{
    QFile f(storePath());
    if (!f.open(QIODevice::ReadOnly)) {
        return {};
    }
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
    f.close();
    if (!doc.isObject()) {
        return {};
    }
    const QString stored = doc.object().value(hostKeyId(host, port)).toString();
    if (stored.isEmpty()) {
        return {};
    }
    return QByteArray::fromBase64(stored.toLatin1());
}

bool SshHostKeyStore::isChanged(const QString &host, int port, const QByteArray &key) const
{
    const QByteArray known = lookup(host, port);
    if (known.isEmpty()) {
        return false; // unknown host is not "changed"
    }
    return known != key;
}

void SshHostKeyStore::add(const QString &host, int port, const QByteArray &key)
{
    QJsonObject root;
    {
        QFile f(storePath());
        if (f.open(QIODevice::ReadOnly)) {
            const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
            if (doc.isObject()) {
                root = doc.object();
            }
            f.close();
        }
    }
    root.insert(hostKeyId(host, port), QString::fromLatin1(key.toBase64()));

    QFile f(storePath());
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        f.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
        f.close();
    }
}

void SshHostKeyStore::remove(const QString &host, int port)
{
    QFile f(storePath());
    if (!f.open(QIODevice::ReadOnly)) {
        return;
    }
    QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
    f.close();
    if (!doc.isObject()) {
        return;
    }
    QJsonObject root = doc.object();
    root.remove(hostKeyId(host, port));

    QFile out(storePath());
    if (out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        out.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
        out.close();
    }
}

} // namespace remote
