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

#ifndef REMOTE_IFILESYSTEMBACKEND_H
#define REMOTE_IFILESYSTEMBACKEND_H

#include <QByteArray>
#include <QDateTime>
#include <QObject>
#include <QString>
#include <QStringList>

namespace remote {

// Filesystem-access seam used by the workspace file tree and the editor's
// open/save path. Phase 1 only provides a local (QFile-backed) implementation
// so the seam is real and exercised with zero regression; the remote
// (SFTP-backed) implementation is Phase 2 — RemoteExecutionContext::fsBackend()
// returns nullptr with a `// TODO P2` until then.
//
// Kept deliberately small: stat / read / write / list + a directoryChanged
// signal. Anything richer (rename, mkdir, watch granularity) is added when the
// Phase 2 remote tree needs it.
struct FileStat
{
    bool      exists = false;
    bool      isDir = false;
    qint64    size = 0;
    QDateTime lastModified;
};

class IFileSystemBackend : public QObject
{
    Q_OBJECT

public:
    explicit IFileSystemBackend(QObject *parent = nullptr) : QObject(parent) {}
    ~IFileSystemBackend() override = default;

    // Synchronous read/write/stat. `ok` (when non-null) reports success vs an
    // I/O error (as opposed to a simply-absent file, which is not an error for
    // stat()). Remote (P2) implementations may add async variants alongside.
    virtual QByteArray readFile(const QString &path, bool *ok = nullptr) = 0;
    virtual bool writeFile(const QString &path, const QByteArray &data) = 0;
    virtual FileStat stat(const QString &path) = 0;
    virtual QStringList readdir(const QString &path) = 0;

signals:
    // Emitted when a watched directory's contents change. Local backend wires
    // this to a filesystem watcher; remote (P2) derives it from SFTP polling
    // or server notifications.
    void directoryChanged(const QString &path);
};

} // namespace remote

#endif // REMOTE_IFILESYSTEMBACKEND_H
