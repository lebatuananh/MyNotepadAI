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

#ifndef EDIT_SCHEDULED_TASK_DIALOG_H
#define EDIT_SCHEDULED_TASK_DIALOG_H

#include <QDialog>

#include "ScheduledTaskDefinition.h"

class AcpAgentRegistry;
class ApplicationSettings;
class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;
class QTimer;

class EditScheduledTaskDialog : public QDialog
{
    Q_OBJECT

public:
    explicit EditScheduledTaskDialog(const ScheduledTaskDefinition &def,
                                     AcpAgentRegistry *agentRegistry,
                                     ApplicationSettings *settings,
                                     const QStringList &recentWorkspaces = {},
                                     QWidget *parent = nullptr);

    ScheduledTaskDefinition taskResult() const;

    static bool editTask(ScheduledTaskDefinition &def,
                         AcpAgentRegistry *agentRegistry,
                         ApplicationSettings *settings,
                         QWidget *parent,
                         const QStringList &recentWorkspaces = {});

private slots:
    void onCronTextChanged();
    void onCronHelperClicked();
    void onBrowseWorkspace();
    void validate();

private:
    void updateCronPreview();

    AcpAgentRegistry *m_agentRegistry;
    ApplicationSettings *m_settings;
    ScheduledTaskDefinition m_original;

    QLineEdit *m_nameEdit;
    QLineEdit *m_cronEdit;
    QPushButton *m_cronHelperBtn;
    QLabel *m_cronPreviewLabel;
    QTimer *m_cronDebounce;
    QComboBox *m_agentCombo;
    QComboBox *m_workspaceCombo;
    QPushButton *m_browseBtn;
    QPlainTextEdit *m_promptEdit;
    QCheckBox *m_skipIfRunningCheck;
    QSpinBox *m_timeoutSpin;

    // Goal section
    QCheckBox *m_goalCheck;
    QPushButton *m_goalConfigBtn;
    ScheduledTaskGoalConfig m_goalConfigData;

    QPushButton *m_saveBtn;
};

#endif // EDIT_SCHEDULED_TASK_DIALOG_H
