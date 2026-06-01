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

#include "SshProfileRegistry.h"

#include <QSettings>
#include <QUuid>

namespace remote {

namespace {
constexpr auto kArrayKey = "Ssh/Profiles";
}

QString SshProfileRegistry::generateId()
{
    // RFC-4122 without braces/dashes → slash-free, composes into ssh:// URI.
    return QUuid::createUuid().toString(QUuid::Id128);
}

SshProfileRegistry::SshProfileRegistry(QSettings *settings, QObject *parent)
    : QObject(parent)
    , m_settings(settings)
{
    load();
}

void SshProfileRegistry::load()
{
    m_profiles.clear();
    if (!m_settings) {
        return;
    }

    const int n = m_settings->beginReadArray(QLatin1String(kArrayKey));
    m_profiles.reserve(n);
    for (int i = 0; i < n; ++i) {
        m_settings->setArrayIndex(i);
        SshProfile p;
        p.id = m_settings->value(QStringLiteral("id")).toString();
        if (p.id.isEmpty()) {
            continue;
        }
        p.host = m_settings->value(QStringLiteral("host")).toString();
        p.port = m_settings->value(QStringLiteral("port"), 22).toInt();
        p.username = m_settings->value(QStringLiteral("username")).toString();
        p.authMethod = authMethodFromString(
            m_settings->value(QStringLiteral("authMethod")).toString());
        p.keyPath = m_settings->value(QStringLiteral("keyPath")).toString();
        p.lastRemotePath = m_settings->value(QStringLiteral("lastRemotePath")).toString();
        p.lastConnectedMs =
            m_settings->value(QStringLiteral("lastConnectedMs"), 0).toLongLong();

        // Skip duplicate ids (defensive against a hand-edited settings file).
        bool dup = false;
        for (const SshProfile &existing : m_profiles) {
            if (existing.id == p.id) { dup = true; break; }
        }
        if (!dup) {
            m_profiles.append(p);
        }
    }
    m_settings->endArray();
}

void SshProfileRegistry::persist()
{
    if (!m_settings) {
        return;
    }
    // beginWriteArray truncates the array group, so a removed tail entry never
    // lingers.
    m_settings->beginWriteArray(QLatin1String(kArrayKey), m_profiles.size());
    for (int i = 0; i < m_profiles.size(); ++i) {
        const SshProfile &p = m_profiles.at(i);
        m_settings->setArrayIndex(i);
        m_settings->setValue(QStringLiteral("id"), p.id);
        m_settings->setValue(QStringLiteral("host"), p.host);
        m_settings->setValue(QStringLiteral("port"), p.port);
        m_settings->setValue(QStringLiteral("username"), p.username);
        m_settings->setValue(QStringLiteral("authMethod"), authMethodToString(p.authMethod));
        m_settings->setValue(QStringLiteral("keyPath"), p.keyPath);
        m_settings->setValue(QStringLiteral("lastRemotePath"), p.lastRemotePath);
        m_settings->setValue(QStringLiteral("lastConnectedMs"), p.lastConnectedMs);
    }
    m_settings->endArray();
}

bool SshProfileRegistry::contains(const QString &id) const
{
    for (const SshProfile &p : m_profiles) {
        if (p.id == id) return true;
    }
    return false;
}

SshProfile SshProfileRegistry::profile(const QString &id) const
{
    for (const SshProfile &p : m_profiles) {
        if (p.id == id) return p;
    }
    return SshProfile{};
}

bool SshProfileRegistry::addProfile(const SshProfile &p)
{
    if (p.id.isEmpty() || contains(p.id)) {
        return false;
    }
    m_profiles.append(p);
    persist();
    emit changed();
    return true;
}

bool SshProfileRegistry::updateProfile(const SshProfile &p)
{
    if (p.id.isEmpty()) {
        return false;
    }
    for (int i = 0; i < m_profiles.size(); ++i) {
        if (m_profiles[i].id == p.id) {
            m_profiles[i] = p;
            persist();
            emit changed();
            return true;
        }
    }
    return false;
}

bool SshProfileRegistry::removeProfile(const QString &id)
{
    for (int i = 0; i < m_profiles.size(); ++i) {
        if (m_profiles[i].id == id) {
            m_profiles.removeAt(i);
            persist();
            emit changed();
            return true;
        }
    }
    return false;
}

} // namespace remote
