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

namespace Ui {
class FolderAsWorkspaceDock;
}

class QFileSystemModel;
class QTimer;

class FolderAsWorkspaceDock : public QDockWidget
{
    Q_OBJECT

public:
    explicit FolderAsWorkspaceDock(QWidget *parent = nullptr);
    FolderAsWorkspaceDock(const QString &initialPath, QWidget *parent);
    ~FolderAsWorkspaceDock();

    void setRootPath(const QString dir);
    QString rootPath() const;

signals:
    void fileDoubleClicked(const QString &filePath);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    Ui::FolderAsWorkspaceDock *ui;

    QFileSystemModel *model;

    QTimer *tooltipTimer;
    QPersistentModelIndex pendingTooltipIndex;
};

#endif // FOLDERASWORKSPACEDOCK_H
