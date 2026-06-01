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

#include "ExecutionContextRegistry.h"

#include "LocalExecutionContext.h"
#include "RemoteExecutionContext.h"
#include "SshConnection.h"
#include "SshProfile.h"
#include "SshProfileRegistry.h"

namespace remote {

ExecutionContextRegistry::ExecutionContextRegistry(SshProfileRegistry *profiles,
                                                   ai::CredentialStore *credentialStore,
                                                   QObject *parent)
    : QObject(parent)
    , m_profiles(profiles)
    , m_credentialStore(credentialStore)
    , m_local(new LocalExecutionContext(this))
{
}

ExecutionContextRegistry::~ExecutionContextRegistry()
{
    // Tear down each remote connection (worker thread joins in
    // ~SshConnection). Contexts are children of this and clean up after.
    for (auto it = m_remotes.begin(); it != m_remotes.end(); ++it) {
        delete it.value().connection;
    }
    m_remotes.clear();
}

RemoteExecutionContext *ExecutionContextRegistry::remoteContext(const QString &profileId) const
{
    return m_remotes.value(profileId).context;
}

RemoteExecutionContext *ExecutionContextRegistry::connect(const QString &profileId)
{
    if (auto existing = m_remotes.value(profileId).context) {
        return existing;
    }
    if (!m_profiles || !m_profiles->contains(profileId)) {
        return nullptr;
    }
    const SshProfile profile = m_profiles->profile(profileId);

    Entry entry;
    // The connection is parented to this registry; the worker thread joins in
    // ~SshConnection. The context is also a child of this registry.
    entry.connection = new SshConnection(profile, m_credentialStore, this);
    entry.context = new RemoteExecutionContext(profile, entry.connection, this);
    m_remotes.insert(profileId, entry);

    emit remoteContextCreated(profileId, entry.context);

    entry.connection->connectToHost();
    return entry.context;
}

void ExecutionContextRegistry::disconnect(const QString &profileId)
{
    auto it = m_remotes.find(profileId);
    if (it == m_remotes.end()) {
        return;
    }
    Entry entry = it.value();
    m_remotes.erase(it);
    // Delete the context first (it only observes the connection), then the
    // connection (joins its worker thread in its destructor).
    delete entry.context;
    delete entry.connection;
}

} // namespace remote
