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

#include <QDialog>
#include <QIcon>
#include <QString>
#include <QVector>

#include <memory>

#include "FileIndexCache.h"

class QLineEdit;
class QListView;
class RecentWorkspaceModel;

// JetBrains-style "Recent Workspace" picker: a frameless popup with a search
// field and a flat, fuzzy-filterable list. Each row carries three parallel,
// index-aligned facets (kept distinct because they differ):
//   - matchText : what fuzzy search runs against (host:path or native path)
//   - displayText : what the row shows (same as matchText today, but kept
//                   separate so the open target never leaks into display)
//   - openUri   : the exact string handed back to openFolderAsWorkspacePath
//                 (an ssh:// URI for remote, an absolute path for local)
// plus decoration (icon + connection status) that is NOT fuzzy-searchable.
//
// The caller (MainWindow) owns all knowledge of SSH profiles / connection
// state, so it builds the entries and passes them in; this dialog only does
// matching, rendering, and selection. Empty query → entries in their given
// (MRU) order; non-empty → QuickFileOpenDialog::computeMatches over a
// FileIndexCache built from the match strings.
class RecentWorkspaceOpenDialog : public QDialog
{
    Q_OBJECT

public:
    struct Entry
    {
        QString matchText;     // fuzzy target + display string
        QString openUri;       // passed verbatim to openFolderAsWorkspacePath
        QString statusText;    // e.g. "connected" / "disconnected"; "" = none
        QIcon icon;            // leading badge; null = none
    };

    explicit RecentWorkspaceOpenDialog(QVector<Entry> entries, QWidget *parent = nullptr);
    ~RecentWorkspaceOpenDialog() override;

    // The openUri of the chosen row, or empty if the dialog was dismissed.
    QString selectedUri() const { return m_selectedUri; }

protected:
    bool eventFilter(QObject *obj, QEvent *event) override;
    void showEvent(QShowEvent *event) override;

private slots:
    void onTextChanged(const QString &text);
    void onItemActivated(const QModelIndex &index);

private:
    void applyFilter(const QString &pattern);

    QVector<Entry> m_entries;
    std::shared_ptr<const FileIndexCache> m_index;   // built from match strings

    QLineEdit *m_lineEdit = nullptr;
    QListView *m_listView = nullptr;
    RecentWorkspaceModel *m_model = nullptr;

    QString m_selectedUri;
};
