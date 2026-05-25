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

#pragma once

#include <QList>
#include <QString>

class ApplicationSettings;

struct TerminalTask
{
    QString name;
    QString command;
    QString env; // raw .env text (KEY=VALUE per line, # comments, blank lines)
    QString cwd; // relative to workspace root, or absolute; empty = workspace root
};

// Pure task-persistence layer. No UI, no PTY, no MainWindow dependency.
// Stores tasks per workspace path in ApplicationSettings under
// Terminal/WorkspaceTasks as a JSON object: { "<normalizedPath>": [...] }.
class TerminalTaskRegistry
{
public:
    explicit TerminalTaskRegistry(ApplicationSettings *settings);

    // Normalize a workspace path to a stable JSON key:
    //   - QDir::cleanPath: separator → '/', strip trailing slash
    //   - toLower on Windows: D:/foo and d:/foo collide into the same entry
    static QString normalizeWorkspacePath(const QString &path);

    QList<TerminalTask> tasksForWorkspace(const QString &workspacePath) const;
    void addTask(const QString &workspacePath, const TerminalTask &task);
    void setTasks(const QString &workspacePath, const QList<TerminalTask> &tasks);

private:
    ApplicationSettings *m_settings;
};
