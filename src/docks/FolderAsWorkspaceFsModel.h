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

#ifndef FOLDER_AS_WORKSPACE_FS_MODEL_H
#define FOLDER_AS_WORKSPACE_FS_MODEL_H

#include <QFileSystemModel>
#include <QSet>
#include <QString>

class PathStatusIndex;
struct GitDiffPalette;

// Subclass of QFileSystemModel powering the Files tab of FolderAsWorkspaceDock.
// Two responsibilities beyond the base class:
//
//   1) Native-separator tooltips on hover (existing behaviour).
//   2) Git decoration colours for file + folder rows, driven by an externally-
//      owned PathStatusIndex pointer and a light/dark palette flag.
//
// The model never owns the PathStatusIndex — the dock owns it and must call
// setStatusIndex(nullptr) before the index is destroyed.
class FolderAsWorkspaceFsModel : public QFileSystemModel
{
    Q_OBJECT

public:
    using QFileSystemModel::QFileSystemModel;

    QVariant data(const QModelIndex &index, int role) const override;

    // Decoration plumbing. Pure setters; the dock controls when to emit
    // dataChanged via notifyPathsChanged.
    void setStatusIndex(const PathStatusIndex *idx);
    void setColorsEnabled(bool enabled);
    void setDarkPalette(bool isDark);

    bool colorsEnabled() const { return m_colorsEnabled; }
    bool isDarkPalette() const { return m_isDark; }

    // Emit dataChanged(ForegroundRole) for each cleanPath whose row is
    // currently loaded into the model. Unloaded paths are skipped — when the
    // user later expands their subtree, QFileSystemModel re-queries data()
    // and the decoration appears.
    void notifyPathsChanged(const QSet<QString> &cleanPaths);

private:
    const PathStatusIndex *m_statusIndex = nullptr;
    bool m_colorsEnabled = true;
    bool m_isDark = false;
};

#endif // FOLDER_AS_WORKSPACE_FS_MODEL_H
