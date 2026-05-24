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

#ifndef TERMINALDOCK_H
#define TERMINALDOCK_H

#include <QDockWidget>
#include <QString>

class QToolButton;
class TerminalWidget;

class TerminalDock : public QDockWidget
{
    Q_OBJECT

public:
    TerminalDock(const QString &shell, const QString &cwd, QWidget *parent = nullptr);
    TerminalDock(const QString &shell, const QString &cwd, const QString &taskCommand, const QString &taskName, QWidget *parent = nullptr);
    ~TerminalDock() override;

    TerminalWidget *terminalWidget() const { return m_terminal; }
    bool isTask() const { return !m_taskCommand.isEmpty(); }

protected:
    void closeEvent(QCloseEvent *event) override;

private:
    void init(const QString &shell, const QString &cwd);
    void setupTaskTitleBar();
    void restartTask();

    TerminalWidget *m_terminal = nullptr;
    QString m_initialCwd;
    QString m_shell;
    QString m_taskCommand;
    QString m_taskName;
    QToolButton *m_restartBtn = nullptr;
};

#endif
