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

#ifndef REMOTE_SSHPROFILEREGISTRY_H
#define REMOTE_SSHPROFILEREGISTRY_H

#include <QList>
#include <QObject>
#include <QString>

#include "SshProfile.h"

class QSettings;

namespace remote {

// Persists SSH connection profiles in the `Ssh/Profiles` settings array via
// QSettings beginWriteArray/beginReadArray (pattern: AcpAgentRegistry, but an
// array rather than a JSON blob). Secrets are NOT here — keychain only. Takes a
// QSettings* (ApplicationSettings derives from QSettings) so tests can drive it
// with a plain QSettings and no app instance.
class SshProfileRegistry : public QObject
{
    Q_OBJECT

public:
    explicit SshProfileRegistry(QSettings *settings, QObject *parent = nullptr);

    // Mint a slash-free unique id (so it composes into the ssh:// URI cleanly).
    static QString generateId();

    QList<SshProfile> profiles() const { return m_profiles; }
    bool contains(const QString &id) const;
    SshProfile profile(const QString &id) const;

    bool addProfile(const SshProfile &p);
    bool updateProfile(const SshProfile &p);
    bool removeProfile(const QString &id);

signals:
    void changed();

private:
    void load();
    void persist();

    QSettings *m_settings;
    QList<SshProfile> m_profiles;
};

} // namespace remote

#endif // REMOTE_SSHPROFILEREGISTRY_H
