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

#ifndef REMOTE_SSHHOSTKEYSTORE_H
#define REMOTE_SSHHOSTKEYSTORE_H

#include <QByteArray>
#include <QString>

namespace remote {

// App-managed known_hosts. Deliberately NOT the system ~/.ssh/known_hosts —
// NotepadADE keeps its own store (a JSON file under the app data dir) so it
// never mutates the user's OpenSSH config and the host-key UX is fully under
// the app's control. First connect presents the SHA256 fingerprint for
// Accept/Reject; on accept add() persists the raw key blob keyed by host:port.
// A later mismatch (isChanged) triggers a loud MITM warning.
//
// All methods are synchronous file ops; the store is tiny so this is fine.
class SshHostKeyStore
{
public:
    SshHostKeyStore() = default;

    // Override the on-disk path (tests). Empty → default app-data location.
    explicit SshHostKeyStore(const QString &filePath) : m_filePath(filePath) {}

    // Returns the stored raw host-key blob for host:port, or empty if unknown.
    QByteArray lookup(const QString &host, int port) const;

    // True if host:port is known AND the stored blob differs from `key`
    // (the MITM / changed-key condition). False when unknown or matching.
    bool isChanged(const QString &host, int port, const QByteArray &key) const;

    // Persist (or replace) the host-key blob for host:port.
    void add(const QString &host, int port, const QByteArray &key);

    // Remove a stored key (e.g. user re-accepts after a deliberate rekey).
    void remove(const QString &host, int port);

    // SHA256 fingerprint formatting of a raw host-key blob, as shown to the
    // user, e.g. "SHA256:abcd…=" (base64, no padding stripped). Static + pure.
    static QString sha256Fingerprint(const QByteArray &key);

private:
    QString storePath() const;
    static QString hostKeyId(const QString &host, int port);

    QString m_filePath; // empty → default app-data path
};

} // namespace remote

#endif // REMOTE_SSHHOSTKEYSTORE_H
