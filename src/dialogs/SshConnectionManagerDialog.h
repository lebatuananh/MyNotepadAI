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

#ifndef SSHCONNECTIONMANAGERDIALOG_H
#define SSHCONNECTIONMANAGERDIALOG_H

#include <QDialog>

#include "remote/SshProfile.h"

class QListWidget;
class QLineEdit;
class QSpinBox;
class QComboBox;
class QPushButton;
class QLabel;

namespace ai { class CredentialStore; }
namespace remote { class SshProfileRegistry; }

// CRUD for SSH connection profiles: a list on the left, an edit form on the
// right (host/port/user/auth method/key path + secret), New/Delete, and a
// Connect button that emits connectRequested(profileId). Secrets are written to
// the OS keychain via CredentialStore (never to settings). Built in code
// (matches SendWithGoalDialog / EditTasksDialog), palette-driven, full keyboard
// nav (Tab order + Enter/Esc via the button box).
class SshConnectionManagerDialog : public QDialog
{
    Q_OBJECT

public:
    SshConnectionManagerDialog(remote::SshProfileRegistry *registry,
                               ai::CredentialStore *credentialStore,
                               QWidget *parent = nullptr);

signals:
    // The user picked a profile and clicked Connect. The caller drives the
    // staged connect (SshConnectDialog) — the manager only owns CRUD.
    void connectRequested(const QString &profileId);

private:
    void rebuildList(const QString &selectId = QString());
    void loadSelectionIntoForm();
    void saveFormToProfile();
    void onNew();
    void onDelete();
    void onConnect();
    void setFormEnabled(bool on);
    void updateAuthRows();
    remote::SshProfile currentFormProfile() const;
    QString selectedProfileId() const;

    remote::SshProfileRegistry *m_registry;
    ai::CredentialStore *m_credentialStore;

    QListWidget *m_list = nullptr;
    QLineEdit *m_hostEdit = nullptr;
    QSpinBox *m_portSpin = nullptr;
    QLineEdit *m_userEdit = nullptr;
    QComboBox *m_authCombo = nullptr;
    QLineEdit *m_keyPathEdit = nullptr;
    QPushButton *m_keyBrowseBtn = nullptr;
    QLineEdit *m_secretEdit = nullptr;   // password or passphrase (kind depends on auth)
    QLabel *m_secretLabel = nullptr;
    QLineEdit *m_lastPathEdit = nullptr;
    QPushButton *m_saveBtn = nullptr;
    QPushButton *m_deleteBtn = nullptr;
    QPushButton *m_connectBtn = nullptr;
    QLabel *m_emptyHint = nullptr;

    QString m_editingId; // id of the profile currently shown in the form
};

#endif // SSHCONNECTIONMANAGERDIALOG_H
