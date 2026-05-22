/*
 * This file is part of Notepad Next.
 * Copyright 2022 Justin Dailey
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


#ifndef FOLDERASWORKSPACEDOCK_H
#define FOLDERASWORKSPACEDOCK_H

#include <QDockWidget>
#include <QPersistentModelIndex>

#include "GitStatusEntry.h"

namespace Ui {
class FolderAsWorkspaceDock;
}

class QFileSystemModel;
class QTimer;
class GitTabWidget;
class GitDiffViewController;
class ScintillaNext;

class FolderAsWorkspaceDock : public QDockWidget
{
    Q_OBJECT

public:
    explicit FolderAsWorkspaceDock(QWidget *parent = nullptr);
    FolderAsWorkspaceDock(const QString &initialPath, QWidget *parent);
    ~FolderAsWorkspaceDock();

    void setRootPath(const QString dir);
    QString rootPath() const;

    // Returns the lazily-created Git tab, or nullptr if the user has never
    // opened the Git tab in this dock yet.
    GitTabWidget *gitTabWidget() const { return gitTab; }

    // Forwards to the per-dock GitDiffViewController, creating it on first use.
    // Host (MainWindow) calls this in response to gitDiffRequested.
    void showGitDiffPreview(const GitStatusEntry &entry);

    // Switch the tab widget to the Git page and lazy-construct it if needed.
    // Used when a workspace is opened via a path that implies "show me Git first"
    // (e.g. clicking a submodule in another workspace's Git status tree).
    void showGitTab();

signals:
    void fileDoubleClicked(const QString &filePath);
    // Forwarded from GitTabWidget once the Git tab is created.
    void gitDiffRequested(const GitStatusEntry &entry);
    void gitOpenSubmoduleRequested(const QString &absPath);
    // Forwarded from the per-dock GitDiffViewController — host raises this
    // editor as the active tab so the user lands on the rendered diff.
    void gitDiffPreviewRendered(ScintillaNext *editor);
    void gitDiffPreviewFailed(const QString &relPath, const QString &message);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private slots:
    void onTabChanged(int index);

private:
    Ui::FolderAsWorkspaceDock *ui;

    QFileSystemModel *model;
    GitTabWidget *gitTab = nullptr;
    GitDiffViewController *gitDiffViewController = nullptr;

    QTimer *tooltipTimer;
    QPersistentModelIndex pendingTooltipIndex;

    void ensureGitTab();
    GitDiffViewController *ensureGitDiffViewController();
};

#endif // FOLDERASWORKSPACEDOCK_H
