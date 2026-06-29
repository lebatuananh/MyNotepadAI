/*
 * This file is part of Notepad Next.
 * Copyright 2019 Justin Dailey
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


#ifndef RECENTFILESLISTMANAGER_H
#define RECENTFILESLISTMANAGER_H

#include <QObject>
#include <QList>

class RecentFilesListManager : public QObject
{
    Q_OBJECT

public:
    // maxFiles caps the list size; older entries past the cap are evicted on
    // addFile(). Defaults to 10 so existing callers (recent files) are
    // unchanged; the recent-workspaces list passes a larger cap.
    explicit RecentFilesListManager(QObject *parent = Q_NULLPTR, int maxFiles = 10);

    QString mostRecentFile() const;
    QStringList fileList() const;
    void setFileList(const QStringList &list);

    int count() const { return recentFiles.size(); }
    int maxFiles() const { return m_maxFiles; }

public slots:
    void addFile(const QString &filePath);
    void removeFile(const QString &filePath);
    void clear();

private:
    QStringList recentFiles;
    int m_maxFiles;
};

#endif // RECENTFILESLISTMANAGER_H
