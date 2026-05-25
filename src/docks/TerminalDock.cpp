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

#include "TerminalDock.h"
#include "TerminalWidget.h"

#include <QCloseEvent>
#include <QDir>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QToolButton>
#include <QWidget>

TerminalDock::TerminalDock(const QString &shell, const QString &cwd, QWidget *parent)
    : QDockWidget(parent)
    , m_initialCwd(cwd)
    , m_shell(shell)
{
    init(shell, cwd);
}

TerminalDock::TerminalDock(const QString &shell, const QString &cwd, const QString &taskCommand, const QString &taskName, const QStringList &env, QWidget *parent)
    : QDockWidget(parent)
    , m_initialCwd(cwd)
    , m_shell(shell)
    , m_taskCommand(taskCommand)
    , m_taskName(taskName.isEmpty() ? taskCommand : taskName)
    , m_taskEnv(env)
{
    init(shell, cwd);
    setupTaskTitleBar();
    setWindowTitle(tr("Task — %1").arg(m_taskName));
}

void TerminalDock::init(const QString &shell, const QString &cwd)
{
    setAttribute(Qt::WA_DeleteOnClose, true);
    setObjectName(QStringLiteral("TerminalDock_%1").arg(reinterpret_cast<qulonglong>(this), 0, 16));

    m_terminal = new TerminalWidget(this);
    setWidget(m_terminal);

    if (m_taskCommand.isEmpty()) {
        QString cwdBasename = QFileInfo(QDir::cleanPath(cwd)).fileName();
        if (cwdBasename.isEmpty()) cwdBasename = cwd;
        setWindowTitle(tr("Terminal — %1").arg(cwdBasename));

        connect(m_terminal, &TerminalWidget::titleChanged, this, [this](const QString &t) {
            if (!t.isEmpty()) {
                setWindowTitle(t);
            }
        });
    }

    connect(m_terminal, &TerminalWidget::spawnFailed, this, [this](const QString &msg) {
        QMessageBox::critical(this, tr("Terminal"), msg);
    });

    m_terminal->start(shell, cwd, m_taskEnv);

    if (!m_taskCommand.isEmpty()) {
        connect(m_terminal, &TerminalWidget::firstOutputReceived, this, [this]() {
            if (!m_cwdWarning.isEmpty()) {
                QByteArray warning = QStringLiteral("\x1b[33m⚠ %1\x1b[0m\r\n")
                    .arg(m_cwdWarning).toUtf8();
                m_terminal->injectOutput(warning);
            }
            QByteArray cmd = m_taskCommand.toUtf8();
            cmd.append('\r');
            m_terminal->writeToPty(cmd);
        }, Qt::SingleShotConnection);
    }
}

TerminalDock::~TerminalDock() = default;

void TerminalDock::setupTaskTitleBar()
{
    auto *titleBar = new QWidget(this);
    auto *layout = new QHBoxLayout(titleBar);
    layout->setContentsMargins(4, 2, 4, 2);
    layout->setSpacing(4);

    auto *label = new QLabel(m_taskName, titleBar);
    label->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    layout->addWidget(label);

    m_restartBtn = new QToolButton(titleBar);
    m_restartBtn->setText(tr("Restart"));
    m_restartBtn->setToolTip(tr("Restart task"));
    connect(m_restartBtn, &QToolButton::clicked, this, &TerminalDock::restartTask);
    layout->addWidget(m_restartBtn);

    setTitleBarWidget(titleBar);
}

void TerminalDock::restartTask()
{
    if (!m_terminal) return;

    m_terminal->killProcess();
    delete m_terminal;

    m_terminal = new TerminalWidget(this);
    setWidget(m_terminal);

    connect(m_terminal, &TerminalWidget::spawnFailed, this, [this](const QString &msg) {
        QMessageBox::critical(this, tr("Terminal"), msg);
    });

    m_terminal->start(m_shell, m_initialCwd, m_taskEnv);

    connect(m_terminal, &TerminalWidget::firstOutputReceived, this, [this]() {
        if (!m_cwdWarning.isEmpty()) {
            QByteArray warning = QStringLiteral("\x1b[33m⚠ %1\x1b[0m\r\n")
                .arg(m_cwdWarning).toUtf8();
            m_terminal->injectOutput(warning);
        }
        QByteArray cmd = m_taskCommand.toUtf8();
        cmd.append('\r');
        m_terminal->writeToPty(cmd);
    }, Qt::SingleShotConnection);

    m_terminal->setFocus();
}

void TerminalDock::closeEvent(QCloseEvent *event)
{
    if (m_terminal && m_terminal->isProcessRunning()) {
        QMessageBox::StandardButton answer = QMessageBox::question(
            this,
            tr("Close terminal?"),
            tr("A process is still running in this terminal. Close anyway?"),
            QMessageBox::Yes | QMessageBox::No,
            QMessageBox::No);
        if (answer != QMessageBox::Yes) {
            event->ignore();
            return;
        }
        m_terminal->killProcess();
    }
    QDockWidget::closeEvent(event);
}
