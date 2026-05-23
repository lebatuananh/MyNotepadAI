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

#include "FolderAsWorkspaceFsModel.h"

#include "../git/GitDiffPalette.h"
#include "../git/GitStatusEntry.h"
#include "../git/PathStatusIndex.h"

#include <QDir>
#include <QFileInfo>

namespace {

QColor colourForChange(GitStatusEntry::Change c, const GitDiffPalette &p)
{
    switch (c) {
    case GitStatusEntry::Unmerged:    return p.fgConflict;
    case GitStatusEntry::Deleted:     return p.fgDeleted;
    case GitStatusEntry::Modified:    return p.fgModified;
    case GitStatusEntry::TypeChanged: return p.fgModified;
    case GitStatusEntry::Added:       return p.fgAdded;
    case GitStatusEntry::Renamed:     return p.fgRenamed;
    case GitStatusEntry::Copied:      return p.fgRenamed;
    case GitStatusEntry::Untracked_:  return p.fgUntracked;
    }
    return {};
}

} // namespace

QVariant FolderAsWorkspaceFsModel::data(const QModelIndex &index, int role) const
{
    if (role == Qt::ToolTipRole && index.isValid()) {
        return QDir::toNativeSeparators(filePath(index));
    }

    if (role == Qt::ForegroundRole
            && m_colorsEnabled
            && m_statusIndex != nullptr
            && index.isValid()) {
        const QString cleanPath = QDir::cleanPath(filePath(index));
        const auto change = isDir(index)
                ? m_statusIndex->folderChange(cleanPath)
                : m_statusIndex->fileChange(cleanPath);
        if (change.has_value()) {
            const QColor c = colourForChange(*change, GitDiffPalette::current(m_isDark));
            if (c.isValid()) return c;
        }
        // Falls through to base — system default text colour.
    }

    return QFileSystemModel::data(index, role);
}

void FolderAsWorkspaceFsModel::setStatusIndex(const PathStatusIndex *idx)
{
    m_statusIndex = idx;
}

void FolderAsWorkspaceFsModel::setColorsEnabled(bool enabled)
{
    if (m_colorsEnabled == enabled) return;
    m_colorsEnabled = enabled;
    if (m_statusIndex != nullptr) {
        notifyPathsChanged(m_statusIndex->allIndexedPaths());
    }
}

void FolderAsWorkspaceFsModel::setDarkPalette(bool isDark)
{
    if (m_isDark == isDark) return;
    m_isDark = isDark;
    if (m_colorsEnabled && m_statusIndex != nullptr) {
        notifyPathsChanged(m_statusIndex->allIndexedPaths());
    }
}

void FolderAsWorkspaceFsModel::notifyPathsChanged(const QSet<QString> &cleanPaths)
{
    if (cleanPaths.isEmpty()) return;
    const QList<int> roles{ Qt::ForegroundRole };
    for (const QString &p : cleanPaths) {
        // QFileSystemModel::index(path) returns invalid for paths the model
        // has not yet loaded into its node cache. We skip those — the next
        // user-driven expand will trigger a fresh data() call and the
        // decoration will appear naturally.
        const QModelIndex idx = index(p, 0);
        if (!idx.isValid()) continue;
        emit dataChanged(idx, idx, roles);
    }
}
