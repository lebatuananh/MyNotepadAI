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

#ifndef REMOTE_EXECUTIONCONTEXTREGISTRY_H
#define REMOTE_EXECUTIONCONTEXTREGISTRY_H

#include <QHash>
#include <QObject>
#include <QString>

namespace ai { class CredentialStore; }

namespace remote {

class ExecutionContext;
class LocalExecutionContext;
class RemoteExecutionContext;
class SshConnection;
class SshProfileRegistry;

// Owned by NotepadNextApplication (like aiAgentManager_). Holds one shared
// LocalExecutionContext and at most one RemoteExecutionContext per connected
// profile. MainWindow::activeExecutionContext() consults this:
//   active SSH workspace → that profile's remote context, else local.
class ExecutionContextRegistry : public QObject
{
    Q_OBJECT

public:
    ExecutionContextRegistry(SshProfileRegistry *profiles,
                             ai::CredentialStore *credentialStore,
                             QObject *parent = nullptr);
    ~ExecutionContextRegistry() override;

    LocalExecutionContext *localContext() const { return m_local; }

    // The remote context for a profile, or nullptr if not yet connected.
    RemoteExecutionContext *remoteContext(const QString &profileId) const;

    // Begin (or return existing) a connection for profileId. Creates the
    // SshConnection + RemoteExecutionContext on first call and starts the
    // staged connect. Returns the context (never null for a known profile),
    // or nullptr if the profile id is unknown.
    RemoteExecutionContext *connect(const QString &profileId);

    // Tear down a profile's connection + context (e.g. last SSH workspace
    // closed). Safe to call when none exists.
    void disconnect(const QString &profileId);

signals:
    void remoteContextCreated(const QString &profileId, remote::RemoteExecutionContext *ctx);

private:
    struct Entry
    {
        SshConnection *connection = nullptr;
        RemoteExecutionContext *context = nullptr;
    };

    SshProfileRegistry *m_profiles;
    ai::CredentialStore *m_credentialStore;
    LocalExecutionContext *m_local = nullptr;
    QHash<QString, Entry> m_remotes;
};

} // namespace remote

#endif // REMOTE_EXECUTIONCONTEXTREGISTRY_H
