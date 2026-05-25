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

#include "TerminalTaskRegistry.h"
#include "ApplicationSettings.h"

#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

TerminalTaskRegistry::TerminalTaskRegistry(ApplicationSettings *settings)
    : m_settings(settings)
{
}

QString TerminalTaskRegistry::normalizeWorkspacePath(const QString &path)
{
#ifdef Q_OS_WIN
    return QDir::cleanPath(path).toLower();
#else
    return QDir::cleanPath(path);
#endif
}

QList<TerminalTask> TerminalTaskRegistry::tasksForWorkspace(const QString &workspacePath) const
{
    if (!m_settings || workspacePath.isEmpty())
        return {};

    const QString raw = m_settings->workspaceTasksJson();
    if (raw.isEmpty())
        return {};

    const QJsonDocument doc = QJsonDocument::fromJson(raw.toUtf8());
    if (!doc.isObject())
        return {};

    const QString key = normalizeWorkspacePath(workspacePath);
    const QJsonArray arr = doc.object().value(key).toArray();
    QList<TerminalTask> result;
    result.reserve(arr.size());
    for (const QJsonValue &v : arr) {
        const QJsonObject obj = v.toObject();
        const QString name    = obj.value(QStringLiteral("name")).toString();
        const QString command = obj.value(QStringLiteral("command")).toString();
        if (!command.isEmpty())
            result.append({name.isEmpty() ? command : name, command});
    }
    return result;
}

void TerminalTaskRegistry::addTask(const QString &workspacePath, const TerminalTask &task)
{
    if (!m_settings || workspacePath.isEmpty() || task.command.isEmpty())
        return;

    const QString raw = m_settings->workspaceTasksJson();
    QJsonObject root;
    if (!raw.isEmpty()) {
        const QJsonDocument doc = QJsonDocument::fromJson(raw.toUtf8());
        if (doc.isObject())
            root = doc.object();
    }

    const QString key = normalizeWorkspacePath(workspacePath);
    QJsonArray arr = root.value(key).toArray();
    QJsonObject entry;
    entry.insert(QStringLiteral("name"),    task.name.isEmpty() ? task.command : task.name);
    entry.insert(QStringLiteral("command"), task.command);
    arr.append(entry);
    root.insert(key, arr);

    m_settings->setWorkspaceTasksJson(
        QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Compact)));
}
